#include "usbmux.h"

#include "plist.h"

namespace usbmux {

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t protocolVersion = 1;  // XML property lists
constexpr std::uint32_t plistMessage = 8;

net::Socket openService(std::string& error) {
#ifdef _WIN32
    net::Socket socket = net::connectTcp("127.0.0.1", 27015, error);
#else
    net::Socket socket = net::connectUnix("/var/run/usbmuxd", error);
#endif
    if (socket.valid()) socket.setTimeouts(3s);
    return socket;
}

void putLittleEndian(std::uint8_t* out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t getLittleEndian(const std::uint8_t* in) {
    return in[0] | in[1] << 8 | in[2] << 16 | static_cast<std::uint32_t>(in[3]) << 24;
}

/// Sends one request and reads the reply. Every message has a 16-byte header:
/// total length, protocol version, message type and a tag, each 32-bit little-endian.
bool exchange(net::Socket& socket, const plist::Dict& request, plist::Value& reply, std::string& error) {
    static std::uint32_t tag = 0;
    std::string body = request.xml();
    std::uint8_t header[16];
    putLittleEndian(header, static_cast<std::uint32_t>(sizeof header + body.size()));
    putLittleEndian(header + 4, protocolVersion);
    putLittleEndian(header + 8, plistMessage);
    putLittleEndian(header + 12, ++tag);
    if (!socket.sendAll(header, sizeof header) || !socket.sendAll(body.data(), body.size())) {
        error = "Couldn't write to Apple's USB service";
        return false;
    }
    if (!socket.readExact(header, sizeof header)) {
        error = "No answer from Apple's USB service";
        return false;
    }
    std::uint32_t length = getLittleEndian(header);
    if (length < sizeof header || length > 4 * 1024 * 1024) {
        error = "Apple's USB service sent a malformed reply";
        return false;
    }
    std::string replyBody(length - sizeof header, '\0');
    if (!socket.readExact(replyBody.data(), replyBody.size()) || !plist::parse(replyBody, reply)) {
        error = "Apple's USB service sent a reply that couldn't be read";
        return false;
    }
    return true;
}

plist::Dict request(const char* messageType) {
    plist::Dict dict;
    dict.strings = {
        {"MessageType", messageType},
        {"ProgName", "BoardScope"},
        {"ClientVersionString", "BoardScope viewer 2"},
        {"BundleID", "com.uipko.boardscope.viewer"},
    };
    dict.integers = {{"kLibUSBMuxVersion", 3}};
    return dict;
}

}  // namespace

Result listDevices(std::vector<Device>& devices, std::string& error) {
    devices.clear();
    net::Socket socket = openService(error);
    if (!socket.valid()) return Result::noService;
    plist::Value reply;
    if (!exchange(socket, request("ListDevices"), reply, error)) return Result::failed;
    const plist::Value* list = reply.find("DeviceList");
    if (!list || list->type != plist::Value::Type::array) {
        error = "Apple's USB service didn't list devices";
        return Result::failed;
    }
    for (const plist::Value& entry : list->items) {
        const plist::Value* properties = entry.find("Properties");
        if (!properties || properties->string("ConnectionType") != "USB") continue;
        Device device;
        device.id = static_cast<int>(properties->integerOr("DeviceID", entry.integerOr("DeviceID", 0)));
        device.serial = properties->string("SerialNumber");
        devices.push_back(device);
    }
    return devices.empty() ? Result::noDevice : Result::ok;
}

Result connect(const Device& device, std::uint16_t port, net::Socket& socket, std::string& error) {
    socket = openService(error);
    if (!socket.valid()) return Result::noService;
    plist::Dict connect = request("Connect");
    // The service expects the port in network byte order, read as a plain integer.
    std::uint16_t swapped = static_cast<std::uint16_t>(port << 8 | port >> 8);
    connect.integers.push_back({"DeviceID", device.id});
    connect.integers.push_back({"PortNumber", swapped});
    plist::Value reply;
    if (!exchange(socket, connect, reply, error)) {
        socket.close();
        return Result::failed;
    }
    std::int64_t number = reply.integerOr("Number", -1);
    if (number == 0) return Result::ok;
    socket.close();
    error = "Apple's USB service answered " + std::to_string(number);
    // 2: the device is gone; 3: the connection was refused.
    return number == 2 ? Result::noDevice : number == 3 ? Result::refused : Result::failed;
}

}  // namespace usbmux
