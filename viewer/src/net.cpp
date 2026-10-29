#include "net.h"

#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
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
bool connecting() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void setBlocking(Native s, bool blocking) {
    u_long nonBlocking = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
}
#else
using Native = int;
int lastError() { return errno; }
bool interrupted() { return errno == EINTR; }
bool connecting() { return errno == EINPROGRESS; }
void setBlocking(Native s, bool blocking) {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}
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

Socket connectTcp(const char* host, std::uint16_t port, std::string& error, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host, std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        error = std::string("Can't find ") + host;
        return {};
    }
    sockaddr_in address;
    std::memcpy(&address, found->ai_addr, sizeof address);
    freeaddrinfo(found);

    Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Socket socket(static_cast<Handle>(s));
    if (!socket.valid()) {
        error = describe("socket");
        return {};
    }
    // Connect without blocking, so an address that doesn't answer gives up after `timeout`.
    setBlocking(s, false);
    if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        if (!connecting()) {
            error = describe("connect");
            return {};
        }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        timeval wait{};
        wait.tv_sec = static_cast<long>(timeout.count() / 1000);
        wait.tv_usec = static_cast<long>(timeout.count() % 1000 * 1000);
        int problem = 0;
        socklen_t length = sizeof problem;
        if (::select(static_cast<int>(s) + 1, nullptr, &writable, nullptr, &wait) <= 0) {
            error = std::string("No answer from ") + host;
            return {};
        }
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&problem), &length);
        if (problem != 0) {
            error = std::string("Couldn't connect to ") + host + " (error " + std::to_string(problem) + ")";
            return {};
        }
    }
    setBlocking(s, true);
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
