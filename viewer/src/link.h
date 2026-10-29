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

/// Keeps a connection to BoardScope on the iPhone, over USB or Wi-Fi, on its own thread. It
/// reconnects on its own when the link drops, keeps only the newest frame, and measures the round
/// trip. On Automatic it prefers the cable, and moves back to it from Wi-Fi when it's plugged in.
class PhoneLink {
public:
    enum class Mode { automatic, usb, wifi };
    enum class Transport { usb, wifi };
    enum class Status {
        searching,
        noService,
        noPhone,
        noApp,
        /// Over Wi-Fi the phone wants its pairing code; waits until new options arrive.
        needsCode,
        /// Another viewer connected to the phone; waits until `reconnect()`.
        takenOver,
        connected,
    };

    struct Options {
        Mode mode = Mode::automatic;
        /// Shown in BoardScope's settings on the iPhone; only needed over Wi-Fi.
        std::string pairingCode;
        /// The phone's Wi-Fi address, for networks where it can't be found automatically.
        std::string address;
    };

    struct Snapshot {
        Status status = Status::searching;
        /// What's wrong and what to do about it, when not connected.
        std::string problem;
        Transport transport = Transport::usb;
        /// The phone's serial number over USB, or its name and address over Wi-Fi.
        std::string phone;
        std::optional<wire::State> state;
        /// Counts up with every new connection, so the UI can forget changes it was waiting on.
        std::uint64_t connection = 0;
        double roundTripMs = -1;
        double receivedFps = 0;
        std::size_t frameBytes = 0;
    };

    explicit PhoneLink(Options options);
    ~PhoneLink();
    PhoneLink(const PhoneLink&) = delete;
    PhoneLink& operator=(const PhoneLink&) = delete;

    Snapshot snapshot() const;

    /// Takes effect at the next connection; a new mode reconnects straight away.
    void setOptions(Options options);

    /// Connects again after another viewer took over the phone.
    void reconnect();

    /// Moves the newest frame into `jpeg` if one arrived since the last call; frames that were
    /// overtaken before the UI took them are dropped.
    bool takeFrame(std::vector<std::uint8_t>& jpeg);

    /// Sends a command with the next sequence number and returns that number, or 0 when not connected.
    std::uint32_t send(wire::Command command);

private:
    void run();
    bool connect();
    bool connectUsb(Status& status, std::string& problem);
    bool connectWifi(const Options& options, Status& status, std::string& problem);
    void use(net::Socket socket, Transport transport, std::string phone);
    void serve();
    void handle(wire::Type type, const std::uint8_t* payload, std::size_t size);
    bool sendMessage(const std::vector<std::uint8_t>& message);
    void setStatus(Status status, std::string problem);
    Options options() const;

    std::atomic<bool> running_{true};
    /// Set to end the current connection, such as when the mode changes.
    std::atomic<bool> restart_{false};
    /// Set while waiting for the user (a pairing code, or permission to take over again).
    std::atomic<bool> paused_{false};

    mutable std::mutex mutex_;  // guards everything below up to the send mutex
    Snapshot snapshot_;
    Options options_;
    std::vector<std::uint8_t> frame_;
    bool hasNewFrame_ = false;

    /// A cable connection found while on Wi-Fi, to switch to. Only used on the link thread.
    net::Socket handover_;
    std::string handoverSerial_;

    std::mutex sendMutex_;  // guards the socket for writing, and the sequence number
    net::Socket socket_;
    std::uint32_t nextSequence_ = 1;

    std::thread thread_;
};
