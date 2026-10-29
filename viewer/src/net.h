#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

#ifdef _WIN32
using Handle = std::uintptr_t;
#else
using Handle = int;
#endif

/// A connected stream socket, closed when destroyed.
class Socket {
public:
    Socket() = default;
    explicit Socket(Handle handle) : handle_(handle) {}
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    bool valid() const;
    void close();

    /// Sends all of `data`; false if the connection broke.
    bool sendAll(const void* data, std::size_t size);
    /// Reads exactly `size` bytes; false on close, error or timeout.
    bool readExact(void* data, std::size_t size);
    /// Waits up to `timeout` for data and reads what is there.
    /// Returns the byte count, 0 on timeout, or -1 once the connection is closed or broken.
    long readSome(void* data, std::size_t size, std::chrono::milliseconds timeout);
    /// Makes blocking sends and reads give up after `timeout`.
    void setTimeouts(std::chrono::milliseconds timeout);

private:
    Handle handle_ = invalidHandle;
#ifdef _WIN32
    static constexpr Handle invalidHandle = ~Handle(0);
#else
    static constexpr Handle invalidHandle = -1;
#endif
};

/// Starts the socket library on Windows; does nothing elsewhere.
void startup();
/// Connects to `host` (a name or IPv4 address), giving up after `timeout`.
Socket connectTcp(const char* host, std::uint16_t port, std::string& error,
                  std::chrono::milliseconds timeout = std::chrono::seconds(3));
Socket connectUnix(const char* path, std::string& error);

}  // namespace net
