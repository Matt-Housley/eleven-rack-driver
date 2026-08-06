// LaunchAgentControl.swift — enable/disable "Launch at Login" via the per-user
// LaunchAgent the installer places in /Library/LaunchAgents. Bootstrapping a
// LaunchAgent into your own GUI domain needs no admin rights, so this toggles
// without a password prompt. If the plist is missing (e.g. running the app
// un-installed during development) the control reports/does nothing.

import Foundation

enum LaunchAgentControl {
    static var isInstalled: Bool {
        FileManager.default.fileExists(atPath: ER.launchAgentPath)
    }

    /// True if the agent is currently loaded in this user's GUI domain.
    static var isEnabled: Bool {
        guard isInstalled else { return false }
        let out = launchctl(["print", "gui/\(getuid())/\(ER.bundleID)"])
        return out.exitCode == 0
    }

    /// Load the agent (RunAtLoad launches the app now and at each login).
    @discardableResult
    static func enable() -> Bool {
        guard isInstalled else { return false }
        return launchctl(["bootstrap", "gui/\(getuid())", ER.launchAgentPath]).exitCode == 0
    }

    /// Unload the agent (app no longer auto-starts at login).
    @discardableResult
    static func disable() -> Bool {
        guard isInstalled else { return false }
        return launchctl(["bootout", "gui/\(getuid())/\(ER.bundleID)"]).exitCode == 0
    }

    // MARK: - Private

    @discardableResult
    private static func launchctl(_ args: [String]) -> (exitCode: Int32, output: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do { try p.run() } catch { return (-1, "\(error)") }
        p.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return (p.terminationStatus, String(data: data, encoding: .utf8) ?? "")
    }
}
