// Constants.swift — install-layout paths and identifiers shared across the app.

import Foundation

enum ER {
    /// Bundle identifier / LaunchAgent label.
    static let bundleID = "co.uk.matthousley.ElevenRack"

    /// Where the installer places the per-user LaunchAgent.
    static let launchAgentPath = "/Library/LaunchAgents/\(bundleID).plist"

    /// Installed locations the uninstaller removes.
    static let halPluginPath = "/Library/Audio/Plug-Ins/HAL/Eleven Rack.driver"
    static let appPath = "/Applications/Eleven Rack.app"

    /// Core Audio device name published by the HAL plugin.
    static let deviceName = "Eleven Rack"

    /// Channel labels (must match the map documented in ERAudioRing.h).
    static let inputNames = [
        "Guitar In", "Mic In", "Eleven Rig L", "Eleven Rig R",
        "Digital In L", "Digital In R", "Line In L", "Line In R",
    ]
    static let outputNames = [
        "Main Out L", "Main Out R", "Re-Amp L", "Re-Amp R",
        "Digital Out L", "Digital Out R",
    ]

    /// Sample rates the hardware supports.
    static let sampleRates: [UInt32] = [44100, 48000, 88200, 96000]
}
