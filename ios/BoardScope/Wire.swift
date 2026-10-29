import Foundation

/// The messages BoardScope and the PC viewer exchange over USB. Each message is a type byte,
/// a 4-byte big-endian payload length, then the payload. Numbers are big-endian; decimals are float32.
/// `viewer/src/wire.h` is the other half of this file.
nonisolated enum MessageType: UInt8, Sendable {
    /// Phone → PC: one JPEG image.
    case frame = 1
    /// PC → phone: a frame arrived, so the phone may send another.
    case frameAck = 2
    /// PC → phone: set zoom, focus or JPEG quality.
    case command = 3
    /// Phone → PC: zoom, focus, quality and heat, sent after every change from either side.
    case state = 4
    /// Either way: 8 opaque bytes, echoed back unchanged in a pong.
    case ping = 5
    case pong = 6
}

nonisolated enum Wire {
    static let port: UInt16 = 7700
    static let headerSize = 5
    /// The PC only sends small messages; anything larger means the stream is out of step.
    static let maxIncomingPayload = 64 * 1024

    static func message(_ type: MessageType, _ payload: Data = Data()) -> Data {
        var data = Data(capacity: headerSize + payload.count)
        data.append(type.rawValue)
        data.appendBigEndian(UInt32(payload.count))
        data.append(payload)
        return data
    }

    static func header(_ data: Data) -> (type: UInt8, length: Int)? {
        var reader = WireReader(data)
        guard let type = reader.uint8(), let length = reader.uint32() else { return nil }
        return (type, Int(length))
    }
}

/// A change the PC asks for. Fields the PC left out are nil.
nonisolated struct ScopeCommand: Sendable {
    /// Counts up with every command, so the PC can tell which of its commands a state already includes.
    var sequence: UInt32
    var zoom: Double?
    var focus: Float?
    var quality: Float?

    init?(_ payload: Data) {
        var reader = WireReader(payload)
        guard let sequence = reader.uint32(), let fields = reader.uint8(),
              let zoom = reader.float32(), let focus = reader.float32(), let quality = reader.float32() else { return nil }
        self.sequence = sequence
        if fields & 1 != 0, zoom.isFinite { self.zoom = Double(zoom) }
        if fields & 2 != 0, focus.isFinite { self.focus = focus }
        if fields & 4 != 0, quality.isFinite { self.quality = quality }
    }
}

/// Everything the PC shows about the phone.
nonisolated struct ScopeState: Sendable, Equatable {
    static let version: UInt8 = 1

    /// The last command applied, so the PC knows its own changes have landed.
    var lastCommand: UInt32
    /// Zoom as shown on screen, with the range the current lens allows.
    var zoom: Double
    var zoomRange: ClosedRange<Double>
    /// Lens position, from 0 (closest) to 1 (farthest).
    var focus: Float
    var quality: Float
    var thermal: ProcessInfo.ThermalState
    var isPausedForHeat: Bool
    var isTorchOn: Bool
    var isTelephoto: Bool

    var payload: Data {
        var data = Data(capacity: 32)
        data.append(Self.version)
        data.appendBigEndian(lastCommand)
        data.appendBigEndian(Float(zoom))
        data.appendBigEndian(Float(zoomRange.lowerBound))
        data.appendBigEndian(Float(zoomRange.upperBound))
        data.appendBigEndian(focus)
        data.appendBigEndian(quality)
        data.append(UInt8(clamping: thermal.rawValue))
        data.append((isPausedForHeat ? 1 : 0) | (isTorchOn ? 2 : 0))
        data.append(isTelephoto ? 1 : 0)
        return data
    }
}

nonisolated struct WireReader {
    private let data: Data
    private var offset: Int

    init(_ data: Data) {
        self.data = data
        offset = data.startIndex
    }

    mutating func uint8() -> UInt8? {
        guard offset < data.endIndex else { return nil }
        defer { offset += 1 }
        return data[offset]
    }

    mutating func uint32() -> UInt32? {
        guard data.endIndex - offset >= 4 else { return nil }
        defer { offset += 4 }
        return data[offset..<offset + 4].reduce(0) { $0 << 8 | UInt32($1) }
    }

    mutating func float32() -> Float? {
        uint32().map(Float.init(bitPattern:))
    }
}

nonisolated extension Data {
    mutating func appendBigEndian(_ value: UInt32) {
        Swift.withUnsafeBytes(of: value.bigEndian) { append(contentsOf: $0) }
    }

    mutating func appendBigEndian(_ value: Float) {
        appendBigEndian(value.bitPattern)
    }
}
