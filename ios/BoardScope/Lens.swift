import AVFoundation

/// A physical back camera. The app locks onto one and never switches on its own.
enum Lens: String, CaseIterable, Identifiable {
    case main
    case telephoto

    var id: Self { self }

    var name: String {
        switch self {
        case .main: "Main"
        case .telephoto: "Telephoto"
        }
    }

    var deviceType: AVCaptureDevice.DeviceType {
        switch self {
        case .main: .builtInWideAngleCamera
        case .telephoto: .builtInTelephotoCamera
        }
    }

    /// Zoom shown on screen for the camera's own 1x; the telephoto starts at 4x.
    var displayScale: CGFloat {
        switch self {
        case .main: 1
        case .telephoto: 4
        }
    }

    /// Range of the camera's own zoom factor.
    var zoomRange: ClosedRange<CGFloat> {
        switch self {
        case .main: 1...8
        case .telephoto: 1...4
        }
    }

    /// Double-tap jumps between these two zoom factors.
    var doubleTapZooms: (low: CGFloat, high: CGFloat) {
        switch self {
        case .main: (2, 4)
        case .telephoto: (1, 2)
        }
    }
}

/// Shutter speeds that don't flicker under lights on mains power.
enum Shutter: Int, CaseIterable, Identifiable {
    case s100 = 100
    case s50 = 50
    case s120 = 120
    case s60 = 60

    var id: Self { self }

    var duration: CMTime { CMTime(value: 1, timescale: CMTimeScale(rawValue)) }

    var label: String { "1/\(rawValue) s · \(rawValue % 50 == 0 ? 50 : 60) Hz mains" }
}
