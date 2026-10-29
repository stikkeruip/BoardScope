import Combine
import SwiftUI

/// Full-screen preview with pinch and double-tap zoom, a focus slider, and a status corner.
/// Controls fade after a few seconds without a touch. While the PC is viewing, the screen
/// also dims then, and brightens again at a touch.
struct ContentView: View {
    @State private var camera: CameraController
    @State private var server: StreamServer
    @Environment(\.scenePhase) private var scenePhase
    /// The user's brightness while the screen is dimmed. Kept on disk, so a run that ends dimmed
    /// (the app was closed or reinstalled) is undone at the next launch.
    @AppStorage("brightnessBeforeDimming") private var brightnessBeforeDimming: Double?
    @State private var thermal = ProcessInfo.processInfo.thermalState
    @State private var isCharging = false
    @State private var batteryLevel: Float = -1
    @State private var controlsVisible = true
    @State private var zoomVisible = false
    @State private var showSettings = false
    @State private var pinchStartZoom: CGFloat?
    @State private var hideControls: Task<Void, Never>?
    @State private var hideZoom: Task<Void, Never>?

    init() {
        let camera = CameraController()
        _camera = State(initialValue: camera)
        _server = State(initialValue: StreamServer(camera: camera))
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            CameraPreview(session: camera.session, configurationID: camera.configurationID)
                .ignoresSafeArea()
                .gesture(pinch)
                .onTapGesture(count: 2) {
                    camera.toggleZoom()
                    flashZoom()
                }
            if zoomVisible {
                Text(String(format: "%.1fx", Double(camera.displayZoom)))
                    .font(.system(size: 72, weight: .bold, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(.white)
                    .shadow(radius: 8)
                    .allowsHitTesting(false)
            }
            if let message {
                Text(message)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white)
                    .padding()
                    .background(.black.opacity(0.7), in: .rect(cornerRadius: 12))
            }
            controls
        }
        .simultaneousGesture(DragGesture(minimumDistance: 0).onChanged { _ in showControls() })
        .sheet(isPresented: $showSettings) { SettingsView(camera: camera, server: server) }
        .task {
            UIDevice.current.isBatteryMonitoringEnabled = true
            updateBattery()
            showControls()
            dimScreen(false)
            await camera.start(streamingTo: server.encoder)
            await camera.adapt(to: thermal)
        }
        .task { await server.run() }
        .onChange(of: thermal) { _, state in
            server.thermal = state
            Task { await camera.adapt(to: state) }
        }
        .onChange(of: server.isViewing && !controlsVisible && scenePhase == .active) { _, dimmed in
            dimScreen(dimmed)
        }
        .onReceive(NotificationCenter.default.publisher(for: ProcessInfo.thermalStateDidChangeNotification).receive(on: DispatchQueue.main)) { _ in
            thermal = ProcessInfo.processInfo.thermalState
        }
        .onReceive(NotificationCenter.default.publisher(for: UIDevice.batteryStateDidChangeNotification).receive(on: DispatchQueue.main)) { _ in
            updateBattery()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIDevice.batteryLevelDidChangeNotification).receive(on: DispatchQueue.main)) { _ in
            updateBattery()
        }
    }

    private var message: String? {
        if let problem = camera.problem { return problem }
        if camera.isPausedForHeat { return "Camera paused: the phone is too hot.\nIt resumes when it cools down." }
        return nil
    }

    private var controls: some View {
        VStack {
            HStack(alignment: .top) {
                StatusBadge(lens: camera.lens, minimumFocusDistance: camera.minimumFocusDistance, thermal: thermal, isCharging: isCharging, batteryLevel: batteryLevel, link: server.status)
                Spacer()
                Group {
                    roundButton(camera.isTorchOn ? "flashlight.on.fill" : "flashlight.off.fill", tint: camera.isTorchOn ? .yellow : .white) {
                        Task { await camera.setTorch(on: !camera.isTorchOn) }
                    }
                    .disabled(!camera.hasTorch)
                    roundButton("gearshape.fill", tint: .white) { showSettings = true }
                }
                .opacity(controlsVisible ? 1 : 0)
                .allowsHitTesting(controlsVisible)
            }
            Spacer()
            if camera.isTorchOn {
                torchSlider
                    .opacity(controlsVisible ? 1 : 0)
                    .allowsHitTesting(controlsVisible)
            }
        }
        .overlay(alignment: .trailing) {
            FocusSlider(position: camera.focus, onChange: camera.setFocus, onEnd: camera.save)
                .padding(.top, 80)
                .padding(.bottom, 40)
                .opacity(controlsVisible ? 1 : 0)
                .allowsHitTesting(controlsVisible)
        }
        .padding()
    }

    private var torchSlider: some View {
        HStack(spacing: 12) {
            Image(systemName: "sun.min.fill")
            Slider(
                value: Binding(get: { camera.torchLevel }, set: { camera.setTorchLevel($0) }),
                in: 0.05...1,
                onEditingChanged: { editing in
                    if !editing { Task { await camera.finishTorchChange() } }
                }
            )
            .tint(.yellow)
            Image(systemName: "sun.max.fill")
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(.black.opacity(0.5), in: Capsule())
        .frame(maxWidth: 380)
    }

    private func roundButton(_ symbol: String, tint: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.title2)
                .foregroundStyle(tint)
                .frame(width: 44, height: 44)
                .background(.black.opacity(0.5), in: Circle())
        }
    }

    private var pinch: some Gesture {
        MagnifyGesture()
            .onChanged { value in
                let start = pinchStartZoom ?? camera.zoom
                pinchStartZoom = start
                camera.setZoom(start * value.magnification)
                flashZoom()
            }
            .onEnded { _ in
                pinchStartZoom = nil
                camera.save()
            }
    }

    private func showControls() {
        withAnimation(.easeOut(duration: 0.2)) { controlsVisible = true }
        hideControls?.cancel()
        hideControls = Task {
            try? await Task.sleep(for: .seconds(4))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.6)) { controlsVisible = false }
        }
    }

    private func flashZoom() {
        zoomVisible = true
        hideZoom?.cancel()
        hideZoom = Task {
            try? await Task.sleep(for: .seconds(1.2))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.3)) { zoomVisible = false }
        }
    }

    /// Turns the brightness all the way down while the PC shows the picture, to save battery and heat.
    /// The user's brightness comes back afterwards.
    private func dimScreen(_ dimmed: Bool) {
        guard let screen = UIApplication.shared.connectedScenes.compactMap({ ($0 as? UIWindowScene)?.screen }).first else { return }
        if dimmed {
            let brightness = brightnessBeforeDimming ?? Double(screen.brightness)
            brightnessBeforeDimming = brightness
            screen.brightness = 0
            print("[screen] dimmed from \(brightness)")
        } else if let brightness = brightnessBeforeDimming {
            screen.brightness = CGFloat(brightness)
            brightnessBeforeDimming = nil
            print("[screen] brightness back to \(brightness)")
        }
    }

    private func updateBattery() {
        let state = UIDevice.current.batteryState
        isCharging = state == .charging || state == .full
        batteryLevel = UIDevice.current.batteryLevel
        server.isCharging = isCharging
        server.batteryLevel = batteryLevel
    }
}
