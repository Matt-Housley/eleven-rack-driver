# Playback Dropout Analysis & Fix Design

**Status:** root cause confirmed; fix **not yet implemented**. Do **not** ship a
"dropout fix" release until the fix below is built and verified with the harness
described here.

## Symptom

Used as a general-purpose output (e.g. streaming from Apple Music), playback has
audible dropouts — clicks, pops, and short (~1 s) silences, worst right after a
track skip. Present since 1.0.0; it is the issue the user originally reported. The
menu-bar "Dropouts" indicator also lights, but that is partly a separate false
alarm (see below).

## Confirmed root cause (refined — the trigger is a SECOND audio client)

**Single-client playback is clean. A second client opening the device degrades
coreaudiod's delivery ~5%, and it latches until a full IO restart.**

Measured live with `rawmon` (per-second `outWrite`/`outRead` deltas), clean 1.0.1
baseline, post-reboot:

| Phase | coreaudiod delivery (`dOut`) | Audio |
|---|---|---|
| Music only (single client), steady, incl. track skips | **~48000–48288 f/s**, backlog stable ~96 | **clean** |
| The instant Safari opens/loads YouTube (2nd client attaches) | collapses to ~1300–23000 f/s for ~5 s | ~5 s of noise, then silence |
| Settled with the 2nd client present | **~45500 f/s** (steady ~5% deficit), backlog ~0 | continuous dropouts |
| After quitting Safari + Music pause/play | **still ~45000 f/s** (latched) | still broken |

So it is **not** a steady clock deficit (the earlier "~4%" reading came from
perturbed test setups — a manually-run engine and diagnostic `os_log`/`printf`).
With one client at the device rate coreaudiod delivers a clean 48000 f/s. The
fault is triggered when a **second client** opens the device (Safari/YouTube, even
before playing): coreaudiod's multi-client mix/scheduling for this virtual device
drops ~5% of delivery, the ~0-backlog ring underruns continuously, and the state
**does not reset** when the second client leaves — only a full device IO restart
(switch output away and back, replug, or restart coreaudiod) clears it.

Contributing factor: the output IO path has almost **zero timing headroom** — a
single per-second `os_log` on the plug-in's `DoIOOperation` collapsed delivery to
~8 k f/s. The plug-in's `GetZeroTimeStamp` is a free-running `mach_absolute_time`
nominal clock, decoupled from the device's real clock; coreaudiod has no accurate,
stable hardware reference, which is what makes its multi-client rate handling
degrade here.

**Original user report maps exactly:** "streaming from Music … switching to Safari
while that is playing, it really drops out." That is this bug.

## What was ruled out (all disproven by measurement)

1. **Sample-rate changes / the `reconfigure()` path** — rates never changed during
   the dropouts (`hwRate`/`ringRate` stayed 48000). Not the cause.
2. **CPU load** — user confirmed it is not load-related; it correlates with track
   skips, not CPU.
3. **The USB engine** — healthy throughout: full in-flight pool (`inflight=64`),
   zero re-arm failures, isoc frames scheduled correctly *ahead* of the bus
   (`frameLag ≈ +63`). The engine is not at fault.
4. **A clock-drift servo in `GetZeroTimeStamp`** (level servo forcing a backlog
   target) — catastrophic: drove coreaudiod to ~half rate. Reverted.
5. **A non-zero `kAudioDevicePropertySafetyOffset`** (256, then 1024) — does *not*
   build a ring cushion in this architecture (the plug-in just appends to the ring
   regardless of the reported offset). Backlog stayed ~0. Reverted.
6. **An engine-side prebuffer** (hold off consuming until the ring builds a
   cushion) — the cushion cannot hold because delivery is genuinely short; it
   drains in ~0.5 s and re-primes, emitting silence. Confirmed the deficit is a
   sustained rate shortfall, not mere jitter. Reverted.
7. **Larger IO buffers** (min 512 / default 1024; coreaudiod actually used 96 then
   384 frames) — `underrunFr` stayed ~2200/s at every buffer size. Not a per-cycle
   overhead problem. Reverted.

## Secondary issue: the "Dropouts" indicator is partly a false alarm

The engine increments `xrunCount` on every capture-ring overrun. When no app
records the 8-channel input (the normal case for plain playback), the capture ring
saturates and overruns **every frame (~48000/s)**. The menu-bar app then reports
"Dropouts" even when playback is fine. When fixing the real issue, also:
- stop counting capture overruns as dropouts when there is no input consumer, and
- base the UI warning on genuine **playback** underruns, not the global
  `xrunCount`.

(An unrelated, already-shipped-in-1.0.2 menu-bar fix: the icon no longer shows
"Active" for a stale ring — it now trusts `engineRunning`. See
`RingReader.swift` / `DriverModel.swift`.)

## Fix design (the real work)

> **Update:** Option B (hardware-referenced timeline) has been **tried and
> disproven** — the multi-client deficit is timeline-independent (see above). The
> remaining candidates are Option A (engine-side async SRC) and, more likely
> needed, a **direct investigation of why coreaudiod's multi-client mixer
> under-delivers to this AudioServerPlugIn device** (Option C).

### Option C — Understand coreaudiod's multi-client behavior (do this first)

The deficit appears only with a second client and is unaffected by every driver
timeline/buffer/offset change tried. Before writing more driver code, determine
what coreaudiod actually does when the second client attaches:
- Compare against a reference virtual driver (BlackHole) on the same Mac: does it
  also drop ~5% with two clients, or stay clean? If BlackHole is clean, diff its
  AudioServerPlugIn property/format/latency reporting against ours — we are
  missing part of the contract coreaudiod's mixer relies on.
- Capture `HALS`/coreaudiod internal logs at the moment the second client attaches
  (client-add, format negotiation, IO restart, overload).
- Check whether the second client opens the device at a different stream format /
  rate and whether coreaudiod inserts an ASRC whose ratio is wrong.

### Option A — Asynchronous sample-rate conversion in the engine

Make the engine resample coreaudiod's playback stream from its delivered rate to
the device's true rate, driven by a control loop on the ring fill level:

- Track the playback ring backlog. Run a slow control loop that estimates the
  ratio between coreaudiod's delivery rate and the device's consumption rate.
- Replace the fixed output accordion (`gOutAccum += gHwRate/8000.0`) with a
  resampler (e.g. a good windowed-sinc or at least a high-quality cubic) whose
  ratio is the estimated clock ratio, so the engine emits exactly what the device
  needs while consuming exactly what coreaudiod provides on average.
- This is the standard USB-audio playback approach (adaptive/async endpoints).
- Do the same, symmetrically, for capture if capture drift ever matters.

### Option B — Hardware-referenced timeline in the plug-in — DISPROVEN

Reworking `GetZeroTimeStamp` to lock Core Audio's clock to `hwFrames` (both raw
and smoothed/phase-locked) did **not** change the multi-client deficit and hurt
single-client audio. Kept only as infrastructure (`hwFrames`). Do not pursue as
the multi-client fix.

### The deficit is timeline-independent (decisive)

A hardware-clock counter (`hwFrames`) was added to the ring (engine publishes the
true device-frame count every captured frame; advances at ~48096/s even when
nothing records). `GetZeroTimeStamp` was then reworked to lock Core Audio's clock
to it, two ways:

- **Raw hardware timestamps** (report the true position at the true host time):
  coreaudiod's rate estimate collapsed → it nearly stopped producing (`dOut`~0),
  audible as constant noise. Too jittery.
- **Smooth phase-locked timeline** (nominal-rate anchor with a gentle, clamped
  pull toward `hwFrames`): single-client became stuttery, and — decisively —
  **the multi-client deficit was unchanged: `dOut` ≈ 45500 with a second client
  open, exactly as under the nominal timeline.**

**Conclusion: the ~5% multi-client deficit does not depend on what clock/timeline
the driver reports.** No `GetZeroTimeStamp` change moves it. The hardware-clock
infrastructure (ring `hwFrames` + engine publisher) is kept in the tree for a
future attempt, but the plug-in reverted to the nominal timeline (clean
single-client). This means Option B (hardware-referenced timeline) is **disproven**
as a fix for the multi-client case.

### Reliable reproduction (use this as the test harness)

1. Plug in the Eleven Rack, set it as the default output.
2. Play a track in Music — confirm clean (`rawmon` `dOut ≈ 48000`).
3. Open Safari and load a YouTube page (no need to play it).
4. Observe: `dOut` collapses for ~5 s then settles at ~45500; audio drops out.
5. Quit Safari — `dOut` stays ~45000 (latched). Switch output away/back or replug
   to reset.

A fix must keep `dOut ≈ 48000` (clean audio) **with a second client open**.

### Open question to resolve first

Why does a second client drop delivery ~5% and latch it? Likely coreaudiod's
multi-client mixer/ASRC scheduling for this device, made brittle by the
free-running (non-hardware-referenced) timeline and zero timing headroom. Before
implementing, instrument the plug-in's `DoIOOperation` **without** `os_log` on the
RT thread (mmap'd counters or ring header fields — increment only, no logging) and
compare single- vs multi-client: is coreaudiod producing short buffers, dropping
whole IO cycles, or resampling at a wrong ratio? That decides Option A vs B. Also
worth checking `HALS`/coreaudiod overload logs while the second client attaches.

## Measurement harness (rebuild as needed)

Diagnostic tools used this session (kept out of the shipping build):

- **`rawmon`** — attaches to the shared ring, prints per-second `inWrite/inRead/
  outWrite/outRead` deltas. `dOut` = coreaudiod's true production rate; `dOutRd` =
  engine drain; backlog = `outWrite − outRead`. The cleanest, non-perturbing
  measurement.
- **Engine `[diag]` line** — a per-second dump (added to `rateFollow`) of
  `hwRate/ringRate/inflight/outBacklog/rearmFail/underrunFr/isocErr/frameLag`.
  `underrunFr` is the key number (playback frames the engine had to silence-fill).
- **`setbuf <frames>`** — sets `kAudioDevicePropertyBufferFrameSize` live to probe
  buffer-size effects.
- **`verify_fix`** — reads the device's Core Audio properties (safety offset,
  nominal rate) plus a ring watch.

Reintroduce these behind a build flag or in `ElevenRackBridge/` tools; do **not**
put `os_log`/`printf` on coreaudiod's IO thread — it perturbs the very thing being
measured.

## Acceptance criteria before shipping a dropout fix

- `underrunFr` ≈ 0 sustained during steady playback.
- Ring backlog stable (not chronically 0, not oscillating 0↔full).
- Clean audio through several minutes of playback **and** repeated track skips
  (including Lossless/Hi-Res tracks at different rates).
- No regression to capture, MIDI, rate-follow, or DAW use.
