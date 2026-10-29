#include "mdns.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mdns {

namespace {

#ifdef _WIN32
using Native = SOCKET;
const Native invalidSocket = INVALID_SOCKET;
void closeSocket(Native s) { closesocket(s); }
using TtlOption = DWORD;
#else
using Native = int;
const Native invalidSocket = -1;
void closeSocket(Native s) { ::close(s); }
using TtlOption = unsigned char;
#endif

constexpr std::uint16_t typeA = 1;
constexpr std::uint16_t typePtr = 12;
constexpr std::uint16_t typeSrv = 33;

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool linkLocal(const std::string& address) { return address.rfind("169.254.", 0) == 0; }

/// Reads a DNS message, following name compression pointers.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool u16(std::size_t& at, std::uint16_t& value) const {
        if (at + 2 > size_) return false;
        value = static_cast<std::uint16_t>(data_[at] << 8 | data_[at + 1]);
        at += 2;
        return true;
    }

    bool name(std::size_t& at, std::string& out) const {
        out.clear();
        std::size_t position = at;
        bool jumped = false;
        for (int jumps = 0;;) {
            if (position >= size_) return false;
            std::uint8_t length = data_[position];
            if ((length & 0xC0) == 0xC0) {
                if (position + 1 >= size_ || ++jumps > 16) return false;
                if (!jumped) at = position + 2;
                jumped = true;
                position = static_cast<std::size_t>(length & 0x3F) << 8 | data_[position + 1];
                continue;
            }
            if (length == 0) {
                if (!jumped) at = position + 1;
                return true;
            }
            if (position + 1 + length > size_) return false;
            if (!out.empty()) out += '.';
            out.append(reinterpret_cast<const char*>(data_ + position + 1), length);
            position += 1 + length;
        }
    }

    const std::uint8_t* data() const { return data_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
};

struct Instance {
    std::string host;
    std::uint16_t port = 0;
    /// Where the answer came from, in case it names no address.
    std::string source;
};

/// Collects what one response says about instances of `service` and the addresses of their hosts.
void read(const std::uint8_t* data, std::size_t size, const std::string& source, const std::string& service,
          std::map<std::string, Instance>& instances, std::map<std::string, std::string>& addresses) {
    Reader reader(data, size);
    std::size_t at = 0;
    std::uint16_t id, flags, questions, answers, authorities, additionals;
    if (!reader.u16(at, id) || !reader.u16(at, flags) || !reader.u16(at, questions) || !reader.u16(at, answers) ||
        !reader.u16(at, authorities) || !reader.u16(at, additionals) || !(flags & 0x8000)) {
        return;
    }
    std::string name;
    for (int i = 0; i < questions; ++i) {
        if (!reader.name(at, name)) return;
        at += 4;
    }
    for (int i = 0; i < answers + authorities + additionals; ++i) {
        std::uint16_t type, rrClass, length;
        if (!reader.name(at, name) || !reader.u16(at, type) || !reader.u16(at, rrClass)) return;
        at += 4;  // time to live
        if (!reader.u16(at, length) || at + length > size) return;
        std::size_t next = at + length;
        std::string owner = lowercase(name);
        if (type == typePtr && owner == service) {
            std::string instance;
            if (reader.name(at, instance)) {
                Instance& found = instances[instance];
                if (found.source.empty() || linkLocal(found.source)) found.source = source;
            }
        } else if (type == typeSrv && owner.size() > service.size() && owner.ends_with("." + service)) {
            std::uint16_t priority, weight, port;
            std::string host;
            if (reader.u16(at, priority) && reader.u16(at, weight) && reader.u16(at, port) && reader.name(at, host)) {
                for (auto& [instance, found] : instances) {
                    if (lowercase(instance) == owner) {
                        found.host = lowercase(host);
                        found.port = port;
                    }
                }
            }
        } else if (type == typeA && length == 4) {
            const std::uint8_t* ip = reader.data() + at;
            std::string address = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
            std::string& known = addresses[owner];
            // The phone also answers over its USB network link; the Wi-Fi address is the one wanted.
            if (known.empty() || linkLocal(known)) known = address;
        }
        at = next;
    }
}

}  // namespace

std::vector<Service> browse(const char* type, std::chrono::milliseconds wait) {
    std::string service = lowercase(std::string(type) + ".local");
    Native s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == invalidSocket) return {};
    // From an ordinary port, not 5353: responders then answer straight back to this socket.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    ::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof local);
    TtlOption ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);

    std::vector<std::uint8_t> query;
    auto put16 = [&query](std::uint16_t value) {
        query.push_back(static_cast<std::uint8_t>(value >> 8));
        query.push_back(static_cast<std::uint8_t>(value));
    };
    put16(static_cast<std::uint16_t>(std::random_device{}()));
    put16(0);  // flags: a standard query
    put16(1);  // one question
    put16(0);
    put16(0);
    put16(0);
    for (std::size_t start = 0; start < service.size();) {
        std::size_t end = std::min(service.find('.', start), service.size());
        query.push_back(static_cast<std::uint8_t>(end - start));
        query.insert(query.end(), service.begin() + static_cast<std::ptrdiff_t>(start), service.begin() + static_cast<std::ptrdiff_t>(end));
        start = end + 1;
    }
    query.push_back(0);
    put16(typePtr);
    put16(0x8001);  // class IN, asking for a unicast answer

    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);

    std::map<std::string, Instance> instances;
    std::map<std::string, std::string> addresses;
    auto start = std::chrono::steady_clock::now();
    bool askedAgain = false;
    auto ask = [&] {
        ::sendto(s, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                 reinterpret_cast<sockaddr*>(&group), sizeof group);
    };
    ask();
    for (;;) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (elapsed >= wait) break;
        // Multicast can get lost on Wi-Fi, so ask once more halfway through.
        if (!askedAgain && elapsed >= wait / 2) {
            ask();
            askedAgain = true;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval pause{0, 50 * 1000};
        if (::select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &pause) <= 0) continue;
        std::uint8_t packet[9000];
        sockaddr_in from{};
        socklen_t fromLength = sizeof from;
        auto got = ::recvfrom(s, reinterpret_cast<char*>(packet), sizeof packet, 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (got <= 0) continue;
        char source[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, source, sizeof source);
        read(packet, static_cast<std::size_t>(got), source, service, instances, addresses);
    }
    closeSocket(s);

    std::vector<Service> services;
    for (const auto& [instance, found] : instances) {
        Service entry;
        entry.name = instance.substr(0, instance.size() - std::min(instance.size(), service.size() + 1));
        auto address = addresses.find(found.host);
        entry.address = address != addresses.end() && !linkLocal(address->second) ? address->second : found.source;
        entry.port = found.port;
        if (!entry.address.empty() && !linkLocal(entry.address)) services.push_back(entry);
    }
    return services;
}

}  // namespace mdns
