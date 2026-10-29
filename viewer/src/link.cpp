#include "link.h"

#include <chrono>

#include "mdns.h"
#include "usbmux.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto retryInterval = 500ms;
constexpr auto pingInterval = 500ms;
/// The phone answers every ping, so this much silence means the link is gone. Wi-Fi gets more slack.
constexpr auto usbSilenceLimit = 2s;
constexpr auto wifiSilenceLimit = 3s;
/// How long to listen for the phone's Bonjour answer.
constexpr auto browseTime = 800ms;
/// How often a Wi-Fi link checks whether the cable has been plugged in.
constexpr auto usbCheckInterval = 3s;

std::uint64_t microsecondsNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

#ifdef _WIN32
constexpr const char* noServiceAdvice =
    "Apple Mobile Device Service isn't running. Install Apple Devices from the Microsoft Store, or iTunes.";
constexpr const char* noPhoneAnywhere =
    "No iPhone found. Open BoardScope with the phone on this Wi-Fi network, or plug it in "
    "(the cable needs Apple Devices from the Microsoft Store).";
#else
constexpr const char* noServiceAdvice = "Apple's USB service (usbmuxd) isn't running.";
constexpr const char* noPhoneAnywhere =
    "No iPhone found. Plug it in with its cable, or open BoardScope with the phone on this Wi-Fi network.";
#endif
constexpr const char* noUsbPhone = "Connect the iPhone with its USB cable, unlock it, and tap Trust if it asks.";
constexpr const char* noWifiPhone =
    "BoardScope isn't on this Wi-Fi network. Open it on the iPhone, with Viewing over Wi-Fi on in its Settings.";

}  // namespace

PhoneLink::PhoneLink(Options options) : options_(std::move(options)), thread_([this] { run(); }) {}

PhoneLink::~PhoneLink() {
    running_ = false;
    thread_.join();
}

PhoneLink::Snapshot PhoneLink::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

PhoneLink::Options PhoneLink::options() const {
    std::lock_guard lock(mutex_);
    return options_;
}

void PhoneLink::setOptions(Options options) {
    std::lock_guard lock(mutex_);
    if (options.mode != options_.mode) restart_ = true;
    options_ = std::move(options);
    paused_ = false;
}

void PhoneLink::reconnect() { paused_ = false; }

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
    auto lastCableTry = Clock::time_point{};
    while (running_) {
        if (paused_) {
            // Waiting for a pairing code doesn't stop the cable from working.
            Status status;
            std::string problem;
            if (snapshot().status == Status::needsCode && options().mode == Mode::automatic &&
                Clock::now() - lastCableTry >= 1s) {
                lastCableTry = Clock::now();
                if (connectUsb(status, problem)) {
                    paused_ = false;
                    serve();
                    continue;
                }
            }
            std::this_thread::sleep_for(50ms);
            continue;
        }
        if (connect()) serve();
        if (handover_.valid()) continue;  // switch to the cable straight away
        for (auto waited = 0ms; running_ && !restart_ && waited < retryInterval; waited += 50ms) {
            std::this_thread::sleep_for(50ms);
        }
    }
}

bool PhoneLink::connect() {
    restart_ = false;
    if (handover_.valid()) {
        use(std::move(handover_), Transport::usb, handoverSerial_);
        return true;
    }
    Options options = this->options();
    Status status = Status::noPhone;
    std::string problem;
    if (options.mode != Mode::wifi) {
        if (connectUsb(status, problem)) return true;
        // On the cable but with the app closed, the phone can't be on Wi-Fi either.
        if (status == Status::noApp || options.mode == Mode::usb) {
            setStatus(status, problem);
            return false;
        }
    }
    if (options.mode != Mode::usb) {
        if (connectWifi(options, status, problem)) return true;
        if (options.mode == Mode::automatic && status == Status::noPhone && options.address.empty()) {
            problem = noPhoneAnywhere;
        }
    }
    setStatus(status, problem);
    return false;
}

bool PhoneLink::connectUsb(Status& status, std::string& problem) {
    std::string error;
    std::vector<usbmux::Device> devices;
    switch (usbmux::listDevices(devices, error)) {
    case usbmux::Result::ok:
        break;
    case usbmux::Result::noService:
        status = Status::noService;
        problem = noServiceAdvice;
        return false;
    case usbmux::Result::noDevice:
        status = Status::noPhone;
        problem = noUsbPhone;
        return false;
    default:
        status = Status::noService;
        problem = error;
        return false;
    }
    for (const usbmux::Device& device : devices) {
        net::Socket socket;
        usbmux::Result result = usbmux::connect(device, wire::port, socket, error);
        if (result == usbmux::Result::refused) {
            status = Status::noApp;
            problem = "iPhone found. Open BoardScope on it.";
            continue;
        }
        if (result != usbmux::Result::ok) {
            status = Status::noPhone;
            problem = error;
            continue;
        }
        socket.setTimeouts(1s);
        use(std::move(socket), Transport::usb, device.serial);
        return true;
    }
    return false;
}

bool PhoneLink::connectWifi(const Options& options, Status& status, std::string& problem) {
    std::string address = options.address;
    std::string name;
    std::uint16_t port = wire::wifiPort;
    if (address.empty()) {
        std::vector<mdns::Service> found = mdns::browse(wire::bonjourType, browseTime);
        if (found.empty()) {
            status = Status::noPhone;
            problem = noWifiPhone;
            return false;
        }
        address = found.front().address;
        name = found.front().name;
        if (found.front().port) port = found.front().port;
    }
    std::string error;
    net::Socket socket = net::connectTcp(address.c_str(), port, error, 2s);
    if (!socket.valid()) {
        status = Status::noPhone;
        problem = error + ". Is BoardScope open, with Viewing over Wi-Fi on in its Settings?";
        return false;
    }
    socket.setTimeouts(1s);
    // The phone sends nothing until it has the pairing code.
    wire::Writer hello;
    hello.u8(1);
    for (char digit : options.pairingCode) hello.u8(static_cast<std::uint8_t>(digit));
    std::vector<std::uint8_t> message = wire::message(wire::Type::hello, hello.bytes.data(), hello.bytes.size());
    if (!socket.sendAll(message.data(), message.size())) {
        status = Status::noPhone;
        problem = "The iPhone closed the connection.";
        return false;
    }
    use(std::move(socket), Transport::wifi, name.empty() ? address : name + ", " + address);
    return true;
}

void PhoneLink::use(net::Socket socket, Transport transport, std::string phone) {
    {
        std::lock_guard lock(sendMutex_);
        socket_ = std::move(socket);
        nextSequence_ = 1;
    }
    std::lock_guard lock(mutex_);
    snapshot_.status = Status::connected;
    snapshot_.problem.clear();
    snapshot_.transport = transport;
    snapshot_.phone = std::move(phone);
    snapshot_.state.reset();
    snapshot_.roundTripMs = -1;
    snapshot_.receivedFps = 0;
    ++snapshot_.connection;
}

/// Reads messages until the link drops, pinging the phone twice a second.
void PhoneLink::serve() {
    Transport transport = snapshot().transport;
    auto silenceLimit = transport == Transport::usb ? usbSilenceLimit : wifiSilenceLimit;
    std::vector<std::uint8_t> inbox;
    std::vector<std::uint8_t> chunk(256 * 1024);
    auto lastHeard = Clock::now();
    auto lastPing = Clock::time_point{};
    auto lastUsbCheck = Clock::now();
    auto fpsStart = Clock::now();
    int fpsFrames = 0;
    std::optional<wire::Type> farewell;
    std::string problem = "The link to the iPhone dropped. Reconnecting...";

    while (running_ && !restart_ && !farewell) {
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
        if (transport == Transport::wifi && options().mode == Mode::automatic && now - lastUsbCheck >= usbCheckInterval) {
            lastUsbCheck = now;
            std::vector<usbmux::Device> devices;
            std::string error;
            if (usbmux::listDevices(devices, error) == usbmux::Result::ok) {
                for (const usbmux::Device& device : devices) {
                    if (usbmux::connect(device, wire::port, handover_, error) == usbmux::Result::ok) {
                        handover_.setTimeouts(1s);
                        handoverSerial_ = device.serial;
                        break;
                    }
                }
            }
            if (handover_.valid()) {
                problem = "Switching to the USB cable...";
                break;
            }
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
            used += wire::headerSize + length;
            auto messageType = static_cast<wire::Type>(type);
            if (messageType == wire::Type::denied || messageType == wire::Type::takenOver) {
                farewell = messageType;
                break;
            }
            if (messageType == wire::Type::frame) ++fpsFrames;
            handle(messageType, payload, length);
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
    if (farewell == wire::Type::denied) {
        // Wait for the user rather than guessing again; new options end the wait.
        paused_ = true;
        setStatus(Status::needsCode, options().pairingCode.empty()
                                         ? "Enter the pairing code shown in BoardScope's Settings on the iPhone."
                                         : "That pairing code didn't work. Check it in BoardScope's Settings on the iPhone.");
    } else if (farewell == wire::Type::takenOver) {
        // Reconnecting on our own would just take the phone back; the user decides.
        paused_ = true;
        setStatus(Status::takenOver, "Another viewer is using the iPhone.");
    } else if (running_) {
        setStatus(Status::searching, problem);
    }
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
