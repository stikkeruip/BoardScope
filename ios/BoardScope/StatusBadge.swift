import SwiftUI

/// Lens, closest focus, heat and battery, out of the way in a corner.
struct StatusBadge: View {
    let lens: Lens
    /// Millimeters.
    let minimumFocusDistance: Int?
    let thermal: ProcessInfo.ThermalState
    let isCharging: Bool
    /// From 0 to 1, or negative when unknown.
    let batteryLevel: Float

    var body: some View {
        HStack(spacing: 10) {
            Text(lens.name)
            if let minimumFocusDistance {
                Text("focus from \(minimumFocusDistance / 10) cm")
            }
            Label(thermal.label, systemImage: "thermometer.medium")
                .foregroundStyle(thermal.color)
            HStack(spacing: 4) {
                Image(systemName: batterySymbol)
                if batteryLevel >= 0 {
                    Text("\(Int((batteryLevel * 100).rounded()))%")
                }
            }
            .foregroundStyle(batteryColor)
        }
        .font(.caption.weight(.medium))
        .foregroundStyle(.white)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(.black.opacity(0.5), in: Capsule())
    }

    private var batterySymbol: String {
        if isCharging { return "battery.100percent.bolt" }
        switch batteryLevel {
        case ..<0.13: return "battery.0percent"
        case ..<0.38: return "battery.25percent"
        case ..<0.63: return "battery.50percent"
        case ..<0.88: return "battery.75percent"
        default: return "battery.100percent"
        }
    }

    /// Orange when unplugged, since the phone should stay on the charger for a session.
    private var batteryColor: Color {
        if isCharging { return .green }
        return batteryLevel >= 0 && batteryLevel < 0.2 ? .red : .orange
    }
}

extension ProcessInfo.ThermalState {
    var label: String {
        switch self {
        case .nominal: "Cool"
        case .fair: "Warm"
        case .serious: "Hot · 15 fps"
        case .critical: "Too hot"
        @unknown default: "Unknown"
        }
    }

    var color: Color {
        switch self {
        case .nominal: .green
        case .fair: .yellow
        case .serious: .orange
        case .critical: .red
        @unknown default: .gray
        }
    }
}
