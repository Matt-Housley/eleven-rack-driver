// main.swift — menu-bar app entry point.
//
// A UI-less (LSUIElement) agent app: it supervises the USB engine, shows a
// status item, and presents the control panel. No Dock icon, no main window.

import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let engine = EngineController()
    private var model: DriverModel!
    private var status: StatusController!

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)

        model = DriverModel(engine: engine)
        status = StatusController(
            model: model,
            onRestartEngine: { [engine] in engine.restart() },
            onOpenAudioMIDISetup: { Self.openAudioMIDISetup() },
            onUninstall: { Uninstaller.run() },
            onQuit: { NSApp.terminate(nil) })

        engine.start()
        model.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        model?.stop()
        engine.stop()
    }

    private static func openAudioMIDISetup() {
        let url = URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app")
        NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
