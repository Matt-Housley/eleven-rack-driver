/**
 * @file ERAudioRing.h
 * @brief Shared-memory audio transport between the user-space USB engine and the
 *        Core Audio HAL plugin (an @c AudioServerPlugin loaded by @c coreaudiod).
 *
 * No kernel driver, no system extension: the app owns the USB device and streams
 * audio, while the HAL plugin — running inside @c coreaudiod — exchanges float32
 * frames with the app through two lock-free single-producer/single-consumer rings
 * held in a POSIX shared-memory object.
 *
 * - **Input ring**  — capture:  app/USB is producer, @c coreaudiod is consumer (8 ch).
 * - **Output ring** — playback: @c coreaudiod is producer, app/USB is consumer (6 ch).
 *
 * Eleven Rack channel map (interleaved, one 32-bit word per channel per frame):
 * - Inputs (8): 0 Guitar In, 1 Mic In, 2 Eleven Rig L, 3 Eleven Rig R,
 *               4 Digital In L, 5 Digital In R, 6 Line In L, 7 Line In R
 * - Outputs (6): 0 Main Out L, 1 Main Out R, 2 Re-Amp L, 3 Re-Amp R,
 *                4 Digital Out L, 5 Digital Out R
 *
 * @note This header compiles as **both C and C++** (the USB engine is C, the plugin
 *       is C++). Shared fields are plain integers with identical layout in both
 *       languages; atomicity is provided by Clang/GCC @c __atomic builtins rather
 *       than @c _Atomic / @c std::atomic. All shared-memory specifics live behind
 *       ::er_ring_create / ::er_ring_attach / ::er_ring_close so the transport can
 *       be swapped later without touching callers.
 */

#ifndef ER_AUDIO_RING_H
#define ER_AUDIO_RING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define ER_RING_MAGIC    0x33314552u        /**< Header magic ('R','E','1','3') identifying the v3 layout. */
#define ER_RING_VERSION  3u                 /**< Shared-layout version; bump on any struct change. */
#define ER_RING_NAME     "/ElevenRackAudioRing" /**< POSIX shared-memory object name. */
#define ER_IN_CH         8u                 /**< Capture channel count (device → Core Audio). */
#define ER_OUT_CH        6u                 /**< Playback channel count (Core Audio → device). */
#define ER_RING_FRAMES   32768u             /**< Ring capacity in frames (power of two; ~0.68 s at 48 kHz). */
#define ER_RING_MASK     (ER_RING_FRAMES - 1u) /**< Mask for wrapping a frame counter to a ring index. */

/**
 * @brief Shared-memory layout mapped by both the engine and the plugin.
 *
 * The header fields are control/diagnostic state; the two float arrays are the
 * capture and playback ring buffers. Frame counters are monotonic (they never
 * wrap in practice); a ring index is @c counter @c & @c ::ER_RING_MASK.
 *
 * @warning Do not reorder or resize fields without bumping ::ER_RING_VERSION and
 *          ::ER_RING_MAGIC — a stale mapping from another build must be rejected.
 */
typedef struct {
    uint32_t magic;               /**< ::ER_RING_MAGIC once initialized; published last. */
    uint32_t version;             /**< ::ER_RING_VERSION of the creating build. */
    uint32_t sampleRate;          /**< Current sample rate in Hz; the plugin publishes the Core Audio rate. */
    uint32_t streamingRequested;  /**< Plugin sets 1 between StartIO and StopIO, else 0 (power hint). */
    uint32_t engineRunning;       /**< Engine sets 1 while USB streaming is live. */
    uint32_t xrunCount;           /**< Diagnostics: count of producer overruns (dropped frames). */
    uint32_t inChannels;          /**< Sanity copy of ::ER_IN_CH. */
    uint32_t outChannels;         /**< Sanity copy of ::ER_OUT_CH. */

    uint64_t inWrite;             /**< Capture producer (app) write counter, in frames. */
    uint64_t inRead;              /**< Capture consumer (coreaudiod) read counter, in frames. */
    uint64_t outWrite;            /**< Playback producer (coreaudiod) write counter, in frames. */
    uint64_t outRead;             /**< Playback consumer (app) read counter, in frames. */

    float in [ER_RING_FRAMES * ER_IN_CH];   /**< Capture ring: interleaved ::ER_IN_CH-wide float frames. */
    float out[ER_RING_FRAMES * ER_OUT_CH];  /**< Playback ring: interleaved ::ER_OUT_CH-wide float frames. */

    /**
     * @name Live metering
     * Per-channel decaying peak (linear 0..1) published by the engine every
     * captured/played frame, independent of the ring fill state — so a UI can
     * show live levels even when nothing is draining the ring. Written only by
     * the engine; read by observers. Non-atomic (a stale read is harmless).
     * @{
     */
    float inLevel [ER_IN_CH];   /**< Live input peak per capture channel. */
    float outLevel[ER_OUT_CH];  /**< Live output peak per playback channel. */
    /** @} */
} ERRing;

/**
 * @name Atomic accessors
 * Acquire/release helpers usable from both C and C++ (Clang/GCC builtins).
 * @{
 */
/** @brief Atomically load a 64-bit counter with acquire ordering. */
static inline uint64_t er_load(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
/** @brief Atomically store a 64-bit counter with release ordering. */
static inline void     er_store(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
/** @brief Atomically load a 32-bit field with acquire ordering. */
static inline uint32_t er_load32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
/** @brief Atomically store a 32-bit field with release ordering. */
static inline void     er_store32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
/** @} */

/**
 * @brief Create or open the shared ring and map it into the caller's address space.
 *
 * If the shared object does not exist it is created, sized, and initialized;
 * otherwise it is opened and (if its magic is stale) re-initialized. Typically
 * called by the engine.
 *
 * @param[out] created  Optional. Set to non-zero if this call created a fresh
 *                       object, zero if it opened an existing one. May be @c NULL.
 * @return Pointer to the mapped ::ERRing, or @c NULL on failure.
 */
static inline ERRing *er_ring_create(int *created) {
    int firstTime = 0;
    int fd = shm_open(ER_RING_NAME, O_RDWR, 0666);
    if (fd < 0) {
        fd = shm_open(ER_RING_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) { fd = shm_open(ER_RING_NAME, O_RDWR, 0666); if (fd < 0) return NULL; }
        else firstTime = 1;
    }
    /* Ensure the object is at least the current struct size. A ring left over
       from an older, smaller layout is grown here so the new fields are backed
       (accessing beyond the object's size would otherwise fault). */
    {
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); if (firstTime) shm_unlink(ER_RING_NAME); return NULL; }
        if ((size_t)st.st_size < sizeof(ERRing) &&
            ftruncate(fd, (off_t)sizeof(ERRing)) != 0) {
            close(fd); if (firstTime) shm_unlink(ER_RING_NAME); return NULL;
        }
    }
    void *m = mmap(0, sizeof(ERRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    ERRing *r = (ERRing *)m;
    if (firstTime || er_load32(&r->magic) != ER_RING_MAGIC) {
        memset(r, 0, sizeof(ERRing));
        r->version     = ER_RING_VERSION;
        r->sampleRate  = 48000;
        r->inChannels  = ER_IN_CH;
        r->outChannels = ER_OUT_CH;
        er_store32(&r->magic, ER_RING_MAGIC);   /* publish last */
    }
    if (created) *created = firstTime;
    return r;
}

/**
 * @brief Attach to an already-created ring (does not create it).
 *
 * Typically called by the plugin. Returns @c NULL if the object does not exist
 * yet or its header magic does not match ::ER_RING_MAGIC.
 *
 * @return Pointer to the mapped ::ERRing, or @c NULL if unavailable/invalid.
 */
static inline ERRing *er_ring_attach(void) {
    int fd = shm_open(ER_RING_NAME, O_RDWR, 0666);
    if (fd < 0) return NULL;
    void *m = mmap(0, sizeof(ERRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    ERRing *r = (ERRing *)m;
    if (er_load32(&r->magic) != ER_RING_MAGIC) { munmap(m, sizeof(ERRing)); return NULL; }
    return r;
}

/**
 * @brief Unmap the ring and, optionally, remove the shared-memory name.
 * @param r          Ring to unmap (may be @c NULL).
 * @param unlink_it  Non-zero to also @c shm_unlink the name (the engine does this on shutdown).
 */
static inline void er_ring_close(ERRing *r, int unlink_it) {
    if (r) munmap(r, sizeof(ERRing));
    if (unlink_it) shm_unlink(ER_RING_NAME);
}

/**
 * @name SPSC ring operations
 * Single-producer/single-consumer moves of interleaved float32 frames. Counts are
 * in FRAMES (a frame is @c nch interleaved floats). Each function returns the
 * number of frames actually moved.
 * @{
 */

/** @brief Frames of free space given write/read counters. */
static inline uint32_t er__space(uint64_t w, uint64_t rd) { return ER_RING_FRAMES - (uint32_t)(w - rd); }
/** @brief Frames available to read given write/read counters. */
static inline uint32_t er__avail(uint64_t w, uint64_t rd) { return (uint32_t)(w - rd); }

/**
 * @brief Copy @p frames of @p nch-wide interleaved audio into a ring, handling wrap.
 * @param buf      Base of the ring's float array.
 * @param nch      Channels per frame.
 * @param counter  Producer's current write counter (frames).
 * @param src      Source interleaved samples.
 * @param frames   Frame count to write (must not exceed free space).
 */
static inline void er__wr(float *buf, uint32_t nch, uint64_t counter, const float *src, uint32_t frames) {
    uint32_t idx = (uint32_t)(counter & ER_RING_MASK);
    uint32_t first = ER_RING_FRAMES - idx; if (first > frames) first = frames;
    memcpy(buf + (size_t)idx * nch, src, (size_t)first * nch * sizeof(float));
    if (first < frames) memcpy(buf, src + (size_t)first * nch, (size_t)(frames - first) * nch * sizeof(float));
}
/**
 * @brief Copy @p frames of @p nch-wide interleaved audio out of a ring, handling wrap.
 * @param buf      Base of the ring's float array.
 * @param nch      Channels per frame.
 * @param counter  Consumer's current read counter (frames).
 * @param dst      Destination interleaved buffer.
 * @param frames   Frame count to read (must not exceed available frames).
 */
static inline void er__rd(const float *buf, uint32_t nch, uint64_t counter, float *dst, uint32_t frames) {
    uint32_t idx = (uint32_t)(counter & ER_RING_MASK);
    uint32_t first = ER_RING_FRAMES - idx; if (first > frames) first = frames;
    memcpy(dst, buf + (size_t)idx * nch, (size_t)first * nch * sizeof(float));
    if (first < frames) memcpy(dst + (size_t)first * nch, buf, (size_t)(frames - first) * nch * sizeof(float));
}

/**
 * @brief Capture producer: write ::ER_IN_CH-wide frames (app → coreaudiod).
 * @param r       Ring.
 * @param src     Interleaved ::ER_IN_CH-channel source frames.
 * @param frames  Requested frame count.
 * @return Frames written; fewer than requested (and ::ERRing::xrunCount incremented) on overrun.
 */
static inline uint32_t er_in_write(ERRing *r, const float *src, uint32_t frames) {
    uint64_t w = er_load(&r->inWrite), rd = er_load(&r->inRead);
    uint32_t space = er__space(w, rd);
    if (frames > space) { r->xrunCount++; frames = space; }
    er__wr(r->in, ER_IN_CH, w, src, frames);
    er_store(&r->inWrite, w + frames);
    return frames;
}

/**
 * @brief Backlog the consumer keeps ahead of the read head (~43 ms at 48 kHz).
 *
 * Bounds latency and rides out small producer/consumer clock drift: if the ring
 * accumulates more than this, the consumer skips the stale surplus.
 */
#define ER_TARGET_FILL 2048u

/**
 * @brief Capture consumer: read ::ER_IN_CH-wide frames (coreaudiod).
 *
 * If the backlog exceeds @p frames + ::ER_TARGET_FILL (e.g. the ring filled while
 * idle, or clock drift), stale frames are skipped so the read stays near the write
 * head — bounding latency and preventing the ring from sitting permanently full.
 * The consumer owns @c inRead, so this skip is safe. On underrun fewer frames are
 * returned and the caller's buffer beyond that stays whatever it was (zero-filled
 * by the caller).
 *
 * @param r       Ring.
 * @param dst     Destination interleaved ::ER_IN_CH-channel buffer.
 * @param frames  Requested frame count.
 * @return Frames actually read (may be fewer than requested on underrun).
 */
static inline uint32_t er_in_read(ERRing *r, float *dst, uint32_t frames) {
    uint64_t w = er_load(&r->inWrite), rd = er_load(&r->inRead);
    uint32_t avail = er__avail(w, rd);
    if (avail > frames + ER_TARGET_FILL) {
        uint32_t drop = avail - (frames + ER_TARGET_FILL);
        rd += drop; avail -= drop;
    }
    if (frames > avail) frames = avail;
    er__rd(r->in, ER_IN_CH, rd, dst, frames);
    er_store(&r->inRead, rd + frames);
    return frames;
}

/**
 * @brief Playback producer: write ::ER_OUT_CH-wide frames (coreaudiod → app).
 * @param r       Ring.
 * @param src     Interleaved ::ER_OUT_CH-channel source frames.
 * @param frames  Requested frame count.
 * @return Frames written; fewer (and ::ERRing::xrunCount incremented) on overrun.
 */
static inline uint32_t er_out_write(ERRing *r, const float *src, uint32_t frames) {
    uint64_t w = er_load(&r->outWrite), rd = er_load(&r->outRead);
    uint32_t space = er__space(w, rd);
    if (frames > space) { r->xrunCount++; frames = space; }
    er__wr(r->out, ER_OUT_CH, w, src, frames);
    er_store(&r->outWrite, w + frames);
    return frames;
}

/**
 * @brief Playback consumer: read ::ER_OUT_CH-wide frames (app/USB).
 *
 * Applies the same latency-bounding skip as ::er_in_read.
 *
 * @param r       Ring.
 * @param dst     Destination interleaved ::ER_OUT_CH-channel buffer.
 * @param frames  Requested frame count.
 * @return Frames actually read (may be fewer than requested on underrun).
 */
static inline uint32_t er_out_read(ERRing *r, float *dst, uint32_t frames) {
    uint64_t w = er_load(&r->outWrite), rd = er_load(&r->outRead);
    uint32_t avail = er__avail(w, rd);
    if (avail > frames + ER_TARGET_FILL) {
        uint32_t drop = avail - (frames + ER_TARGET_FILL);
        rd += drop; avail -= drop;
    }
    if (frames > avail) frames = avail;
    er__rd(r->out, ER_OUT_CH, rd, dst, frames);
    er_store(&r->outRead, rd + frames);
    return frames;
}
/** @} */

/**
 * @name Non-destructive metering
 * Read-only peek at recent ring contents for level meters. These NEVER touch the
 * read/write counters, so a third observer (e.g. the menu-bar app) can compute
 * VU levels without disturbing the single-producer/single-consumer discipline —
 * the real consumer (@c coreaudiod) keeps its own read position.
 * @{
 */

/** @brief Frames a meter looks back from the write head (~21 ms at 48 kHz). */
#define ER_PEEK_FRAMES 1024u

/**
 * @brief Per-channel absolute peak over the last @p lookback frames behind a write head.
 *
 * Reads @p lookback frames ending at @p writeCounter (clamped to the buffer) and
 * writes the max absolute sample for each of @p nch channels into @p outPeak.
 * Does not read or modify any ring counter, so it is safe to call concurrently
 * with the producer and consumer (values may be momentarily torn — acceptable
 * for a meter). If nothing has been written yet, all peaks are 0.
 *
 * @param buf          Base of the ring's interleaved float array.
 * @param nch          Channels per frame.
 * @param writeCounter Producer's current monotonic write counter (frames).
 * @param lookback     Number of trailing frames to scan.
 * @param[out] outPeak Array of @p nch floats receiving per-channel abs-peak.
 */
static inline void er_peek_levels(const float *buf, uint32_t nch, uint64_t writeCounter,
                                  uint32_t lookback, float *outPeak) {
    for (uint32_t c = 0; c < nch; c++) outPeak[c] = 0.0f;
    if (writeCounter == 0) return;
    if ((uint64_t)lookback > writeCounter) lookback = (uint32_t)writeCounter;
    if (lookback > ER_RING_FRAMES) lookback = ER_RING_FRAMES;
    uint64_t start = writeCounter - lookback;
    for (uint32_t f = 0; f < lookback; f++) {
        uint32_t idx = (uint32_t)((start + f) & ER_RING_MASK);
        const float *frame = buf + (size_t)idx * nch;
        for (uint32_t c = 0; c < nch; c++) {
            float v = frame[c]; if (v < 0.0f) v = -v;
            if (v > outPeak[c]) outPeak[c] = v;
        }
    }
}

/**
 * @brief Capture (input) per-channel peak levels for metering.
 * @param r            Ring (read-only use).
 * @param[out] outPeak Array of ::ER_IN_CH floats receiving per-channel abs-peak.
 */
static inline void er_in_peek_levels(const ERRing *r, float *outPeak) {
    er_peek_levels(r->in, ER_IN_CH, er_load(&r->inWrite), ER_PEEK_FRAMES, outPeak);
}

/**
 * @brief Playback (output) per-channel peak levels for metering.
 * @param r            Ring (read-only use).
 * @param[out] outPeak Array of ::ER_OUT_CH floats receiving per-channel abs-peak.
 */
static inline void er_out_peek_levels(const ERRing *r, float *outPeak) {
    er_peek_levels(r->out, ER_OUT_CH, er_load(&r->outWrite), ER_PEEK_FRAMES, outPeak);
}

/**
 * @brief Copy the engine-published live input meter levels (::ER_IN_CH values).
 *
 * These are live regardless of ring fill (see ::ERRing::inLevel), unlike
 * ::er_in_peek_levels which reads buffer contents that freeze when the ring
 * saturates with no consumer.
 */
static inline void er_read_in_meters(const ERRing *r, float *out) {
    for (uint32_t c = 0; c < ER_IN_CH; c++) out[c] = r->inLevel[c];
}
/** @brief Copy the engine-published live output meter levels (::ER_OUT_CH values). */
static inline void er_read_out_meters(const ERRing *r, float *out) {
    for (uint32_t c = 0; c < ER_OUT_CH; c++) out[c] = r->outLevel[c];
}
/** @} */

#endif /* ER_AUDIO_RING_H */
