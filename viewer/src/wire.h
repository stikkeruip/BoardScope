#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

/// The messages BoardScope and the viewer exchange over USB. Each message is a type byte,
/// a 4-byte big-endian payload length, then the payload. Numbers are big-endian; decimals are float32.
/// `ios/BoardScope/Wire.swift` is the other half of this file.
namespace wire {

constexpr std::uint16_t port = 7700;
constexpr std::size_t headerSize = 5;
constexpr std::uint32_t maxPayload = 16 * 1024 * 1024;

enum class Type : std::uint8_t {
    frame = 1,     // phone → PC: one JPEG image
    frameAck = 2,  // PC → phone: a frame arrived, so the phone may send another
    command = 3,   // PC → phone: set zoom, focus or JPEG quality
    state = 4,     // phone → PC: zoom, focus, quality and heat, after every change from either side
    ping = 5,      // either way: 8 opaque bytes, echoed back unchanged in a pong
    pong = 6,
};

enum class Thermal : std::uint8_t { nominal, fair, serious, critical };

struct State {
    /// The last of this viewer's commands the phone has applied.
    std::uint32_t lastCommand = 0;
    /// Zoom as the phone shows it, with the range the current lens allows.
    float zoom = 1, zoomMin = 1, zoomMax = 1;
    /// Lens position, from 0 (closest) to 1 (farthest).
    float focus = 0.5f;
    float quality = 0.75f;
    Thermal thermal = Thermal::nominal;
    bool pausedForHeat = false;
    bool torchOn = false;
    bool telephoto = false;
    /// Percent, or -1 when unknown (or the phone app is older than the battery).
    int battery = -1;
    bool charging = false;
};

struct Command {
    std::uint32_t sequence = 0;
    std::optional<float> zoom, focus, quality;
};

class Writer {
public:
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void f32(float value) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, 4);
        u32(bits);
    }
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool u8(std::uint8_t& value) {
        if (size_ - at_ < 1) return false;
        value = data_[at_++];
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (size_ - at_ < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) value = value << 8 | data_[at_++];
        return true;
    }
    bool f32(float& value) {
        std::uint32_t bits;
        if (!u32(bits)) return false;
        std::memcpy(&value, &bits, 4);
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
};

inline std::vector<std::uint8_t> message(Type type, const std::uint8_t* payload, std::size_t size) {
    Writer writer;
    writer.bytes.reserve(headerSize + size);
    writer.u8(static_cast<std::uint8_t>(type));
    writer.u32(static_cast<std::uint32_t>(size));
    writer.bytes.insert(writer.bytes.end(), payload, payload + size);
    return writer.bytes;
}

inline std::vector<std::uint8_t> encode(const Command& command) {
    Writer writer;
    writer.u32(command.sequence);
    writer.u8((command.zoom ? 1 : 0) | (command.focus ? 2 : 0) | (command.quality ? 4 : 0));
    writer.f32(command.zoom.value_or(0));
    writer.f32(command.focus.value_or(0));
    writer.f32(command.quality.value_or(0));
    return message(Type::command, writer.bytes.data(), writer.bytes.size());
}

inline std::optional<State> decodeState(const std::uint8_t* payload, std::size_t size) {
    Reader reader(payload, size);
    State state;
    std::uint8_t version, thermal, flags, lens;
    if (!reader.u8(version) || version < 1 || !reader.u32(state.lastCommand) || !reader.f32(state.zoom) ||
        !reader.f32(state.zoomMin) || !reader.f32(state.zoomMax) || !reader.f32(state.focus) ||
        !reader.f32(state.quality) || !reader.u8(thermal) || !reader.u8(flags) || !reader.u8(lens)) {
        return std::nullopt;
    }
    state.thermal = static_cast<Thermal>(thermal > 3 ? 3 : thermal);
    state.pausedForHeat = flags & 1;
    state.torchOn = flags & 2;
    state.charging = flags & 4;
    state.telephoto = lens == 1;
    std::uint8_t battery;
    if (reader.u8(battery) && battery <= 100) state.battery = battery;
    return state;
}

}  // namespace wire
