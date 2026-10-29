#include "net.h"

#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS sets SO_NOSIGPIPE on the socket instead.
#endif
#endif

namespace net {

namespace {

#ifdef _WIN32
using Native = SOCKET;
int lastError() { return WSAGetLastError(); }
bool interrupted() { return false; }
#else
using Native = int;
int lastError() { return errno; }
bool interrupted() { return errno == EINTR; }
#endif

Native native(Handle handle) { return static_cast<Native>(handle); }

std::string describe(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(lastError()) + ")";
}

}  // namespace

Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, invalidHandle)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, invalidHandle);
    }
    return *this;
}

bool Socket::valid() const { return handle_ != invalidHandle; }

void Socket::close() {
    if (!valid()) return;
#ifdef _WIN32
    closesocket(native(handle_));
#else
    ::close(native(handle_));
#endif
    handle_ = invalidHandle;
}

bool Socket::sendAll(const void* data, std::size_t size) {
    auto bytes = static_cast<const char*>(data);
    while (size > 0) {
#ifdef _WIN32
        int sent = ::send(native(handle_), bytes, static_cast<int>(size), 0);
#else
        ssize_t sent = ::send(native(handle_), bytes, size, MSG_NOSIGNAL);
#endif
        if (sent < 0 && interrupted()) continue;
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool Socket::readExact(void* data, std::size_t size) {
    auto bytes = static_cast<char*>(data);
    while (size > 0) {
#ifdef _WIN32
        int got = ::recv(native(handle_), bytes, static_cast<int>(size), 0);
#else
        ssize_t got = ::recv(native(handle_), bytes, size, 0);
#endif
        if (got < 0 && interrupted()) continue;
        if (got <= 0) return false;
        bytes += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

long Socket::readSome(void* data, std::size_t size, std::chrono::milliseconds timeout) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(native(handle_), &readable);
    timeval wait{};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>(timeout.count() % 1000 * 1000);
    int ready = ::select(static_cast<int>(native(handle_)) + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0) return interrupted() ? 0 : -1;
    if (ready == 0) return 0;
#ifdef _WIN32
    int got = ::recv(native(handle_), static_cast<char*>(data), static_cast<int>(size), 0);
#else
    ssize_t got = ::recv(native(handle_), data, size, 0);
#endif
    if (got < 0 && interrupted()) return 0;
    return got > 0 ? static_cast<long>(got) : -1;
}

void Socket::setTimeouts(std::chrono::milliseconds timeout) {
#ifdef _WIN32
    DWORD value = static_cast<DWORD>(timeout.count());
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>(timeout.count() % 1000 * 1000);
#endif
    setsockopt(native(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof value);
    setsockopt(native(handle_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof value);
}

void startup() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

Socket connectTcp(const char* host, std::uint16_t port, std::string& error) {
    Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Socket socket(static_cast<Handle>(s));
    if (!socket.valid()) {
        error = describe("socket");
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, host, &address.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        error = describe("connect");
        return {};
    }
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
    return socket;
}

Socket connectUnix(const char* path, std::string& error) {
#ifdef _WIN32
    (void)path;
    error = "Unix sockets aren't used on Windows";
    return {};
#else
    Native s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    Socket socket(static_cast<Handle>(s));
    if (!socket.valid()) {
        error = describe("socket");
        return {};
    }
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof address.sun_path - 1);
    if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        error = describe("connect");
        return {};
    }
    return socket;
#endif
}

}  // namespace net
