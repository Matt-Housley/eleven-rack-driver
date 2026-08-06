// StatusController.swift — the menu-bar status item and its popover.
//
// The menu-bar glyph is the Eleven Rack pick as a *template* image, so macOS
// auto-inverts it for light/dark menu bars. State is shown without breaking that
// inversion: full strength = active, dimmed = device not connected, red = engine
// not running. Clicking opens the full control panel.
//
// Dismissal: a plain `.transient` popover passes the outside "dismiss" click
// through to whatever is under the cursor — clicking the desktop then triggers
// macOS's reveal-desktop. Native menu-bar menus instead *consume* that click. To
// match them we use `.applicationDefined` dismissal plus an invisible full-screen
// click-catcher window behind the popover: an outside click lands on the catcher
// (closing the popover) instead of falling through to the desktop.

import AppKit
import SwiftUI
import Combine

/// Transparent full-screen view that closes the popover when clicked.
private final class ClickCatcherView: NSView {
    var onClick: () -> Void = {}
    override func mouseDown(with event: NSEvent) { onClick() }
    override func rightMouseDown(with event: NSEvent) { onClick() }
}

final class StatusController {
    private let statusItem: NSStatusItem
    private let popover = NSPopover()
    private let model: DriverModel
    private var cancellable: AnyCancellable?
    private var clickCatcher: NSWindow?
    private var resignObserver: Any?

    init(model: DriverModel,
         onRestartEngine: @escaping () -> Void,
         onOpenAudioMIDISetup: @escaping () -> Void,
         onUninstall: @escaping () -> Void,
         onQuit: @escaping () -> Void) {
        self.model = model
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        let panel = ControlPanel(model: model,
                                 onRestartEngine: onRestartEngine,
                                 onOpenAudioMIDISetup: onOpenAudioMIDISetup,
                                 onUninstall: onUninstall,
                                 onQuit: onQuit)
        popover.behavior = .applicationDefined     // we manage dismissal (see above)
        let hosting = NSHostingController(rootView: panel)
        hosting.sizingOptions = [.preferredContentSize]   // popover tracks the SwiftUI size
        popover.contentViewController = hosting

        if let button = statusItem.button {
            button.image = Self.menuBarImage()
            button.imagePosition = .imageOnly
            button.action = #selector(togglePopover)
            button.target = self
        }

        apply(status: model.status)
        cancellable = model.$status.receive(on: RunLoop.main).sink { [weak self] in self?.apply(status: $0) }
    }

    /// Load the pick template from the bundle, sized for the menu bar. Falls back
    /// to an SF Symbol if the asset is missing (e.g. running un-bundled).
    private static func menuBarImage() -> NSImage? {
        let height: CGFloat = 18
        if let url = Bundle.main.resourceURL?.appendingPathComponent("MenuBarIcon.png"),
           let img = NSImage(contentsOf: url) {
            let aspect = img.size.width / max(img.size.height, 1)
            img.size = NSSize(width: height * aspect, height: height)
            img.isTemplate = true
            return img
        }
        let fallback = NSImage(systemSymbolName: "guitars", accessibilityDescription: "Eleven Rack")
        fallback?.isTemplate = true
        return fallback
    }

    private func apply(status: DriverModel.Status) {
        guard let button = statusItem.button else { return }
        switch status {
        case .active:
            button.contentTintColor = nil      // template auto-inverts black/white
            button.alphaValue = 1.0
            button.toolTip = "Eleven Rack — Active"
        case .idleNoDevice:
            button.contentTintColor = nil      // still inverts, just dimmed
            button.alphaValue = 0.45
            button.toolTip = "Eleven Rack — device not connected"
        case .notRunning:
            button.contentTintColor = .systemRed
            button.alphaValue = 1.0
            button.toolTip = "Eleven Rack — engine not running"
        }
    }

    @objc private func togglePopover() {
        if popover.isShown { closePopover() } else { openPopover() }
    }

    private func openPopover() {
        guard let button = statusItem.button else { return }
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        NSApp.activate(ignoringOtherApps: true)
        installClickCatcher()
        // Make the popover the key window so its controls render in the active
        // (bright) appearance rather than the dimmed inactive one. App activation
        // is asynchronous, so key it explicitly for a consistent look every time.
        popover.contentViewController?.view.window?.makeKey()

        // Also dismiss if the app is deactivated another way (Cmd-Tab, etc.).
        resignObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didResignActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in self?.closePopover() }
    }

    private func closePopover() {
        popover.performClose(nil)
        clickCatcher?.orderOut(nil)
        clickCatcher = nil
        if let obs = resignObserver { NotificationCenter.default.removeObserver(obs); resignObserver = nil }
    }

    /// Place an invisible window spanning all screens just beneath the popover, so
    /// a click anywhere outside the popover closes it without reaching the desktop.
    private func installClickCatcher() {
        guard clickCatcher == nil else { return }
        let frame = NSScreen.screens.reduce(NSRect.zero) { $0.union($1.frame) }
        let win = NSWindow(contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false)
        win.isOpaque = false
        win.backgroundColor = .clear
        win.hasShadow = false
        win.ignoresMouseEvents = false
        let view = ClickCatcherView(frame: frame)
        view.onClick = { [weak self] in self?.closePopover() }
        win.contentView = view

        // Sit just below the popover window (above every app window and the desktop).
        let popLevel = popover.contentViewController?.view.window?.level ?? .popUpMenu
        win.level = NSWindow.Level(rawValue: popLevel.rawValue - 1)
        win.orderFront(nil)
        popover.contentViewController?.view.window?.orderFrontRegardless()
        clickCatcher = win
    }
}
