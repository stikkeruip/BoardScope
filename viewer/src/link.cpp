#include "link.h"

#include <chrono>

#include "usbmux.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto retryInterval = 500ms;
constexpr auto pingInterval = 500ms;
/// The phone answers every ping, so this much silence means the link is gone.
constexpr auto silenceLimit = 2s;

std::uint64_t microsecondsNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

#ifdef _WIN32
constexpr const char* noServiceAdvice =
    "Apple Mobile Device Service isn't running. Install Apple Devices from the Microsoft Store, or iTunes.";
#else
constexpr const char* noServiceAdvice = "Apple's USB service (usbmuxd) isn't running.";
#endif

}  // namespace

PhoneLink::PhoneLink() : thread_([this] { run(); }) {}

PhoneLink::~PhoneLink() {
    running_ = false;
    thread_.join();
}

PhoneLink::Snapshot PhoneLink::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

bool PhoneLink::takeFrame(std::vector<std::uint8_t>& jpeg) {
    std::lock_guard lock(mutex_);
    if (!hasNewFrame_) return false;
    jpeg.swap(frame_);
    hasNewFrame_ = false;
    return true;
}

std::uint32_t PhoneLink::send(wire::Command command) {
    std::lock_guard lock(sendMutex_);
    if (!socket_.valid()) return 0;
    command.sequence = nextSequence_++;
    std::vector<std::uint8_t> message = wire::encode(command);
    // A failed write closes the link from the reading side soon after.
    return socket_.sendAll(message.data(), message.size()) ? command.sequence : 0;
}

bool PhoneLink::sendMessage(const std::vector<std::uint8_t>& message) {
    std::lock_guard lock(sendMutex_);
    return socket_.valid() && socket_.sendAll(message.data(), message.size());
}

void PhoneLink::setStatus(Status status, std::string problem) {
    std::lock_guard lock(mutex_);
    snapshot_.status = status;
    snapshot_.problem = std::move(problem);
}

void PhoneLink::run() {
    net::startup();
    while (running_) {
        if (connect()) serve();
        for (auto waited = 0ms; running_ && waited < retryInterval; waited += 50ms) {
            std::this_thread::sleep_for(50ms);
        }
    }
}

bool PhoneLink::connect() {
    std::string error;
    std::vector<usbmux::Device> devices;
    switch (usbmux::listDevices(devices, error)) {
    case usbmux::Result::ok:
        break;
    case usbmux::Result::noService:
        setStatus(Status::noService, noServiceAdvice);
        return false;
    case usbmux::Result::noDevice:
        setStatus(Status::noPhone, "Connect the iPhone with its USB cable, unlock it, and tap Trust if it asks.");
        return false;
    default:
        setStatus(Status::noService, error);
        return false;
    }

    for (const usbmux::Device& device : devices) {
        net::Socket socket;
        usbmux::Result result = usbmux::connect(device, wire::port, socket, error);
        if (result == usbmux::Result::refused) {
            setStatus(Status::noApp, "iPhone found. Open BoardScope on it.");
            continue;
        }
        if (result != usbmux::Result::ok) {
            setStatus(Status::noPhone, error);
            continue;
        }
        socket.setTimeouts(1s);
        {
            std::lock_guard lock(sendMutex_);
            socket_ = std::move(socket);
            nextSequence_ = 1;
        }
        std::lock_guard lock(mutex_);
        snapshot_.status = Status::connected;
        snapshot_.problem.clear();
        snapshot_.serial = device.serial;
        snapshot_.state.reset();
        snapshot_.roundTripMs = -1;
        snapshot_.receivedFps = 0;
        ++snapshot_.connection;
        return true;
    }
    return false;
}

/// Reads messages until the link drops, pinging the phone twice a second.
void PhoneLink::serve() {
    std::vector<std::uint8_t> inbox;
    std::vector<std::uint8_t> chunk(256 * 1024);
    auto lastHeard = Clock::now();
    auto lastPing = Clock::time_point{};
    auto fpsStart = Clock::now();
    int fpsFrames = 0;
    std::string problem = "The link to the iPhone dropped. Reconnecting...";

    while (running_) {
        auto now = Clock::now();
        if (now - lastPing >= pingInterval) {
            wire::Writer ping;
            std::uint64_t sent = microsecondsNow();
            ping.u32(static_cast<std::uint32_t>(sent >> 32));
            ping.u32(static_cast<std::uint32_t>(sent));
            if (!sendMessage(wire::message(wire::Type::ping, ping.bytes.data(), ping.bytes.size()))) break;
            lastPing = now;
        }
        if (now - lastHeard > silenceLimit) {
            problem = "The iPhone stopped answering. Reconnecting...";
            break;
        }

        long got = socket_.readSome(chunk.data(), chunk.size(), 50ms);
        if (got < 0) break;
        if (got == 0) continue;
        lastHeard = Clock::now();
        inbox.insert(inbox.end(), chunk.begin(), chunk.begin() + got);

        std::size_t used = 0;
        bool broken = false;
        while (inbox.size() - used >= wire::headerSize) {
            wire::Reader header(inbox.data() + used, wire::headerSize);
            std::uint8_t type;
            std::uint32_t length;
            header.u8(type);
            header.u32(length);
            if (length > wire::maxPayload) {
                broken = true;
                break;
            }
            if (inbox.size() - used - wire::headerSize < length) break;
            const std::uint8_t* payload = inbox.data() + used + wire::headerSize;
            if (static_cast<wire::Type>(type) == wire::Type::frame) ++fpsFrames;
            handle(static_cast<wire::Type>(type), payload, length);
            used += wire::headerSize + length;
        }
        if (broken) {
            problem = "The iPhone sent something unreadable. Reconnecting...";
            break;
        }
        inbox.erase(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(used));

        double seconds = std::chrono::duration<double>(Clock::now() - fpsStart).count();
        if (seconds >= 1) {
            std::lock_guard lock(mutex_);
            snapshot_.receivedFps = fpsFrames / seconds;
            fpsFrames = 0;
            fpsStart = Clock::now();
        }
    }

    {
        std::lock_guard lock(sendMutex_);
        socket_.close();
    }
    if (running_) setStatus(Status::searching, problem);
}

void PhoneLink::handle(wire::Type type, const std::uint8_t* payload, std::size_t size) {
    switch (type) {
    case wire::Type::frame: {
        // Acknowledge first, so the phone can start on the next frame while this one is drawn.
        sendMessage(wire::message(wire::Type::frameAck, nullptr, 0));
        std::lock_guard lock(mutex_);
        frame_.assign(payload, payload + size);
        hasNewFrame_ = true;
        snapshot_.frameBytes = size;
        break;
    }
    case wire::Type::state:
        if (auto state = wire::decodeState(payload, size)) {
            std::lock_guard lock(mutex_);
            snapshot_.state = state;
        }
        break;
    case wire::Type::ping:
        sendMessage(wire::message(wire::Type::pong, payload, size));
        break;
    case wire::Type::pong: {
        wire::Reader reader(payload, size);
        std::uint32_t high, low;
        if (!reader.u32(high) || !reader.u32(low)) break;
        std::uint64_t sent = static_cast<std::uint64_t>(high) << 32 | low;
        double ms = static_cast<double>(microsecondsNow() - sent) / 1000.0;
        std::lock_guard lock(mutex_);
        snapshot_.roundTripMs = snapshot_.roundTripMs < 0 ? ms : snapshot_.roundTripMs * 0.7 + ms * 0.3;
        break;
    }
    default:
        break;
    }
}
