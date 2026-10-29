import SwiftUI

/// The few settings that aren't zoom or focus, kept off the main screen.
struct SettingsView: View {
    let camera: CameraController
    let server: StreamServer
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Lens", selection: Binding(get: { camera.lens }, set: { lens in Task { await camera.select(lens) } })) {
                        ForEach(Lens.allCases) { Text($0.name).tag($0) }
                    }
                } footer: {
                    if let distance = camera.minimumFocusDistance {
                        Text("The \(camera.lens.name.lowercased()) camera focuses from \(distance / 10) cm.")
                    }
                }
                Section {
                    Picker("Shutter", selection: Binding(get: { camera.shutter }, set: { shutter in Task { await camera.setShutter(shutter) } })) {
                        ForEach(Shutter.allCases) { Text($0.label).tag($0) }
                    }
                    Button("Measure light again") {
                        Task { await camera.meterLight() }
                    }
                } footer: {
                    Text("Europe uses 50 Hz. Measure again after changing the lights.")
                }
                Section {
                    LabeledContent("JPEG quality", value: "\(Int((server.quality * 100).rounded()))%")
                    Slider(value: Binding(get: { server.quality }, set: { server.setQuality($0) }), in: 0.3...0.95)
                } header: {
                    Text("PC stream")
                } footer: {
                    Text("The PC viewer connects over the USB cable. Lower quality keeps the picture smooth on a slow cable; the PC can change it too.")
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                Button("Done") { dismiss() }
            }
        }
    }
}
