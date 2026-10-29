import Foundation
import Network
import Observation
import dnssd

/// Serves the PC viewer over USB and, when allowed, over Wi-Fi.
///
/// Over USB the listener is on the loopback interface, which the PC reaches through Apple's USB
/// service, and a viewer is trusted at once. Over Wi-Fi the listener is announced with Bonjour,
/// and a viewer gets nothing until it sends the pairing code shown in Settings. One viewer at a
/// time: a newly trusted viewer takes over, and the previous one is told so it doesn't reconnect
/// on its own. Commands from the PC are applied straight away, and every change from either side
/// is sent back as the current state.
@Observable
final class StreamServer {
    enum Status: Equatable {
        case starting
        case waiting
        case viewing(overWiFi: Bool)
        case failed(String)
    }

    private(set) var status = Status.starting
    /// JPEG quality, from 0.3 to 0.95.
    private(set) var quality: Float
    /// Set by the screen, which already follows the phone's heat and battery.
    var thermal = ProcessInfo.processInfo.thermalState
    /// From 0 to 1, or negative when unknown.
    var batteryLevel: Float = -1
    var isCharging = false
    /// Whether viewers on the same Wi-Fi network may connect.
    private(set) var isWiFiEnabled: Bool
    /// Six digits a Wi-Fi viewer must send before it gets anything.
    private(set) var pairingCode: String
    /// Why viewing over Wi-Fi isn't available right now, if it isn't.
    private(set) var wifiProblem: String?

    var isViewing: Bool {
        if case .viewing = status { true } else { false }
    }

    let encoder = FrameEncoder()
    @ObservationIgnored private let camera: CameraController
    @ObservationIgnored private var usbListener: NWListener?
    @ObservationIgnored private var wifiListener: NWListener?
    @ObservationIgnored private var active: Viewer?
    /// Wi-Fi viewers that haven't sent the pairing code yet.
    @ObservationIgnored private var pending: [Viewer] = []
    @ObservationIgnored private var lastCommand: UInt32 = 0
    @ObservationIgnored private var lastDenied: ContinuousClock.Instant?
    @ObservationIgnored private var saveSoon: Task<Void, Never>?
    @ObservationIgnored private var meterSoon: Task<Void, Never>?

    /// The PC pings twice a second; silence this long means it's gone.
    private static let silenceLimit = Duration.seconds(5)
    /// A Wi-Fi viewer has this long to send the pairing code.
    private static let helloLimit = Duration.seconds(5)
    /// After a wrong code, tries are refused for this long, so the code can't be guessed quickly.
    private static let retryDelay = Duration.seconds(1)

    /// One viewer's connection.
    private final class Viewer {
        let connection: NWConnection
        let overWiFi: Bool
        let opened = ContinuousClock.now
        var lastHeard = ContinuousClock.now

        init(_ connection: NWConnection, overWiFi: Bool) {
            self.connection = connection
            self.overWiFi = overWiFi
        }
    }

    init(camera: CameraController) {
        self.camera = camera
        let defaults = UserDefaults.standard
        quality = Float(defaults.object(forKey: "jpegQuality") as? Double ?? 0.75)
        isWiFiEnabled = defaults.object(forKey: "wifiEnabled") as? Bool ?? true
        if let code = defaults.string(forKey: "wifiPairingCode") {
            pairingCode = code
        } else {
            pairingCode = Self.makePairingCode()
            defaults.set(pairingCode, forKey: "wifiPairingCode")
        }
        encoder.setQuality(quality)
    }

    /// Listens until the app ends, restarting listeners whenever they fail.
    func run() async {
        async let stateUpdates: Void = sendStateOnChange()
        while !Task.isCancelled {
            if usbListener == nil { usbListener = listen(overWiFi: false) }
            if isWiFiEnabled, wifiListener == nil { wifiListener = listen(overWiFi: true) }
            try? await Task.sleep(for: .seconds(1))
            let now = ContinuousClock.now
            if let active, now - active.lastHeard > Self.silenceLimit {
                print("[stream] PC went quiet")
                drop(active)
            }
            for viewer in pending where now - viewer.opened > Self.helloLimit {
                drop(viewer)
            }
        }
        await stateUpdates
    }

    func setQuality(_ newQuality: Float) {
        quality = min(max(newQuality, 0.3), 0.95)
        encoder.setQuality(quality)
        UserDefaults.standard.set(Double(quality), forKey: "jpegQuality")
    }

    func setWiFiEnabled(_ enabled: Bool) {
        isWiFiEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: "wifiEnabled")
        if enabled {
            if wifiListener == nil { wifiListener = listen(overWiFi: true) }
        } else {
            wifiListener?.cancel()
            wifiListener = nil
            wifiProblem = nil
            dropWiFiViewers()
        }
    }

    /// Replaces the pairing code; Wi-Fi viewers still using the old one are disconnected.
    func newPairingCode() {
        pairingCode = Self.makePairingCode()
        UserDefaults.standard.set(pairingCode, forKey: "wifiPairingCode")
        dropWiFiViewers()
    }

    /// The phone's address on Wi-Fi, for typing into the viewer when it can't find the phone itself.
    static func wifiAddress() -> String? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard String(cString: entry.pointee.ifa_name) == "en0", let address = entry.pointee.ifa_addr,
                  address.pointee.sa_family == UInt8(AF_INET) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 else { continue }
            return host.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        }
        return nil
    }

    private static func makePairingCode() -> String {
        String(format: "%06d", Int.random(in: 0..<1_000_000))
    }

    private func listen(overWiFi: Bool) -> NWListener? {
        let parameters = NWParameters.tcp
        parameters.requiredInterfaceType = overWiFi ? .wifi : .loopback
        parameters.allowLocalEndpointReuse = true
        let port = overWiFi ? Wire.wifiPort : Wire.port
        guard let listener = try? NWListener(using: parameters, on: NWEndpoint.Port(rawValue: port)!) else {
            if overWiFi { wifiProblem = "Can't open port \(port)." } else { status = .failed("Can't open port \(port)") }
            return nil
        }
        if overWiFi { listener.service = NWListener.Service(type: Wire.bonjourType) }
        listener.stateUpdateHandler = { [weak self, weak listener] state in
            MainActor.assumeIsolated {
                guard let self, let listener else { return }
                self.listener(listener, changedTo: state, overWiFi: overWiFi)
            }
        }
        listener.newConnectionHandler = { [weak self] connection in
            MainActor.assumeIsolated { self?.accept(connection, overWiFi: overWiFi) }
        }
        listener.start(queue: .main)
        return listener
    }

    private func listener(_ listener: NWListener, changedTo state: NWListener.State, overWiFi: Bool) {
        guard listener === (overWiFi ? wifiListener : usbListener) else { return }
        switch state {
        case .ready:
            print("[stream] listening on port \(listener.port?.rawValue ?? 0)\(overWiFi ? " on Wi-Fi" : "")")
            if overWiFi {
                wifiProblem = nil
            } else if active == nil {
                status = .waiting
            }
        case .waiting(let error):
            if overWiFi { wifiProblem = Self.describe(error) }
        case .failed(let error):
            print("[stream] listener failed: \(error)")
            listener.cancel()
            if overWiFi {
                wifiListener = nil
                wifiProblem = Self.describe(error)
            } else {
                usbListener = nil
                status = .failed("Link error")
            }
        default:
            break
        }
    }

    private static func describe(_ error: NWError) -> String {
        switch error {
        case .dns(let code) where code == DNSServiceErrorType(kDNSServiceErr_PolicyDenied):
            "Allow Local Network for BoardScope in Settings → Privacy & Security → Local Network."
        case .posix(.ENETDOWN), .posix(.ENETUNREACH):
            "The phone isn't on Wi-Fi."
        default:
            error.localizedDescription
        }
    }

    private func accept(_ connection: NWConnection, overWiFi: Bool) {
        let viewer = Viewer(connection, overWiFi: overWiFi)
        // Counted from the start, so a Wi-Fi connection that never finishes setting up times out too.
        if overWiFi { pending.append(viewer) }
        // The handler keeps the viewer alive until `forget` removes it.
        connection.stateUpdateHandler = { [weak self] state in
            MainActor.assumeIsolated {
                guard let self else { return }
                switch state {
                case .ready:
                    if !overWiFi { self.trust(viewer) }
                    self.receive(from: viewer)
                case .failed, .cancelled:
                    self.drop(viewer)
                default:
                    break
                }
            }
        }
        connection.start(queue: .main)
    }

    /// Serves `viewer` from now on, telling the previous viewer it was taken over.
    private func trust(_ viewer: Viewer) {
        pending.removeAll { $0 === viewer }
        if let previous = active { dismiss(previous, with: .takenOver) }
        active = viewer
        // Each viewer numbers its commands from 1.
        lastCommand = 0
        viewer.lastHeard = .now
        print("[stream] PC connected over \(viewer.overWiFi ? "Wi-Fi" : "USB")")
        status = .viewing(overWiFi: viewer.overWiFi)
        let connection = viewer.connection
        encoder.start { [weak connection] jpeg in
            connection?.send(content: Wire.message(.frame, jpeg), completion: .idempotent)
        }
        sendState()
    }

    private func isOpen(_ viewer: Viewer) -> Bool {
        viewer === active || pending.contains { $0 === viewer }
    }

    /// Stops serving `viewer` without closing its connection.
    private func forget(_ viewer: Viewer) {
        viewer.connection.stateUpdateHandler = nil
        pending.removeAll { $0 === viewer }
        guard viewer === active else { return }
        print("[stream] PC disconnected")
        active = nil
        encoder.stop()
        status = usbListener == nil ? .failed("Link error") : .waiting
    }

    private func drop(_ viewer: Viewer) {
        forget(viewer)
        viewer.connection.cancel()
    }

    /// Sends one last message and closes the connection a moment later. Closing straight away,
    /// while the viewer's own messages are still arriving, resets the connection and can lose the message.
    private func dismiss(_ viewer: Viewer, with type: MessageType) {
        forget(viewer)
        let connection = viewer.connection
        connection.send(content: Wire.message(type), completion: .idempotent)
        Task {
            try? await Task.sleep(for: .seconds(2))
            connection.cancel()
        }
    }

    private func dropWiFiViewers() {
        pending.forEach(drop)
        if let active, active.overWiFi { drop(active) }
    }

    /// Reads one message, handles it, and reads the next.
    private func receive(from viewer: Viewer) {
        let connection = viewer.connection
        connection.receive(minimumIncompleteLength: Wire.headerSize, maximumLength: Wire.headerSize) { [weak self] header, _, _, error in
            MainActor.assumeIsolated {
                guard let self, self.isOpen(viewer) else { return }
                guard error == nil, let header, let (type, length) = Wire.header(header), length <= Wire.maxIncomingPayload else {
                    return self.drop(viewer)
                }
                guard length > 0 else {
                    self.handle(type, Data(), from: viewer)
                    return self.receive(from: viewer)
                }
                connection.receive(minimumIncompleteLength: length, maximumLength: length) { [weak self] payload, _, _, error in
                    MainActor.assumeIsolated {
                        guard let self, self.isOpen(viewer) else { return }
                        guard error == nil, let payload, payload.count == length else { return self.drop(viewer) }
                        self.handle(type, payload, from: viewer)
                        if self.isOpen(viewer) { self.receive(from: viewer) }
                    }
                }
            }
        }
    }

    private func handle(_ type: UInt8, _ payload: Data, from viewer: Viewer) {
        viewer.lastHeard = .now
        guard viewer === active else {
            // Until a Wi-Fi viewer gives the pairing code, the only thing it may send is the code.
            if MessageType(rawValue: type) == .hello { checkPairingCode(payload, from: viewer) }
            return
        }
        switch MessageType(rawValue: type) {
        case .frameAck:
            encoder.frameReceived()
        case .ping:
            viewer.connection.send(content: Wire.message(.pong, payload), completion: .idempotent)
        case .command:
            guard let command = ScopeCommand(payload) else { return }
            apply(command)
        default:
            break
        }
    }

    private func checkPairingCode(_ payload: Data, from viewer: Viewer) {
        let code = String(decoding: payload.dropFirst(), as: UTF8.self)
        let tooSoon = lastDenied.map { ContinuousClock.now - $0 < Self.retryDelay } ?? false
        if !tooSoon, code == pairingCode {
            trust(viewer)
        } else {
            print("[stream] wrong pairing code over Wi-Fi")
            lastDenied = .now
            dismiss(viewer, with: .denied)
        }
    }

    private func apply(_ command: ScopeCommand) {
        lastCommand = command.sequence
        if let zoom = command.zoom { camera.setDisplayZoom(zoom) }
        if let focus = command.focus { camera.setFocus(focus) }
        if let quality = command.quality { setQuality(quality) }
        if let on = command.isTorchOn { camera.setTorch(on: on) }
        if let level = command.torchLevel { camera.setTorchLevel(level) }
        if command.isTorchOn != nil || command.torchLevel != nil {
            // The light changes how bright the board is; measure once the PC stops moving it.
            meterSoon?.cancel()
            meterSoon = Task {
                try? await Task.sleep(for: .milliseconds(600))
                guard !Task.isCancelled else { return }
                await camera.finishTorchChange()
            }
        }
        // Sent even when nothing changed, so the PC learns its command landed, clamped or not.
        sendState()
        saveSoon?.cancel()
        saveSoon = Task {
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
            camera.save()
        }
    }

    private var state: ScopeState {
        ScopeState(
            lastCommand: lastCommand,
            zoom: camera.displayZoom,
            zoomRange: camera.displayZoomRange,
            focus: camera.focus,
            quality: quality,
            thermal: thermal,
            isPausedForHeat: camera.isPausedForHeat,
            isTorchOn: camera.isTorchOn,
            hasTorch: camera.hasTorch,
            torchLevel: camera.torchLevel,
            isTelephoto: camera.lens == .telephoto,
            batteryLevel: batteryLevel,
            isCharging: isCharging)
    }

    private func sendState() {
        active?.connection.send(content: Wire.message(.state, state.payload), completion: .idempotent)
    }

    /// Sends the state whenever the camera, quality, heat or battery changes on the phone.
    private func sendStateOnChange() async {
        for await _ in Observations({ self.state }) {
            sendState()
        }
    }
}
