import AVFoundation
import Observation

/// Owns the capture session and keeps the camera locked: one physical lens, manual focus,
/// fixed exposure and white balance. Nothing adjusts on its own while you work.
@Observable
final class CameraController {
    private(set) var lens: Lens
    private(set) var shutter: Shutter
    /// The camera's own zoom factor.
    private(set) var zoom: CGFloat = 1
    /// Lens position, from 0 (closest) to 1 (farthest).
    private(set) var focus: Float = 0.5
    /// Millimeters, as reported by the camera.
    private(set) var minimumFocusDistance: Int?
    private(set) var hasTorch = false
    private(set) var isTorchOn: Bool
    /// Torch brightness, from 0.05 to 1.
    private(set) var torchLevel: Float
    private(set) var isPausedForHeat = false
    private(set) var problem: String?
    /// Changes whenever the camera is reconfigured.
    private(set) var configurationID = 0

    let session = AVCaptureSession()
    /// Frames for the PC, with the same framing as the preview.
    @ObservationIgnored private let videoOutput = AVCaptureVideoDataOutput()
    @ObservationIgnored private var device: AVCaptureDevice?
    @ObservationIgnored private var framesPerSecond: CMTimeScale = 30
    @ObservationIgnored private let sessionQueue = DispatchQueue(label: "com.uipko.boardscope.session")

    var displayZoom: CGFloat { zoom * lens.displayScale }

    /// Zoom range as shown on screen, limited by what the current camera can do.
    var displayZoomRange: ClosedRange<Double> {
        let range = zoomRange
        return Double(range.lowerBound * lens.displayScale)...Double(range.upperBound * lens.displayScale)
    }

    private var zoomRange: ClosedRange<CGFloat> {
        let maxZoom = min(lens.zoomRange.upperBound, device?.activeFormat.videoMaxZoomFactor ?? 1)
        return lens.zoomRange.lowerBound...max(maxZoom, lens.zoomRange.lowerBound)
    }

    init() {
        let defaults = UserDefaults.standard
        lens = Lens(rawValue: defaults.string(forKey: "lens") ?? "") ?? .main
        shutter = Shutter(rawValue: defaults.integer(forKey: "shutter")) ?? .s100
        isTorchOn = defaults.bool(forKey: "torchOn")
        torchLevel = Float(defaults.object(forKey: "torchLevel") as? Double ?? 0.5)
    }

    func start(streamingTo encoder: FrameEncoder) async {
        guard await AVCaptureDevice.requestAccess(for: .video) else {
            problem = "BoardScope needs the camera. Allow it in Settings → BoardScope."
            return
        }
        session.sessionPreset = .inputPriority
        videoOutput.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
        videoOutput.alwaysDiscardsLateVideoFrames = true
        videoOutput.setSampleBufferDelegate(encoder, queue: encoder.queue)
        if session.canAddOutput(videoOutput) { session.addOutput(videoOutput) }
        await configure(lens)
    }

    func select(_ newLens: Lens) async {
        guard newLens != lens else { return }
        save()
        await configure(newLens)
    }

    func setZoom(_ factor: CGFloat) {
        zoom = min(max(factor, zoomRange.lowerBound), zoomRange.upperBound)
        guard let device, (try? device.lockForConfiguration()) != nil else { return }
        device.videoZoomFactor = zoom
        device.unlockForConfiguration()
    }

    func setDisplayZoom(_ value: Double) {
        setZoom(CGFloat(value) / lens.displayScale)
    }

    func toggleZoom() {
        let zooms = lens.doubleTapZooms
        setZoom(zoom < zooms.high ? zooms.high : zooms.low)
        save()
    }

    func setFocus(_ position: Float) {
        focus = min(max(position, 0), 1)
        guard let device, device.isLockingFocusWithCustomLensPositionSupported,
              (try? device.lockForConfiguration()) != nil else { return }
        device.setFocusModeLocked(lensPosition: focus, completionHandler: nil)
        device.unlockForConfiguration()
    }

    /// Call `finishTorchChange()` once the light has settled.
    func setTorch(on: Bool) {
        isTorchOn = on
        applyTorch()
    }

    func setTorchLevel(_ level: Float) {
        torchLevel = min(max(level, 0.05), 1)
        applyTorch()
    }

    /// The torch changes how bright the board is, so exposure is measured again.
    func finishTorchChange() async {
        save()
        await meterLight()
    }

    func setShutter(_ newShutter: Shutter) async {
        shutter = newShutter
        save()
        await meterLight()
    }

    /// Measures the light once, then locks exposure at the flicker-free shutter speed, and white balance.
    func meterLight() async {
        guard let device, (try? device.lockForConfiguration()) != nil else { return }
        device.exposureMode = .continuousAutoExposure
        device.whiteBalanceMode = .continuousAutoWhiteBalance
        device.unlockForConfiguration()
        await settle { device.isAdjustingExposure || device.isAdjustingWhiteBalance }

        let format = device.activeFormat
        let duration = CMTimeClampToRange(shutter.duration, range: CMTimeRange(start: format.minExposureDuration, end: format.maxExposureDuration))
        // Same brightness at the new shutter speed: ISO scales inversely with exposure time.
        let iso = min(max(device.iso * Float(device.exposureDuration.seconds / duration.seconds), format.minISO), format.maxISO)
        guard (try? device.lockForConfiguration()) != nil else { return }
        device.setExposureModeCustom(duration: duration, iso: iso, completionHandler: nil)
        device.setWhiteBalanceModeLocked(with: device.deviceWhiteBalanceGains, completionHandler: nil)
        device.unlockForConfiguration()
        print("[camera] exposure locked at 1/\(shutter.rawValue) s, ISO \(Int(iso))")
    }

    /// Runs at 15 fps when the phone is hot, and pauses the camera when it is too hot.
    func adapt(to thermal: ProcessInfo.ThermalState) async {
        framesPerSecond = thermal.rawValue >= ProcessInfo.ThermalState.serious.rawValue ? 15 : 30
        if let device { applyFrameRate(device) }
        if thermal == .critical, !isPausedForHeat {
            isPausedForHeat = true
            await stopRunning()
        } else if thermal != .critical, isPausedForHeat {
            isPausedForHeat = false
            await startRunning()
        }
    }

    /// Remembers the lens, the shutter, and each lens's zoom and focus for the next launch.
    func save() {
        let defaults = UserDefaults.standard
        defaults.set(lens.rawValue, forKey: "lens")
        defaults.set(shutter.rawValue, forKey: "shutter")
        defaults.set(Double(zoom), forKey: "zoom.\(lens.rawValue)")
        defaults.set(Double(focus), forKey: "focus.\(lens.rawValue)")
        defaults.set(isTorchOn, forKey: "torchOn")
        defaults.set(Double(torchLevel), forKey: "torchLevel")
    }

    private func savedValue(_ name: String) -> Double? {
        UserDefaults.standard.object(forKey: "\(name).\(lens.rawValue)") as? Double
    }

    private func configure(_ newLens: Lens) async {
        guard let device = AVCaptureDevice.default(newLens.deviceType, for: .video, position: .back),
              let input = try? AVCaptureDeviceInput(device: device) else {
            problem = "The \(newLens.name.lowercased()) camera isn't available."
            return
        }
        if let oldDevice = self.device, oldDevice.hasTorch, (try? oldDevice.lockForConfiguration()) != nil {
            oldDevice.torchMode = .off
            oldDevice.unlockForConfiguration()
        }
        session.beginConfiguration()
        session.inputs.forEach(session.removeInput)
        let added = session.canAddInput(input)
        if added { session.addInput(input) }
        session.commitConfiguration()
        guard added else {
            problem = "The \(newLens.name.lowercased()) camera can't be opened."
            return
        }

        self.device = device
        lens = newLens
        problem = nil
        minimumFocusDistance = device.minimumFocusDistance >= 0 ? device.minimumFocusDistance : nil
        hasTorch = device.hasTorch
        configureFormat(device)
        configurationID += 1
        if !isPausedForHeat { await startRunning() }

        setZoom(savedValue("zoom").map { CGFloat($0) } ?? newLens.doubleTapZooms.low)
        if let saved = savedValue("focus") {
            setFocus(Float(saved))
        } else {
            await focusOnce(device)
        }
        applyTorch()
        await meterLight()
        save()
        print("[camera] \(lens.name): closest focus \(device.minimumFocusDistance) mm, zoom \(displayZoom)x, focus \(focus), torch \(isTorchOn ? "on at \(torchLevel)" : "off")")
    }

    private func applyTorch() {
        guard let device, device.hasTorch, (try? device.lockForConfiguration()) != nil else { return }
        if isTorchOn, device.isTorchAvailable {
            try? device.setTorchModeOn(level: torchLevel)
        } else {
            device.torchMode = .off
        }
        device.unlockForConfiguration()
    }

    private func configureFormat(_ device: AVCaptureDevice) {
        guard (try? device.lockForConfiguration()) != nil else { return }
        if let format = Self.streamFormat(for: device) { device.activeFormat = format }
        if device.activeFormat.isVideoHDRSupported {
            device.automaticallyAdjustsVideoHDREnabled = false
            device.isVideoHDREnabled = false
        }
        device.unlockForConfiguration()
        applyFrameRate(device)
    }

    /// A 1080p format that runs at 30 fps, preferring one whose 2x zoom reads the 48 MP sensor at full detail,
    /// then full-range color, which the JPEG encoder takes without converting.
    private static func streamFormat(for device: AVCaptureDevice) -> AVCaptureDevice.Format? {
        let candidates = device.formats.filter { format in
            let size = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            return size.width == 1920 && size.height == 1080
                && format.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 30 }
        }
        let detailed = candidates.filter { $0.secondaryNativeResolutionZoomFactors.contains(2) }
        let preferred = detailed.isEmpty ? candidates : detailed
        let fullRange = preferred.first {
            CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        }
        return fullRange ?? preferred.first
    }

    /// Fixes the frame rate, changing the min and max frame durations in an order the camera accepts.
    private func applyFrameRate(_ device: AVCaptureDevice) {
        guard (try? device.lockForConfiguration()) != nil else { return }
        let frame = CMTime(value: 1, timescale: framesPerSecond)
        if frame > device.activeVideoMaxFrameDuration {
            device.activeVideoMaxFrameDuration = frame
            device.activeVideoMinFrameDuration = frame
        } else {
            device.activeVideoMinFrameDuration = frame
            device.activeVideoMaxFrameDuration = frame
        }
        device.unlockForConfiguration()
    }

    /// First use of a lens only: focuses once on the center to get a starting point, then locks.
    private func focusOnce(_ device: AVCaptureDevice) async {
        if device.isFocusModeSupported(.autoFocus), (try? device.lockForConfiguration()) != nil {
            if device.isFocusPointOfInterestSupported { device.focusPointOfInterest = CGPoint(x: 0.5, y: 0.5) }
            device.focusMode = .autoFocus
            device.unlockForConfiguration()
            await settle { device.isAdjustingFocus }
        }
        setFocus(device.lensPosition)
    }

    /// Waits for an automatic adjustment to finish, for up to about five seconds.
    private func settle(while adjusting: () -> Bool) async {
        try? await Task.sleep(for: .milliseconds(300))
        for _ in 0..<50 {
            guard adjusting() else { return }
            try? await Task.sleep(for: .milliseconds(100))
        }
    }

    private func startRunning() async {
        nonisolated(unsafe) let session = session
        await withCheckedContinuation { done in
            sessionQueue.async {
                session.startRunning()
                done.resume()
            }
        }
    }

    private func stopRunning() async {
        nonisolated(unsafe) let session = session
        await withCheckedContinuation { done in
            sessionQueue.async {
                session.stopRunning()
                done.resume()
            }
        }
    }
}
