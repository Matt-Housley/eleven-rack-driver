// Uninstaller.swift — remove every installed component with a single admin
// prompt, then quit. Uses AppleScript's "with administrator privileges" so the
// user authenticates in a standard macOS dialog (no Terminal). Removes the HAL
// plugin, the LaunchAgent, and the app itself, then restarts coreaudiod so the
// device disappears immediately.

import Foundation
import AppKit

enum Uninstaller {
    /// Present a confirmation, then run the privileged removal. Returns when done.
    static func run() {
        let alert = NSAlert()
        alert.messageText = "Uninstall Eleven Rack?"
        alert.informativeText = """
        This removes the audio driver, the login item, and this app. You'll be \
        asked for your password once. Your DAW projects are not affected.
        """
        alert.addButton(withTitle: "Uninstall")
        alert.addButton(withTitle: "Cancel")
        alert.alertStyle = .warning
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        // Unload the agent from the user domain first (no admin needed).
        LaunchAgentControl.disable()

        let uid = getuid()
        let shell = """
        /bin/launchctl bootout gui/\(uid)/\(ER.bundleID) 2>/dev/null; \
        /bin/rm -f '\(ER.launchAgentPath)'; \
        /bin/rm -rf '\(ER.halPluginPath)'; \
        /bin/rm -rf '\(ER.appPath)'; \
        /usr/bin/killall coreaudiod 2>/dev/null; \
        true
        """
        // Escape for AppleScript string literal.
        let escaped = shell.replacingOccurrences(of: "\\", with: "\\\\")
                           .replacingOccurrences(of: "\"", with: "\\\"")
        let source = "do shell script \"\(escaped)\" with administrator privileges"

        var errorDict: NSDictionary?
        if let script = NSAppleScript(source: source) {
            script.executeAndReturnError(&errorDict)
        }
        if let err = errorDict {
            let a = NSAlert()
            a.messageText = "Uninstall did not complete"
            a.informativeText = (err[NSAppleScript.errorMessage] as? String) ?? "Unknown error."
            a.runModal()
            return
        }

        let done = NSAlert()
        done.messageText = "Eleven Rack uninstalled"
        done.informativeText = "The app will now quit."
        done.runModal()
        NSApp.terminate(nil)
    }
}
