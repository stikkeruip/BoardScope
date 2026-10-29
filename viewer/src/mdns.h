#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

/// Just enough multicast DNS (Bonjour) to find BoardScope on the local network.
namespace mdns {

struct Service {
    /// The instance name, usually the phone's name.
    std::string name;
    std::string address;
    std::uint16_t port = 0;
};

/// Asks the local network for instances of `type` (such as "_boardscope._tcp") and collects the
/// answers that arrive within `wait`. Needs no Bonjour service on the computer itself.
std::vector<Service> browse(const char* type, std::chrono::milliseconds wait);

}  // namespace mdns
