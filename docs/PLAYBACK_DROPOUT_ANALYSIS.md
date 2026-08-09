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

## Confirmed root cause

**coreaudiod delivers playback ~4% slower than the device consumes it.**

Measured with the device streaming and a track playing steadily:

| Quantity | Value |
|---|---|
| Capture production (`inWrite` delta) — the true hardware clock | ~48096 frames/s |
| Playback delivery by coreaudiod (`outWrite` delta) | **~45000–46000 frames/s** |
| Engine consumption (fixed accordion `gHwRate/8000`) | 48000 frames/s |
| Steady playback underrun (`underrunFr`) | **~2000–3000 frames/s (~4–6%)** |
| Playback ring backlog (`outWrite − outRead`) | ~0 (chronically empty) |

The USB engine consumes the playback ring at the device's hardware rate (~48 kHz,
bus-paced). coreaudiod fills it from its own software timeline at ~46 kHz
effective. The ~2000 frames/s shortfall is filled with silence every second →
continuous glitching, and a burst of silence whenever coreaudiod stalls (track
skips). The ring runs at ~0 backlog, so there is **zero slack**: any hiccup is
instantly audible. Adding a single `os_log` per second to the plug-in's
`DoIOOperation` collapsed delivery from ~46 k to ~8 k frames/s — the output IO
path has essentially no timing headroom.

This is a **clock-domain mismatch**: the plug-in's `GetZeroTimeStamp` timeline is
a free-running `mach_absolute_time`-based nominal clock, completely decoupled from
the engine's real consumption of the ring. Nothing reconciles the two clocks.

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

The producer (coreaudiod, software clock) and consumer (device, hardware clock)
are in **different clock domains** and must be reconciled. Two options:

### Option A — Asynchronous sample-rate conversion in the engine (recommended)

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

### Option B — Hardware-referenced timeline in the plug-in

Make `GetZeroTimeStamp` report a timeline **derived from the engine's actual ring
consumption** (`outRead` progress vs host time) via a proper DLL, so coreaudiod
locks its production rate to the device's true rate. This is what BlackHole /
Apple's `SimpleAudioDriver` effectively do. Note the earlier failed servo was a
*level* servo; this must be a *frequency-lock DLL* on real consumption, smoothed
and clamped. Higher risk (it's on coreaudiod's RT path) but no resampling.

Option A keeps the risky change out of coreaudiod's RT thread and is the more
conventional fix; prefer it unless measurement shows the deficit is purely a
reportable-rate issue (it did not appear to be — coreaudiod under-delivered even
against a correct nominal timeline).

### Open question to resolve first

Why is the deficit ~4% rather than the ~0.2% implied by the measured hardware
clock (48096 vs 48000)? ~4% is too large for crystal drift. Before implementing,
instrument the plug-in's `DoIOOperation` **without** `os_log` on the RT thread
(write counters to an mmap'd file or add ring header fields) and confirm whether
coreaudiod is (a) producing short buffers, (b) dropping whole IO cycles, or (c)
being throttled. The answer decides whether Option A alone suffices.

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
