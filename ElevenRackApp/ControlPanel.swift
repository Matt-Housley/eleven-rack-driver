// ControlPanel.swift — the full control panel shown in the status-item popover.
//
// Live per-channel input/output meters, a sample-rate picker (drives CoreAudio,
// which the engine follows), MIDI status, and actions. All state comes from the
// observed DriverModel; buttons call back into the app for side effects.
//
// Layout is kept compact and lays the meters out in two columns (inputs beside
// outputs) so the popover stays short enough to fit on small screens without
// running off the top of the display.

import SwiftUI
import AppKit

/// Reports the measured height of the panel content up to the enclosing frame.
private struct ContentHeightKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

struct ControlPanel: View {
    @ObservedObject var model: DriverModel
    var onRestartEngine: () -> Void
    var onOpenAudioMIDISetup: () -> Void
    var onUninstall: () -> Void
    var onQuit: () -> Void

    @State private var contentHeight: CGFloat = 460

    /// Never let the popover be taller than the usable screen area, so its top
    /// can't run above the menu bar. It scrolls if the content is taller.
    private var maxHeight: CGFloat {
        let usable = NSScreen.main?.visibleFrame.height ?? 800
        return max(320, usable - 12)
    }

    var body: some View {
        ScrollView(.vertical) {
            panelContent
                .padding(12)
                .background(GeometryReader { geo in
                    Color.clear.preference(key: ContentHeightKey.self, value: geo.size.height)
                })
        }
        .frame(width: 360, height: min(contentHeight, maxHeight))
        .onPreferenceChange(ContentHeightKey.self) { contentHeight = $0 }
    }

    private var panelContent: some View {
        VStack(alignment: .leading, spacing: 9) {
            header
            Divider()
            sampleRateRow
            Divider()
            HStack(alignment: .top, spacing: 14) {
                meterColumn(title: "Inputs", names: ER.inputNames, levels: model.inputLevels)
                meterColumn(title: "Outputs", names: ER.outputNames, levels: model.outputLevels)
            }
            Divider()
            midiRow
            Divider()
            actions
        }
    }

    // MARK: - Sections

    private var header: some View {
        HStack(spacing: 8) {
            Circle().fill(statusColor).frame(width: 10, height: 10)
            VStack(alignment: .leading, spacing: 1) {
                Text("Eleven Rack").font(.headline)
                Text(model.status.title).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if model.dropoutWarning {
                Label("Dropouts", systemImage: "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
                    .labelStyle(.titleAndIcon)
                    .help("Audio is dropping out — the engine can't keep the buffer filled. "
                        + "Try a larger buffer size in your DAW, or a lower sample rate.")
            }
        }
    }

    private var sampleRateRow: some View {
        HStack {
            Text("Sample rate").font(.subheadline)
            Spacer()
            Picker("", selection: rateBinding) {
                ForEach(ER.sampleRates, id: \.self) { hz in
                    Text(rateLabel(hz)).tag(hz)
                }
            }
            .labelsHidden()
            .frame(width: 110)
            .disabled(!model.deviceAvailable)
        }
    }

    private func meterColumn(title: String, names: [String], levels: [Float]) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            ForEach(Array(names.enumerated()), id: \.offset) { i, name in
                MeterRow(name: name, level: i < levels.count ? levels[i] : 0)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var midiRow: some View {
        HStack(spacing: 8) {
            Image(systemName: model.midiPresent ? "pianokeys" : "pianokeys.inverse")
                .foregroundStyle(model.midiPresent ? Color.accentColor : .secondary)
            Text("MIDI")
            Spacer()
            Text(model.midiPresent ? "Connected" : "Not available")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var actions: some View {
        VStack(spacing: 8) {
            Toggle("Launch at login", isOn: launchBinding)
                .toggleStyle(.switch)
                .disabled(!LaunchAgentControl.isInstalled)
            HStack {
                Button("Audio MIDI Setup", action: onOpenAudioMIDISetup)
                Spacer()
                Button("Restart Engine", action: onRestartEngine)
            }
            HStack {
                Button("Uninstall…", role: .destructive, action: onUninstall)
                Spacer()
                Button("Quit", action: onQuit)
            }
        }
    }

    // MARK: - Helpers

    private var statusColor: Color {
        switch model.status {
        case .active: return .green
        case .idleNoDevice: return .orange
        case .notRunning: return .red
        }
    }

    private var rateBinding: Binding<UInt32> {
        Binding(get: { model.sampleRate == 0 ? 48000 : model.sampleRate },
                set: { model.setSampleRate($0) })
    }

    private var launchBinding: Binding<Bool> {
        Binding(get: { model.launchAtLogin },
                set: { model.toggleLaunchAtLogin($0) })
    }

    private func rateLabel(_ hz: UInt32) -> String {
        String(format: "%.1f kHz", Double(hz) / 1000.0)
    }
}

/// A single labelled peak meter. `level` is a linear peak in 0…1.
private struct MeterRow: View {
    let name: String
    let level: Float

    var body: some View {
        HStack(spacing: 6) {
            Text(name)
                .font(.caption2)
                .lineLimit(1)
                .minimumScaleFactor(0.75)
                .frame(width: 74, alignment: .leading)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2).fill(Color.secondary.opacity(0.18))
                    RoundedRectangle(cornerRadius: 2)
                        .fill(barColor)
                        .frame(width: max(0, geo.size.width * CGFloat(fraction)))
                }
            }
            .frame(height: 7)
        }
        .frame(height: 16)
    }

    /// Map linear peak to a 0…1 bar using a -60 dBFS floor.
    private var fraction: Float {
        guard level > 0 else { return 0 }
        let db = 20 * log10(level)
        return max(0, min(1, (db + 60) / 60))
    }

    private var barColor: Color {
        if fraction > 0.92 { return .red }
        if fraction > 0.75 { return .orange }
        return .green
    }
}
