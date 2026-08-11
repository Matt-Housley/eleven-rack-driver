// RingReader.swift — read-only observer of the shared-memory audio ring.
//
// Attaches to the ring the engine publishes and reports, per poll, whether the
// engine is pulling audio from USB, whether an app is actively consuming the
// device, and how many overruns happened since the last poll. It NEVER calls
// er_in_read/er_out_read (those move the consumer index owned by coreaudiod) —
// only field reads and the non-destructive peek helpers are used.

import Foundation

/// One poll's worth of ring activity, derived from counter deltas.
struct RingActivity {
    /// The engine is actively reading USB — the capture counter advanced, or the
    /// ring is saturating (xruns rising because nothing is consuming yet). Either
    /// way the device is connected and streaming.
    let enginePulling: Bool
    /// An app is actively *recording* — the capture consumer (inRead) is advancing.
    /// xrunCount counts capture-ring overruns, which are expected (and harmless)
    /// when nothing records the 8-channel input, so they only signal a real dropout
    /// while recording. Playback activity must NOT gate this, or plain playback
    /// would false-alarm as the idle capture ring saturates.
    let consumerActive: Bool
    /// Overruns since the previous poll.
    let xrunDelta: UInt32
    let sampleRate: UInt32
    let engineRunning: Bool
}

final class RingReader {
    private var ring: UnsafeMutablePointer<ERRing>?
    private var lastInWrite: UInt64 = 0
    private var lastInRead: UInt64 = 0
    private var lastOutWrite: UInt64 = 0
    private var lastXrun: UInt32 = 0

    /// True while a valid ring mapping is held.
    var isAttached: Bool { ring != nil }

    /// Attach to the ring if not already mapped, priming the delta baselines.
    @discardableResult
    func attachIfNeeded() -> Bool {
        if ring != nil { return true }
        ring = er_ring_attach()
        if let r = ring {
            lastInWrite = r.pointee.inWrite
            lastInRead = r.pointee.inRead
            lastOutWrite = r.pointee.outWriteMax   // time-addressed playback write head
            lastXrun = r.pointee.xrunCount
        }
        return ring != nil
    }

    /// Drop the mapping (e.g. after the engine unlinked the shared object).
    func detach() {
        if let r = ring { er_ring_close(r, 0) }
        ring = nil
        lastInWrite = 0; lastInRead = 0; lastOutWrite = 0; lastXrun = 0
    }

    /// Sample the ring and return activity since the previous poll. Returns nil
    /// (and detaches) if the ring's magic has gone stale — the engine unlinked it.
    func poll() -> RingActivity? {
        guard let r = ring else { return nil }
        if r.pointee.magic != ER_RING_MAGIC { detach(); return nil }
        let w = r.pointee.inWrite
        let rd = r.pointee.inRead
        let ow = r.pointee.outWriteMax   // playback advances the time-addressed write head
        let x = r.pointee.xrunCount
        let dW = w &- lastInWrite
        let dRd = rd &- lastInRead
        let dOW = ow &- lastOutWrite
        let dX = x &- lastXrun
        lastInWrite = w; lastInRead = rd; lastOutWrite = ow; lastXrun = x
        // Only trust capture/xrun deltas as "device streaming" when the engine says
        // it is actually running. A stale leftover ring (engine gone, engineRunning
        // == 0) can keep valid magic and a drifting garbage xrun value, which would
        // otherwise be misread as an active device.
        let running = r.pointee.engineRunning != 0
        return RingActivity(
            enginePulling: running && (dW > 0 || dX > 0),
            consumerActive: dRd > 0,   // recording only — see RingActivity doc
            xrunDelta: dX,
            sampleRate: r.pointee.sampleRate,
            engineRunning: running)
    }

    /// Live per-channel input levels (count == ER.inputNames.count). Zeros if
    /// detached. Uses the engine-published meters (live regardless of ring fill),
    /// not the buffer peek that freezes when the ring saturates with no consumer.
    func inputLevels() -> [Float] {
        var out = [Float](repeating: 0, count: Int(ER_IN_CH))
        if let r = ring {
            out.withUnsafeMutableBufferPointer { er_read_in_meters(UnsafePointer(r), $0.baseAddress) }
        }
        return out
    }

    /// Live per-channel output levels (count == ER.outputNames.count). Zeros if detached.
    func outputLevels() -> [Float] {
        var out = [Float](repeating: 0, count: Int(ER_OUT_CH))
        if let r = ring {
            out.withUnsafeMutableBufferPointer { er_read_out_meters(UnsafePointer(r), $0.baseAddress) }
        }
        return out
    }
}
