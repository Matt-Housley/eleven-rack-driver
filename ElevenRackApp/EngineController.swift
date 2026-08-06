// EngineController.swift — spawns and supervises the bundled `erengine`.
//
// The LaunchAgent keeps this app alive; this app keeps the USB engine alive. The
// engine binary ships inside the app bundle (Contents/Resources/erengine) and is
// run in its default bridge mode. If it exits — including the case where the
// Eleven Rack is unplugged, which makes erengine return immediately — it is
// respawned after a short backoff, so the device "just works" once reconnected.

import Foundation

final class EngineController {
    private var process: Process?
    private var shouldRun = false
    private var lastLaunch = Date.distantPast
    private let queue = DispatchQueue(label: "co.uk.matthousley.ElevenRack.engine")

    /// PID of the running engine, or nil.
    private(set) var pid: Int32?

    /// Whether supervision is active (start() called, stop() not yet). Read from
    /// the UI thread for status; a benign race with the engine queue is fine.
    private(set) var supervising = false

    /// Whether the bundled engine binary exists and is runnable.
    var isEngineAvailable: Bool {
        guard let u = engineURL else { return false }
        return FileManager.default.isExecutableFile(atPath: u.path)
    }

    /// Location of the bundled engine binary.
    private var engineURL: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("erengine")
    }

    /// Begin supervising: launch now and respawn on exit until `stop()`.
    func start() {
        queue.async {
            self.shouldRun = true
            self.supervising = true
            self.killStrayEngines()   // reclaim the USB device from any orphaned engine
            self.launch()
        }
    }

    /// Stop supervising and terminate the engine (no respawn).
    func stop() {
        queue.async {
            self.shouldRun = false
            self.supervising = false
            self.process?.terminationHandler = nil
            self.process?.terminate()
            self.process = nil
            self.pid = nil
        }
    }

    /// Kill the current engine; supervision respawns it immediately.
    func restart() {
        queue.async {
            guard self.shouldRun else { return }
            self.lastLaunch = .distantPast   // bypass backoff for a manual restart
            self.process?.terminate()        // terminationHandler relaunches
        }
    }

    // MARK: - Private

    private func launch() {
        guard shouldRun, process == nil, let url = engineURL,
              FileManager.default.isExecutableFile(atPath: url.path) else { return }

        let p = Process()
        p.executableURL = url
        p.arguments = []                     // default bridge mode
        if let log = openLog() {
            p.standardOutput = log
            p.standardError = log
        }
        p.terminationHandler = { [weak self] _ in
            guard let self = self else { return }
            self.queue.async {
                self.process = nil
                self.pid = nil
                guard self.shouldRun else { return }
                // Backoff: if it died within 3 s of launching (e.g. device absent),
                // wait before retrying so we don't spin; otherwise retry promptly.
                let alive = Date().timeIntervalSince(self.lastLaunch)
                let delay = alive < 3.0 ? 2.0 : 0.5
                self.queue.asyncAfter(deadline: .now() + delay) { self.launch() }
            }
        }
        do {
            try p.run()
            process = p
            pid = p.processIdentifier
            lastLaunch = Date()
        } catch {
            NSLog("ElevenRack: failed to launch engine: \(error)")
            queue.asyncAfter(deadline: .now() + 2.0) { self.launch() }
        }
    }

    /// Terminate any engine processes not owned by this app — e.g. one orphaned
    /// when a previous app instance was force-quit (a LaunchAgent reload, logout,
    /// crash). An orphan keeps the USB device claimed and would block our own
    /// engine. Uses SIGTERM (never -9) so the stray runs its teardown and releases
    /// the USB device, then waits (bounded) for it to actually exit before we
    /// launch a replacement.
    private func killStrayEngines() {
        guard let path = engineURL?.path else { return }
        run("/usr/bin/pkill", ["-f", path])
        // Wait up to ~2 s for strays to exit and release the device.
        for _ in 0..<20 {
            if run("/usr/bin/pgrep", ["-f", path]) != 0 { return }   // none left
            usleep(100_000)
        }
    }

    /// Run a tool synchronously; returns its exit status (or -1 on failure).
    @discardableResult
    private func run(_ tool: String, _ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }

    private func openLog() -> FileHandle? {
        let fm = FileManager.default
        guard let logs = fm.urls(for: .libraryDirectory, in: .userDomainMask).first?
            .appendingPathComponent("Logs/ElevenRack", isDirectory: true) else { return nil }
        try? fm.createDirectory(at: logs, withIntermediateDirectories: true)
        let file = logs.appendingPathComponent("engine.log")
        if !fm.fileExists(atPath: file.path) { fm.createFile(atPath: file.path, contents: nil) }
        guard let h = try? FileHandle(forWritingTo: file) else { return nil }
        h.seekToEndOfFile()
        return h
    }
}
