import AVFoundation
import SwiftUI

/// Shows the whole camera frame, the same framing the PC will get.
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    /// Changes whenever the camera is reconfigured, so the rotation below is reapplied.
    let configurationID: Int

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspect
        return view
    }

    func updateUIView(_ view: PreviewView, context: Context) {
        // The app runs in landscape right only, which matches the sensor, so the picture needs no rotation.
        if let connection = view.previewLayer.connection, connection.isVideoRotationAngleSupported(0) {
            connection.videoRotationAngle = 0
        }
    }

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }
}
