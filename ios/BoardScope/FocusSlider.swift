import SwiftUI

/// Vertical focus control: drag up to focus farther, down to focus closer.
/// Slow drags move in fine steps.
struct FocusSlider: View {
    /// Lens position, from 0 (closest) to 1 (farthest).
    let position: Float
    let onChange: (Float) -> Void
    let onEnd: () -> Void

    @State private var lastDrag: (y: CGFloat, time: Date)?
    private let thumbSize: CGFloat = 30

    var body: some View {
        GeometryReader { geometry in
            let travel = geometry.size.height - thumbSize
            ZStack(alignment: .top) {
                Capsule()
                    .fill(.black.opacity(0.4))
                    .frame(width: 6)
                Circle()
                    .fill(.white)
                    .frame(width: thumbSize, height: thumbSize)
                    .shadow(radius: 3)
                    .offset(y: (1 - CGFloat(position)) * travel)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { drag in
                        if let lastDrag {
                            let dy = drag.location.y - lastDrag.y
                            let speed = abs(dy) / max(drag.time.timeIntervalSince(lastDrag.time), 0.001)
                            // Full speed above 800 pt/s; slow drags move as little as 8% as far.
                            let gain = min(max(speed / 800, 0.08), 1)
                            onChange(position - Float(dy / travel * gain))
                        }
                        lastDrag = (drag.location.y, drag.time)
                    }
                    .onEnded { _ in
                        lastDrag = nil
                        onEnd()
                    }
            )
        }
        .frame(width: 64)
        .overlay(alignment: .top) { endLabel("Far").offset(y: -22) }
        .overlay(alignment: .bottom) { endLabel("Near").offset(y: 22) }
    }

    private func endLabel(_ text: String) -> some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .foregroundStyle(.white)
            .shadow(radius: 2)
    }
}
