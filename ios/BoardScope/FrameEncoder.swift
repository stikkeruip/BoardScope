import AVFoundation
import Synchronization
import VideoToolbox

/// Turns camera frames into JPEGs for the PC with the hardware encoder, one at a time.
/// A frame is skipped, never queued, while the previous one is still encoding or the PC
/// hasn't received the last ones yet, so the PC always gets the newest picture.
nonisolated final class FrameEncoder: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let queue = DispatchQueue(label: "com.uipko.boardscope.frames", qos: .userInteractive)
    /// Frames sent but not yet acknowledged. Two keeps the cable busy without building a backlog.
    private static let maxInFlight = 2

    private struct Flow {
        var send: (@Sendable (Data) -> Void)?
        var quality: Float = 0.75
        var isEncoding = false
        var inFlight = 0
    }

    private let flow = Mutex(Flow())
    /// Only touched on `queue`.
    private var compression: VTCompressionSession?
    private var compressionSize: (Int32, Int32) = (0, 0)
    private var compressionQuality: Float = -1

    /// Starts handing JPEGs to `send`, which writes them to the PC.
    func start(sending send: @escaping @Sendable (Data) -> Void) {
        flow.withLock { flow in
            flow.send = send
            flow.inFlight = 0
        }
    }

    func stop() {
        flow.withLock { $0.send = nil }
    }

    func setQuality(_ quality: Float) {
        flow.withLock { $0.quality = quality }
    }

    /// The PC has a frame, so there is room for another.
    func frameReceived() {
        flow.withLock { $0.inFlight = max($0.inFlight - 1, 0) }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let pixels = sampleBuffer.imageBuffer else { return }
        let quality = flow.withLock { flow -> Float? in
            guard flow.send != nil, !flow.isEncoding, flow.inFlight < Self.maxInFlight else { return nil }
            flow.isEncoding = true
            return flow.quality
        }
        guard let quality else { return }
        guard let session = compressionSession(for: pixels, quality: quality) else {
            flow.withLock { $0.isEncoding = false }
            return
        }
        let status = VTCompressionSessionEncodeFrame(
            session, imageBuffer: pixels, presentationTimeStamp: sampleBuffer.presentationTimeStamp,
            duration: .invalid, frameProperties: nil, infoFlagsOut: nil
        ) { [self] status, _, encoded in
            let jpeg = status == noErr ? encoded.flatMap { try? $0.dataBuffer?.dataBytes() } : nil
            let send = flow.withLock { flow -> (@Sendable (Data) -> Void)? in
                flow.isEncoding = false
                guard jpeg != nil, let send = flow.send else { return nil }
                flow.inFlight += 1
                return send
            }
            if let jpeg, let send { send(jpeg) }
        }
        if status != noErr {
            flow.withLock { $0.isEncoding = false }
        }
    }

    private func compressionSession(for pixels: CVPixelBuffer, quality: Float) -> VTCompressionSession? {
        let width = Int32(CVPixelBufferGetWidth(pixels))
        let height = Int32(CVPixelBufferGetHeight(pixels))
        if let compression, compressionSize != (width, height) {
            VTCompressionSessionInvalidate(compression)
            self.compression = nil
        }
        if compression == nil {
            var session: VTCompressionSession?
            let status = VTCompressionSessionCreate(
                allocator: nil, width: width, height: height, codecType: kCMVideoCodecType_JPEG,
                encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                outputCallback: nil, refcon: nil, compressionSessionOut: &session)
            guard status == noErr, let session else {
                print("[stream] JPEG encoder unavailable: \(status)")
                return nil
            }
            VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
            compression = session
            compressionSize = (width, height)
            compressionQuality = -1
        }
        if let compression, quality != compressionQuality {
            VTSessionSetProperty(compression, key: kVTCompressionPropertyKey_Quality, value: NSNumber(value: quality))
            compressionQuality = quality
        }
        return compression
    }
}
