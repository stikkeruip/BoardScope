#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net.h"

/// A client for Apple's USB multiplexer, which tunnels TCP connections to apps on an iPhone
/// over its cable. It runs as usbmuxd on macOS and Linux, and as Apple Mobile Device Service
/// (installed with Apple Devices or iTunes) on Windows.
namespace usbmux {

struct Device {
    int id = 0;
    std::string serial;
};

enum class Result {
    ok,
    /// Apple's USB service isn't running.
    noService,
    /// No iPhone is plugged in, or it doesn't trust this computer yet.
    noDevice,
    /// The iPhone is there but nothing listens on the port: the app isn't open.
    refused,
    /// The service answered with something unexpected.
    failed,
};

/// iPhones connected by cable. Phones that are only reachable over Wi-Fi are left out.
Result listDevices(std::vector<Device>& devices, std::string& error);

/// Opens a TCP connection to `port` on the iPhone, tunneled over USB.
Result connect(const Device& device, std::uint16_t port, net::Socket& socket, std::string& error);

}  // namespace usbmux
