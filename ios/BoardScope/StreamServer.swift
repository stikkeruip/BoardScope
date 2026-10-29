import Foundation
import Network
import Observation

/// Serves the PC viewer over USB: a TCP listener on the loopback interface, which the Mac or PC
/// reaches through Apple's USB service, so nothing is exposed on Wi-Fi. One viewer at a time;
/// a new connection replaces the old one. Commands from the PC are applied straight away, and
/// every change from either side is sent back as the current state.
@Observable
final class StreamServer {
    enum Status: Equatable {
        case starting
        case waiting
        case viewing
        case failed(String)
    }

    private(set) var status = Status.starting
    /// JPEG quality, from 0.3 to 0.95.
    private(set) var quality: Float
    /// Set by the screen, which already follows the phone's heat.
    var thermal = ProcessInfo.processInfo.thermalState

    var isViewing: Bool { status == .viewing }

    let encoder = FrameEncoder()
    @ObservationIgnored private let camera: CameraController
    @ObservationIgnored private var listener: NWListener?
    @ObservationIgnored private var connection: NWConnection?
    @ObservationIgnored private var lastCommand: UInt32 = 0
    @ObservationIgnored private var lastHeard = ContinuousClock.now
    @ObservationIgnored private var saveSoon: Task<Void, Never>?

    /// The PC pings twice a second; silence this long means it's gone.
    private static let silenceLimit = Duration.seconds(5)

    init(camera: CameraController) {
        self.camera = camera
        quality = Float(UserDefaults.standard.object(forKey: "jpegQuality") as? Double ?? 0.75)
        encoder.setQuality(quality)
    }

    /// Listens until the app ends, restarting the listener whenever it fails.
    func run() async {
        async let stateUpdates: Void = sendStateOnChange()
        while !Task.isCancelled {
            if listener == nil { listen() }
            try? await Task.sleep(for: .seconds(1))
            if let connection, ContinuousClock.now - lastHeard > Self.silenceLimit {
                print("[stream] PC went quiet")
                drop(connection)
            }
        }
        await stateUpdates
    }

    func setQuality(_ newQuality: Float) {
        quality = min(max(newQuality, 0.3), 0.95)
        encoder.setQuality(quality)
        UserDefaults.standard.set(Double(quality), forKey: "jpegQuality")
    }

    private func listen() {
        let parameters = NWParameters.tcp
        parameters.requiredInterfaceType = .loopback
        parameters.allowLocalEndpointReuse = true
        guard let listener = try? NWListener(using: parameters, on: NWEndpoint.Port(rawValue: Wire.port)!) else {
            status = .failed("Can't open port \(Wire.port)")
            return
        }
        listener.stateUpdateHandler = { [weak self, weak listener] state in
            MainActor.assumeIsolated {
                guard let self, let listener, listener === self.listener else { return }
                switch state {
                case .ready:
                    if self.connection == nil { self.status = .waiting }
                    print("[stream] listening on port \(Wire.port)")
                case .failed(let error):
                    print("[stream] listener failed: \(error)")
                    self.status = .failed("Link error")
                    listener.cancel()
                    self.listener = nil
                default:
                    break
                }
            }
        }
        listener.newConnectionHandler = { [weak self] connection in
            MainActor.assumeIsolated { self?.accept(connection) }
        }
        self.listener = listener
        listener.start(queue: .main)
    }

    private func accept(_ newConnection: NWConnection) {
        if let connection { drop(connection) }
        connection = newConnection
        // Each viewer numbers its commands from 1.
        lastCommand = 0
        lastHeard = .now
        newConnection.stateUpdateHandler = { [weak self, weak newConnection] state in
            MainActor.assumeIsolated {
                guard let self, let newConnection, newConnection === self.connection else { return }
                switch state {
                case .ready:
                    print("[stream] PC connected")
                    self.status = .viewing
                    self.encoder.start { [weak newConnection] jpeg in
                        newConnection?.send(content: Wire.message(.frame, jpeg), completion: .idempotent)
                    }
                    self.sendState()
                    self.receive(on: newConnection)
                case .failed, .cancelled:
                    self.drop(newConnection)
                default:
                    break
                }
            }
        }
        newConnection.start(queue: .main)
    }

    private func drop(_ dropped: NWConnection) {
        dropped.stateUpdateHandler = nil
        dropped.cancel()
        guard dropped === connection else { return }
        print("[stream] PC disconnected")
        connection = nil
        encoder.stop()
        status = listener == nil ? .failed("Link error") : .waiting
    }

    /// Reads one message, handles it, and reads the next.
    private func receive(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: Wire.headerSize, maximumLength: Wire.headerSize) { [weak self] header, _, _, error in
            MainActor.assumeIsolated {
                guard let self, connection === self.connection else { return }
                guard error == nil, let header, let (type, length) = Wire.header(header), length <= Wire.maxIncomingPayload else {
                    return self.drop(connection)
                }
                guard length > 0 else {
                    self.handle(type, Data(), on: connection)
                    return self.receive(on: connection)
                }
                connection.receive(minimumIncompleteLength: length, maximumLength: length) { [weak self] payload, _, _, error in
                    MainActor.assumeIsolated {
                        guard let self, connection === self.connection else { return }
                        guard error == nil, let payload, payload.count == length else { return self.drop(connection) }
                        self.handle(type, payload, on: connection)
                        self.receive(on: connection)
                    }
                }
            }
        }
    }

    private func handle(_ type: UInt8, _ payload: Data, on connection: NWConnection) {
        lastHeard = .now
        switch MessageType(rawValue: type) {
        case .frameAck:
            encoder.frameReceived()
        case .ping:
            connection.send(content: Wire.message(.pong, payload), completion: .idempotent)
        case .command:
            guard let command = ScopeCommand(payload) else { return }
            apply(command)
        default:
            break
        }
    }

    private func apply(_ command: ScopeCommand) {
        lastCommand = command.sequence
        if let zoom = command.zoom { camera.setDisplayZoom(zoom) }
        if let focus = command.focus { camera.setFocus(focus) }
        if let quality = command.quality { setQuality(quality) }
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
            isTelephoto: camera.lens == .telephoto)
    }

    private func sendState() {
        connection?.send(content: Wire.message(.state, state.payload), completion: .idempotent)
    }

    /// Sends the state whenever the camera, quality or heat changes on the phone.
    private func sendStateOnChange() async {
        for await _ in Observations({ self.state }) {
            sendState()
        }
    }
}
