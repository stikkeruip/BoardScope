#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net.h"
#include "wire.h"

/// Keeps a connection to BoardScope on the iPhone over USB, on its own thread. It reconnects on
/// its own when the link drops, keeps only the newest frame, and measures the round trip.
class PhoneLink {
public:
    enum class Status { searching, noService, noPhone, noApp, connected };

    struct Snapshot {
        Status status = Status::searching;
        /// What's wrong and what to do about it, when not connected.
        std::string problem;
        std::string serial;
        std::optional<wire::State> state;
        /// Counts up with every new connection, so the UI can forget changes it was waiting on.
        std::uint64_t connection = 0;
        double roundTripMs = -1;
        double receivedFps = 0;
        std::size_t frameBytes = 0;
    };

    PhoneLink();
    ~PhoneLink();
    PhoneLink(const PhoneLink&) = delete;
    PhoneLink& operator=(const PhoneLink&) = delete;

    Snapshot snapshot() const;

    /// Moves the newest frame into `jpeg` if one arrived since the last call; frames that were
    /// overtaken before the UI took them are dropped.
    bool takeFrame(std::vector<std::uint8_t>& jpeg);

    /// Sends a command with the next sequence number and returns that number, or 0 when not connected.
    std::uint32_t send(wire::Command command);

private:
    void run();
    bool connect();
    void serve();
    void handle(wire::Type type, const std::uint8_t* payload, std::size_t size);
    bool sendMessage(const std::vector<std::uint8_t>& message);
    void setStatus(Status status, std::string problem);

    std::atomic<bool> running_{true};

    mutable std::mutex mutex_;  // guards everything below up to the send mutex
    Snapshot snapshot_;
    std::vector<std::uint8_t> frame_;
    bool hasNewFrame_ = false;

    std::mutex sendMutex_;  // guards the socket for writing, and the sequence number
    net::Socket socket_;
    std::uint32_t nextSequence_ = 1;

    std::thread thread_;
};
