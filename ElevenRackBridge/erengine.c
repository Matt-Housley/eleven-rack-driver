/**
 * @file erengine.c
 * @brief Eleven Rack user-space USB audio engine — the app-hosted core of the
 *        no-driver architecture.
 *
 * Runs as an ordinary user process (no kernel extension, no DriverKit system
 * extension, no security changes) and:
 * - claims the Eleven Rack over USB from user space,
 * - streams full-duplex high-speed isochronous audio (interface 4 capture,
 *   interface 3 playback),
 * - de-interleaves the 8-channel/32-bit capture frames into float32 and publishes
 *   them to the shared ring (::ERRing) for the Core Audio HAL plugin,
 * - encodes the plugin's 6-channel playback back to the device,
 * - retunes the hardware sample-rate clock to follow the Core Audio device rate.
 *
 * MIDI needs no code here: the Eleven Rack's USB-MIDI interface is a standard
 * class device that macOS exposes directly (the "Eleven Rack Rig" / "Eleven Rack
 * External" ports via AppleMIDIUSBDriver).
 *
 * Modes (first argument):
 * - (none)            run as the audio bridge; rate-follow active. Ctrl-C to stop.
 * - `--monitor [Hz]`  additionally play a stereo pair of input channels to the
 *                     Mac's default output via AudioQueue (live audition).
 * - `--wav F S`       record S seconds of the selected stereo pair to WAV file F.
 * - `--raw F S`       dump every parsed 32-bit word to F (offline analysis).
 * - `--frames F S`    dump each USB isoc packet as `[u16 len][payload]` to F.
 *
 * A handful of `ER_*` environment variables (e.g. `ER_SAMPLERATE`, `ER_CLOCK`,
 * `ER_LANE_L`/`ER_LANE_R`) override defaults for tuning without recompiling.
 *
 * @note Build:
 * @code
 * clang -o erengine erengine.c -framework IOKit -framework CoreFoundation \
 *       -framework CoreMIDI -framework AudioToolbox -framework CoreAudio \
 *       -Wno-deprecated-declarations
 * @endcode
 */

#include "ERAudioRing.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <dispatch/dispatch.h>
#include <math.h>

#define ER_VID 0x0DBA            /**< Eleven Rack USB vendor ID. */
#define ER_PID 0xB011            /**< Eleven Rack USB product ID. */
#define ER_IF_OUT 3              /**< USB interface number for playback (isoc OUT). */
#define ER_IF_IN  4              /**< USB interface number for capture (isoc IN). */
#define ER_ALT 1                 /**< Streaming alternate setting (0 = idle). */
#define N_REQ 32                 /**< In-flight isoc requests per direction (re-arm slack). */
#define FPR 16                   /**< Frame-list entries (microframes) per isoc request. */
#define MAX_PKT 512              /**< Per-frame buffer slot size (≥ endpoint maxPacket 416). */
#define OUT_BYTES_FRAME 288      /**< Legacy constant (unused; superseded by the output accordion). */

/**
 * @name Capture de-interleave layout
 * Each 32-byte capture unit holds ::SF_WORDS 32-bit words (one per input channel).
 * These are tunable via environment variables for diagnostics; the defaults match
 * the production 8-channel layout.
 * @{
 */
static int SF_WORDS = 8;          /**< Words (channels) per capture unit. */
static int SF_LANE_L = 2;         /**< Left channel index used by the monitor/WAV pair. */
static int SF_LANE_R = 3;         /**< Right channel index used by the monitor/WAV pair. */
int gMono = 0;                    /**< Diagnostic: emit the L/R lanes as consecutive mono samples. */
int gLanes[64];                   /**< Diagnostic: `ER_LANES` lane list to emit as mono. */
int gNLanes = 0;                  /**< Diagnostic: number of entries in ::gLanes. */
#define SF_PHASE   0              /**< Word-phase offset applied when indexing lanes. */
#define ER_RATE    48000          /**< Default hardware rate if none can be adopted. */
/** @} */

static IOUSBInterfaceInterface500 **gIn=NULL;   /**< Capture (IF4) interface. */
static IOUSBInterfaceInterface500 **gOut=NULL;  /**< Playback (IF3) interface. */
static UInt8 gInPipe=0;           /**< Pipe index of the isoc IN endpoint. */
static UInt8 gOutPipe=0;          /**< Pipe index of the isoc OUT endpoint. */
static UInt16 gInReqLen=MAX_PKT;  /**< Per-frame request length for capture (endpoint maxPacket). */
static UInt64 gInFrame=0;         /**< Next bus (millisecond) frame to schedule capture from. */
static UInt64 gOutFrame=0;        /**< Next bus (millisecond) frame to schedule playback from. */
static volatile sig_atomic_t gStop=0; /**< Set on error or SIGINT to stop the run loop. */

/** @brief One isoc request: a frame list plus its data buffer. */
typedef struct { IOUSBIsocFrame fr[FPR]; UInt8 *data; } Req;
static Req gInReqs[N_REQ];        /**< Pool of capture requests. */
static Req gOutReqs[N_REQ];       /**< Pool of playback requests. */

static ERRing *gRing=NULL;                    /**< Shared audio transport (NULL in file-dump modes). */
static IOUSBDeviceInterface500 **gDev=NULL;   /**< Device interface, used by rate-follow SET_CUR. */
static uint32_t gHwRate=48000;                /**< Current hardware sample rate (Hz). */
static double   gOutAccum=0.0;                /**< Output accordion accumulator (fractional frames). */

/**
 * @name Time-addressed playback read head
 * The plugin overwrites each client's WriteMix into the ring at absolute sample
 * time. The engine reads at ::gPlayHead, kept trailing the furthest written sample
 * time (::ERRing::outWriteMax) by ~::OUT_TARGET_LAT frames of buffer; it re-anchors
 * if that lag under/overruns (start, stall, drift). @{
 */
static uint64_t gPlayHead=0;                  /**< Engine playback read position (sample time). */
static uint64_t gLastWmax=0;                  /**< outWriteMax at the previous cycle (active detection). */
static uint64_t gPrimeStart=0;                /**< Sample time playback (re)started, while prebuffering. */
static int      gPlayInit=0;                  /**< Play head anchored to the write head yet. */
static int      gPriming=0;                   /**< Prebuffering: emit silence until the buffer fills. */
#define OUT_TARGET_LAT 1024u                  /**< Target lag behind the write head (frames, ~21 ms). */
#define OUT_MIN_LAT     128u                  /**< Re-anchor (while active) if the lag falls below this. */
#define OUT_MAX_LAT     8192u                 /**< Re-anchor if the lag exceeds this. */
/** @} */

/**
 * @name Rate-change state machine
 * A sample-rate change must not happen mid-stream. On a change the engine stops
 * re-arming isoc requests, lets in-flight transfers drain, then restarts the
 * streams at the new rate (see ::rateFollow and ::reconfigure).
 * @{
 */
static volatile int gInFlight=0;   /**< Count of armed-but-not-completed isoc requests. */
static volatile int gRateState=0;  /**< 0 = normal, 1 = draining for a pending rate change. */
static uint32_t gPendingRate=0;    /**< Target rate to apply once drained. */
/** @} */

static UInt8 gCarry[4];           /**< Bytes carried between packets to complete a 32-bit word. */
static int gCarryN=0;             /**< Number of valid bytes in ::gCarry. */
static uint32_t gWordPhase=0;     /**< Running word index (mod ::SF_WORDS) for de-interleaving. */
static int32_t  gSF[64];          /**< The current partial capture unit being assembled. */

static unsigned long long gSFCount=0; /**< Total capture frames emitted (for the measured-rate report). */

/* Live metering state: leaky mean-square per channel (spike-robust RMS meter),
   plus a short startup-settle so the stream's initial garbage frames don't blip
   the meters. Reset by ::resetMeters at each stream (re)start. */
static float    gInMS[ER_IN_CH];   /**< Leaky mean-square for the input meters. */
static float    gOutMS[ER_OUT_CH]; /**< Leaky mean-square for the output meters. */
static uint32_t gMeterSettle=0;    /**< Frames to skip before publishing meters. */
#define METER_RMS_A   0.99986f      /**< Per-frame leak (~150 ms integration at 48 kHz). */
#define METER_SETTLE  2400u         /**< ~50 ms of frames skipped after a (re)start. */

/** @brief Reset meter state and begin a startup settle (call at each stream start). */
static inline void resetMeters(void){
    for(int c=0;c<(int)ER_IN_CH;c++){ gInMS[c]=0.f; if(gRing) gRing->inLevel[c]=0.f; }
    for(int c=0;c<(int)ER_OUT_CH;c++){ gOutMS[c]=0.f; if(gRing) gRing->outLevel[c]=0.f; }
    gMeterSettle=METER_SETTLE;
}
static float   *gWav=NULL;        /**< Growable buffer for `--wav` mode (interleaved stereo). */
static size_t gWavCap=0;          /**< Capacity of ::gWav in floats. */
static size_t gWavN=0;            /**< Used length of ::gWav in floats. */
static int gWavMode=0;            /**< Non-zero in `--wav` mode. */

static FILE *gRawFile=NULL;       /**< `--raw` output file (every 32-bit word), else NULL. */
static unsigned long long gRawWords=0; /**< Words written in `--raw` mode. */
static FILE *gFramesFile=NULL;    /**< `--frames` output file (`[u16 len][payload]` per packet), else NULL. */
static unsigned long long gFrameCount=0; /**< Packets written in `--frames` mode. */


/**
 * @brief Select the device's clock source via a SET_CUR control request.
 * @param dev  Open device interface.
 * @param src  Clock source (1 = internal, 2 = "asbu", 3 = S/PDIF). Without this the
 *             device free-runs and produces incorrect framing.
 * @return @c kIOReturnSuccess on success, else an IOKit error.
 */
static IOReturn setClockSource(IOUSBDeviceInterface500 **dev, uint8_t src){
    IOUSBDevRequest req;
    req.bmRequestType=0x21; req.bRequest=0x01; req.wValue=0x0100;
    req.wIndex=0x8001; req.wLength=1; req.pData=&src; req.wLenDone=0;
    return (*dev)->DeviceRequest(dev, &req);
}

/**
 * @brief Set the device sample rate via a UAC2-style SET_CUR control request.
 *
 * Without this the device streams in an unconfigured default state. Encoding:
 * `bmRequestType=0x21`, `bRequest=0x01` (CUR), `wValue=0x0100` (SAM_FREQ),
 * `wIndex=0x8101` (clock entity 0x81, interface 1), `wLength=4`, data = rate
 * (little-endian u32).
 *
 * @param dev   Open device interface.
 * @param rate  One of 44100, 48000, 88200, 96000.
 * @return @c kIOReturnSuccess on success, else an IOKit error.
 */
static IOReturn setSampleRate(IOUSBDeviceInterface500 **dev, uint32_t rate){
    IOUSBDevRequest req;
    req.bmRequestType = 0x21;
    req.bRequest      = 0x01;
    req.wValue        = 0x0100;
    req.wIndex        = 0x8101;
    req.wLength       = 4;
    req.pData         = &rate;
    req.wLenDone      = 0;
    return (*dev)->DeviceRequest(dev, &req);
}

/**
 * @brief Read the nominal sample rate of the "Eleven Rack" Core Audio device.
 *
 * Core Audio persists and restores this across restarts; adopting it at startup
 * keeps the hardware and the UI in agreement.
 *
 * @return The device's current rate in Hz, or 0 if the device is not present yet.
 */
static uint32_t queryDeviceRate(void){
    AudioObjectPropertyAddress a={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 sz=0; if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,0,&sz)!=noErr) return 0;
    int n=sz/sizeof(AudioObjectID); if(n<=0||n>64) return 0;
    AudioObjectID ids[64]; if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,0,&sz,ids)!=noErr) return 0;
    for(int i=0;i<n;i++){
        CFStringRef nm=NULL; UInt32 s=sizeof(nm);
        AudioObjectPropertyAddress na={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,0};
        if(AudioObjectGetPropertyData(ids[i],&na,0,0,&s,&nm)!=noErr||!nm) continue;
        char buf[128]=""; CFStringGetCString(nm,buf,128,kCFStringEncodingUTF8); CFRelease(nm);
        if(strstr(buf,"Eleven")){
            Float64 sr=0; UInt32 ss=sizeof(sr);
            AudioObjectPropertyAddress ra={kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,0};
            if(AudioObjectGetPropertyData(ids[i],&ra,0,0,&ss,&sr)==noErr && sr>0) return (uint32_t)(sr+0.5);
        }
    }
    return 0;
}

/**
 * @brief ::queryDeviceRate with a hard timeout so a wedged @c coreaudiod can't hang startup.
 *
 * The Core Audio HAL calls are synchronous and will block indefinitely if
 * @c coreaudiod is unresponsive. This runs the query on a background queue and
 * waits only @p timeoutSec; on timeout it returns 0 (caller falls back to a
 * default rate, which rate-follow later corrects). If the query never returns,
 * that one background thread stays blocked — but the engine proceeds.
 *
 * @param timeoutSec  Maximum seconds to wait.
 * @return The device rate in Hz, or 0 on timeout / not present.
 */
static uint32_t queryDeviceRateBounded(double timeoutSec){
    __block uint32_t result = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        uint32_t r = queryDeviceRate();
        result = r;
        dispatch_semaphore_signal(sem);
    });
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSec*1e9))) != 0)
        return 0;   /* timed out */
    return result;
}

/* ------------------------------------------------------------------ USB setup */

/**
 * @brief Build an @c IOUSBDeviceInterface500 for a matched USB service.
 * @param s  Matched @c io_service_t (consumed by the caller separately).
 * @return The device interface, or @c NULL on failure.
 */
static IOUSBDeviceInterface500 **openDevice(io_service_t s){
    IOCFPlugInInterface **pl=NULL; SInt32 sc=0;
    if(IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc)!=KERN_SUCCESS||!pl)return NULL;
    IOUSBDeviceInterface500 **d=NULL;(*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&d);(*pl)->Release(pl);return d;
}

/**
 * @brief Open the interface with a given interface number and return its object.
 * @param dev   Open device interface.
 * @param want  Desired @c bInterfaceNumber.
 * @return The matching interface object, or @c NULL if not found.
 */
static IOUSBInterfaceInterface500 **openIface(IOUSBDeviceInterface500 **dev,UInt8 want){
    IOUSBFindInterfaceRequest r={kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare};
    io_iterator_t it=0; if((*dev)->CreateInterfaceIterator(dev,&r,&it)!=kIOReturnSuccess)return NULL;
    io_service_t s; IOUSBInterfaceInterface500 **f=NULL;
    while((s=IOIteratorNext(it))){ IOCFPlugInInterface **pl=NULL;SInt32 sc=0;
        if(IOCreatePlugInInterfaceForService(s,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc)==KERN_SUCCESS&&pl){
            IOUSBInterfaceInterface500 **i=NULL;(*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),(LPVOID*)&i);(*pl)->Release(pl);
            UInt8 n=0xFF; if(i)(*i)->GetInterfaceNumber(i,&n);
            if(i&&n==want){f=i;IOObjectRelease(s);break;} if(i)(*i)->Release(i);
        } IOObjectRelease(s);
    } IOObjectRelease(it); return f;
}

/**
 * @brief Find the isochronous pipe of a given direction on an interface.
 * @param intf    Open interface object (already at the streaming alt setting).
 * @param wantIn  @c true for the IN (capture) pipe, @c false for OUT (playback).
 * @param mpOut   Optional; receives the pipe's max packet size.
 * @return The 1-based pipe index, or 0 if none matched.
 */
static UInt8 findPipe(IOUSBInterfaceInterface500 **intf,bool wantIn,UInt16 *mpOut){
    UInt8 n=0;(*intf)->GetNumEndpoints(intf,&n);
    for(UInt8 p=1;p<=n;p++){UInt8 d=0,nu=0,tt=0,iv=0;UInt16 mp=0;
        if((*intf)->GetPipeProperties(intf,p,&d,&nu,&tt,&mp,&iv)==kIOReturnSuccess&&tt==kUSBIsoc)
            if((wantIn&&d==kUSBIn)||(!wantIn&&d==kUSBOut)){if(mpOut)*mpOut=mp;return p;}
    } return 0;
}

/* ------------------------------------------------------------- capture parsing */

/**
 * @brief Emit one assembled capture unit as a frame of ::ER_IN_CH float channels.
 *
 * Each 32-bit word holds 24-bit audio left-justified, so dividing by 2^31 yields a
 * -1..1 float. Publishes all channels to the input ring; in `--wav` mode also
 * appends the selected stereo pair to ::gWav.
 */
static inline void emitSuperframe(void){
    float f8[ER_IN_CH];
    for (int c = 0; c < (int)ER_IN_CH; c++)
        f8[c] = (float)((double)gSF[c] / 2147483648.0);
    gSFCount++;
    if (gRing) {
        er_store(&gRing->hwFrames, gSFCount);      // hardware clock (always advances while streaming)
        er_in_write(gRing, f8, 1);                 // publish all 8 channels
        // Live meter: leaky-RMS (spike-robust) rather than peak-hold, so an
        // isolated glitch can't latch a silent channel high. After the startup
        // settle, publish sqrt(mean-square) as the linear level.
        if (gMeterSettle) {
            gMeterSettle--;
            if (gMeterSettle == 0) for (int c=0;c<(int)ER_IN_CH;c++) gInMS[c]=0.f;  // clean slate
        } else {
            for (int c = 0; c < (int)ER_IN_CH; c++) {
                float a = f8[c];
                gInMS[c] = gInMS[c]*METER_RMS_A + a*a*(1.0f-METER_RMS_A);
                gRing->inLevel[c] = sqrtf(gInMS[c]);
            }
        }
    }
    if (gWavMode) {                                 // debug WAV: selected stereo pair
        if (gWavN + 2 > gWavCap) { gWavCap = gWavCap?gWavCap*2:1<<16; gWav=realloc(gWav,gWavCap*sizeof(float)); }
        gWav[gWavN++] = f8[SF_LANE_L];
        gWav[gWavN++] = f8[SF_LANE_R];
    }
}

/**
 * @brief Feed one packet's payload into the continuous capture parser.
 *
 * Bytes are concatenated across packets (carrying a partial word in ::gCarry),
 * split into 32-bit little-endian words, and placed into ::gSF by lane; a complete
 * unit triggers ::emitSuperframe. In `--raw` mode the words are written to file
 * instead.
 *
 * @param p    Packet payload.
 * @param len  Payload length in bytes.
 */
static void feedBytes(const UInt8 *p,int len){
    static UInt8 tmp[MAX_PKT+4]; int t=0;
    for(int i=0;i<gCarryN;i++) tmp[t++]=gCarry[i];
    memcpy(tmp+t,p,len); t+=len;
    int words=t/4, rem=t%4;
    for(int i=0;i<words;i++){ const UInt8 *q=tmp+i*4;
        int32_t w=(int32_t)(q[0]|(q[1]<<8)|(q[2]<<16)|(q[3]<<24));
        if(gRawFile){ fwrite(&w,4,1,gRawFile); gRawWords++; continue; }
        uint32_t lane=(gWordPhase + SF_PHASE) % SF_WORDS;
        gSF[lane]=w;
        gWordPhase++;
        if(lane==(uint32_t)(SF_WORDS-1)) emitSuperframe();
    }
    gCarryN=rem; for(int i=0;i<rem;i++) gCarry[i]=tmp[words*4+i];
}

/* ---------------------------------------------------------------- isoc engine */

static void armIn(Req*);
static void armOut(Req*);
static void reconfigure(void);

/**
 * @brief Completion callback for a capture request.
 *
 * Decrements the in-flight count; if a rate change is draining, triggers
 * ::reconfigure once the last request completes. Otherwise parses each frame's
 * data (read at the fixed `f * gInReqLen` stride the isoc stack uses) and re-arms.
 *
 * @param rc   The ::Req that completed.
 * @param res  Overall transfer result.
 * @param a0   Unused.
 */
static void inDone(void *rc,IOReturn res,void*a0){(void)a0;Req*r=(Req*)rc;
    gInFlight--;
    if(gRateState){ if(gInFlight<=0) reconfigure(); return; }   // draining for rate change
    if(res!=kIOReturnSuccess&&res!=kIOReturnUnderrun){gStop=1;CFRunLoopStop(CFRunLoopGetCurrent());return;}
    // The isoc stack writes each frame's data at a FIXED stride equal to the
    // requested per-frame size (frReqCount = gInReqLen), regardless of how many
    // bytes actually arrived. So frame f is at r->data + f*gInReqLen — NOT
    // contiguous, and NOT f*MAX_PKT.
    for(int f=0;f<FPR;f++){int len=r->fr[f].frActCount; if(len<=0) continue;
        UInt8 *fp = r->data + (size_t)f*gInReqLen;
        if(gFramesFile){ uint16_t L=(uint16_t)len; fwrite(&L,2,1,gFramesFile); fwrite(fp,1,len,gFramesFile); gFrameCount++; }
        else feedBytes(fp,len);
    }
    if(!gStop)armIn(r);
}

/** @brief Nominal output sample-frames per microframe at 48 kHz (48000/8000). */
#define OUT_FRAMES_PER_MICRO 6

/**
 * @brief Completion callback for a playback request.
 *
 * Decrements the in-flight count (driving ::reconfigure during a rate change) and
 * re-arms the request.
 *
 * @param rc   The ::Req that completed.
 * @param res  Overall transfer result (ignored).
 * @param a0   Unused.
 */
static void outDone(void *rc,IOReturn res,void*a0){(void)a0;Req*r=(Req*)rc;
    (void)res;
    gInFlight--;
    if(gRateState){ if(gInFlight<=0) reconfigure(); return; }
    if(!gStop)armOut(r);
}

/**
 * @brief Milliseconds spanned by one isoc request (::FPR microframes / 8 per ms).
 *
 * On high-speed, @c frameStart is a millisecond bus frame but each frame-list entry
 * is a 125 µs microframe. Advancing the start by ::FPR would skip 7/8 of the stream,
 * so it advances by @c FPR/8.
 */
#define FRAME_MS (FPR/8)

/**
 * @brief Arm a capture request and schedule it on the isoc IN pipe.
 * @param r  Capture request to (re)submit.
 */
static void armIn(Req*r){ for(int f=0;f<FPR;f++){r->fr[f].frReqCount=gInReqLen;r->fr[f].frActCount=0;r->fr[f].frStatus=0;}
    if((*gIn)->ReadIsochPipeAsync(gIn,gInPipe,r->data,gInFrame,FPR,r->fr,inDone,r)!=kIOReturnSuccess){gStop=1;CFRunLoopStop(CFRunLoopGetCurrent());return;} gInFrame+=FRAME_MS; gInFlight++; }

/**
 * @brief Arm a playback request: pull output frames from the ring, encode, submit.
 *
 * The per-microframe frame count follows the current hardware rate via an accordion
 * (`gHwRate/8000` on average, e.g. 44.1k alternates 5/6, 96k is 12). Each channel is
 * a float clamped to [-1,1] and written as a 32-bit integer. Data is laid out
 * contiguously (frame @e f at the cumulative `frReqCount` offset), matching the
 * isoc buffer model.
 *
 * @param r  Playback request to (re)submit.
 */
static void armOut(Req*r){
    size_t off=0;
    // Keep the play head trailing coreaudiod's furthest written sample time by
    // ~OUT_TARGET_LAT; re-anchor if it under/overruns (start, stall, drift).
    uint64_t wmax=0;
    if(gRing){
        wmax = er_load(&gRing->outWriteMax);
        int active = (wmax != gLastWmax);      // coreaudiod wrote since the last cycle
        // Enter (re)prebuffering on first use, or when the write head is far ahead of
        // where we read (playback resumed after silence, or the engine fell behind),
        // or — while actively playing — if the lag drifts out of [MIN,MAX].
        if(!gPlayInit || gPlayHead + OUT_MAX_LAT < wmax ||
           (active && !gPriming && gPlayHead + OUT_MIN_LAT > wmax)){
            gPriming    = 1;
            gPrimeStart = wmax;                // consume from here once the buffer fills
            gPlayHead   = wmax;                // read silence meanwhile
            gPlayInit   = 1;
        }
        if(gPriming){
            if(active && wmax >= gPrimeStart + OUT_TARGET_LAT){
                gPlayHead = gPrimeStart;       // buffer built: start consuming from the top
                gPriming  = 0;
            } else if(gPlayHead > wmax){
                gPlayHead = wmax;              // still filling: hold at the frontier → silence
            }
        } else if(gPlayHead > wmax){
            // Paused/stopped (write head frozen): clamp so we read silence past the
            // write head rather than looping stale audio.
            gPlayHead = wmax;
        }
        gLastWmax = wmax;
        er_store(&gRing->outPlayHead, gPlayHead);
    }
    for(int f=0;f<FPR;f++){
        gOutAccum += gHwRate/8000.0;
        int nf=(int)gOutAccum; gOutAccum-=nf;                 // frames this microframe
        if(nf>12)nf=12;
        float buf[12*ER_OUT_CH];
        uint32_t got = 0;
        if(gRing){ er_out_read_at(gRing, gPlayHead, wmax, buf, (uint32_t)nf); gPlayHead += (uint32_t)nf; got=(uint32_t)nf; }
        if (gRing && !gMeterSettle) {                   // live output meters (leaky-RMS)
            for (int c=0;c<(int)ER_OUT_CH;c++){
                for (int fr=0; fr<(int)got; fr++){ float a=buf[fr*(int)ER_OUT_CH+c];
                    gOutMS[c] = gOutMS[c]*METER_RMS_A + a*a*(1.0f-METER_RMS_A); }
                gRing->outLevel[c] = sqrtf(gOutMS[c]);
            }
        }
        uint8_t *dst = r->data + off;
        for(int fr=0; fr<nf; fr++)
            for(int c=0;c<(int)ER_OUT_CH;c++){
                float v = (fr<(int)got)? buf[fr*ER_OUT_CH+c] : 0.f;
                if(v>1.f)v=1.f; else if(v<-1.f)v=-1.f;
                int32_t s=(int32_t)(v*2147483647.0);
                memcpy(dst+(fr*(int)ER_OUT_CH+c)*4,&s,4);
            }
        int bytes=nf*(int)ER_OUT_CH*4;
        r->fr[f].frReqCount=bytes; r->fr[f].frActCount=0; r->fr[f].frStatus=0;
        off+=bytes;
    }
    if((*gOut)->WriteIsochPipeAsync(gOut,gOutPipe,r->data,gOutFrame,FPR,r->fr,outDone,r)!=kIOReturnSuccess){return;} gOutFrame+=FRAME_MS; gInFlight++; }

/* ---------------------------------------------------------------------- WAV */

/**
 * @brief Write the accumulated `--wav` stereo buffer to a 32-bit-float WAV file.
 * @param path  Output file path.
 * @param rate  Sample rate to stamp in the WAV header (Hz).
 */
static void writeWav(const char*path,double rate){
    FILE*f=fopen(path,"wb"); if(!f){printf("  cannot open %s\n",path);return;}
    uint32_t nFrames=(uint32_t)(gWavN/2);       /* debug WAV is a stereo pair */
    uint32_t byteRate=(uint32_t)rate*2*4, dataBytes=nFrames*2*4;
    uint16_t fmt=3 /*IEEE float*/, ch=2, bits=32, block=2*4;
    uint32_t sr=(uint32_t)rate, chunk=36+dataBytes, sub1=16;
    fwrite("RIFF",1,4,f); fwrite(&chunk,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&sub1,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&sr,4,1,f); fwrite(&byteRate,4,1,f); fwrite(&block,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&dataBytes,4,1,f); fwrite(gWav,4,gWavN,f);
    fclose(f);
    printf("  wrote %s: %u frames @ %.0f Hz (%.2fs)\n",path,nFrames,rate,nFrames/rate);
}


/* ---------------------------------------------------------------- live monitor */

static int gMonitor=0;            /**< Non-zero in `--monitor` mode. */

/**
 * @brief AudioQueue output callback for the live monitor.
 *
 * Fills the buffer with the selected input stereo pair (::SF_LANE_L / ::SF_LANE_R)
 * pulled from the input ring, zero-filling on underrun.
 */
static void aqCallback(void *u, AudioQueueRef q, AudioQueueBufferRef b){
    (void)u;
    UInt32 frames = b->mAudioDataBytesCapacity/(sizeof(float)*2);
    float *out=(float*)b->mAudioData;
    memset(out,0,frames*sizeof(float)*2);
    if(gRing){
        static float tmp[2048*ER_IN_CH];
        UInt32 want=frames>2048?2048:frames, got=er_in_read(gRing,tmp,want);
        for(UInt32 i=0;i<got;i++){ out[i*2]=tmp[i*ER_IN_CH+SF_LANE_L]; out[i*2+1]=tmp[i*ER_IN_CH+SF_LANE_R]; }
    }
    b->mAudioDataByteSize = frames*sizeof(float)*2;
    AudioQueueEnqueueBuffer(q,b,0,NULL);
}

static AudioQueueRef gAQ=NULL;    /**< Output AudioQueue for the live monitor. */

/**
 * @brief Start the live monitor: a stereo float32 AudioQueue on the default output.
 * @param rate  Playback sample rate (Hz).
 */
static void startMonitor(double rate){
    AudioStreamBasicDescription f={0};
    f.mSampleRate=rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame=2; f.mBitsPerChannel=32;
    f.mFramesPerPacket=1; f.mBytesPerFrame=2*sizeof(float); f.mBytesPerPacket=f.mBytesPerFrame;
    if(AudioQueueNewOutput(&f,aqCallback,NULL,NULL,NULL,0,&gAQ)!=noErr){printf("  AudioQueue init failed\n");return;}
    UInt32 bytes=1024*2*sizeof(float);
    for(int i=0;i<3;i++){ AudioQueueBufferRef b; AudioQueueAllocateBuffer(gAQ,bytes,&b);
        b->mAudioDataByteSize=bytes; memset(b->mAudioData,0,bytes); AudioQueueEnqueueBuffer(gAQ,b,0,NULL); }
    AudioQueueStart(gAQ,NULL);
    printf("  live monitor: playing lanes %d/%d to default output @ %.0f Hz\n",SF_LANE_L,SF_LANE_R,rate);
}

/**
 * @brief Apply a pending sample-rate change once the isoc streams have drained.
 *
 * Called from the last completing callback when @c gInFlight reaches 0: parks both
 * interfaces at the idle alt setting, issues the new rate, restarts streaming,
 * resets the capture de-interleave phase and scheduling, and re-arms all requests.
 */
static void reconfigure(void){
    (*gIn)->SetAlternateInterface(gIn,0); (*gOut)->SetAlternateInterface(gOut,0);
    setSampleRate(gDev,gPendingRate); gHwRate=gPendingRate; gOutAccum=0; gPlayInit=0;
    (*gIn)->SetAlternateInterface(gIn,ER_ALT); (*gOut)->SetAlternateInterface(gOut,ER_ALT);
    UInt16 im=0; gInPipe=findPipe(gIn,true,&im); gOutPipe=findPipe(gOut,false,NULL);
    gWordPhase=0; gCarryN=0;                        // reset capture de-interleave phase
    UInt64 bus=0;AbsoluteTime at;(*gIn)->GetBusFrameNumber(gIn,&bus,&at); gInFrame=gOutFrame=bus+25;
    gRateState=0;
    printf("  rate-follow: hardware clock -> %u Hz (streams restarted)\n",gPendingRate); fflush(stdout);
    resetMeters();
    for(int i=0;i<N_REQ;i++){armOut(&gOutReqs[i]);armIn(&gInReqs[i]);}
}

/**
 * @brief Timer callback: adopt a sample-rate change requested via the ring.
 *
 * The plugin writes the Core Audio device's chosen rate into the ring; if it
 * differs from the hardware rate this begins draining (see the rate-change state
 * machine) so ::reconfigure can apply it safely between streams.
 */
static void rateFollow(CFRunLoopTimerRef t,void*i){ (void)t;(void)i;
    if(!gRing||!gDev||gRateState) return;
    uint32_t want=er_load32(&gRing->sampleRate);
    if(want!=gHwRate && (want==44100||want==48000||want==88200||want==96000)){
        gPendingRate=want; gRateState=1;           // reconfigure() fires when gInFlight hits 0
    }
}

/** @brief SIGINT/SIGTERM handler: request a clean stop of the run loop, which
 *  proceeds to teardown (stop isoc, SetAlternateInterface(0), close) so the USB
 *  device is released cleanly rather than left wedged by a hard kill. */
static void onSig(int s){(void)s; gStop=1; CFRunLoopStop(CFRunLoopGetCurrent());}

/**
 * @brief Program entry point: parse mode, claim the device, stream, and clean up.
 * @param argc  Argument count.
 * @param argv  Arguments (see the file-level mode list).
 * @return 0 on success; a small non-zero code on a startup failure.
 */
int main(int argc,char**argv){
    const char*wavPath=NULL; double wavSecs=0; const char*rawPath=NULL; double rawSecs=0;
    if(argc>=4 && strcmp(argv[1],"--wav")==0){ gWavMode=1; wavPath=argv[2]; wavSecs=atof(argv[3]); }
    if(argc>=4 && strcmp(argv[1],"--raw")==0){ rawPath=argv[2]; rawSecs=atof(argv[3]);
        gRawFile=fopen(rawPath,"wb"); if(!gRawFile){printf("cannot open %s\n",rawPath);return 1;} wavSecs=rawSecs; }
    if(argc>=4 && strcmp(argv[1],"--frames")==0){ rawPath=argv[2]; rawSecs=atof(argv[3]);
        gFramesFile=fopen(rawPath,"wb"); if(!gFramesFile){printf("cannot open %s\n",rawPath);return 1;} wavSecs=rawSecs; }
    double monRate=48000;
    if(argc>=2 && strcmp(argv[1],"--monitor")==0){ gMonitor=1; if(argc>=3) monRate=atof(argv[2]); }
    // env overrides for live tuning without recompiling:
    if(getenv("ER_WORDS"))  SF_WORDS =atoi(getenv("ER_WORDS"));
    if(getenv("ER_LANE_L")) SF_LANE_L=atoi(getenv("ER_LANE_L"));
    if(getenv("ER_LANE_R")) SF_LANE_R=atoi(getenv("ER_LANE_R"));
    if(getenv("ER_MONO"))   gMono=atoi(getenv("ER_MONO"));
    if(getenv("ER_LANES")){ char*s=strdup(getenv("ER_LANES")),*t=strtok(s,","); while(t&&gNLanes<64){gLanes[gNLanes++]=atoi(t);t=strtok(NULL,",");} free(s);
        printf("  ER_LANES: emitting %d lanes/unit as mono\n",gNLanes); }

    printf("Eleven Rack audio engine %s\n", gRawFile?"(RAW dump mode)":gWavMode?"(WAV record mode)":"(bridge mode)");
    printf("========================================\n");

    SInt32 vid=ER_VID,pid=ER_PID;
    CFMutableDictionaryRef m=IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v=CFNumberCreate(NULL,kCFNumberSInt32Type,&vid),p=CFNumberCreate(NULL,kCFNumberSInt32Type,&pid);
    CFDictionarySetValue(m,CFSTR(kUSBVendorID),v);CFDictionarySetValue(m,CFSTR(kUSBProductID),p);CFRelease(v);CFRelease(p);
    io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,m);
    if(!svc){printf("  Eleven Rack not found — plug it in.\n");return 1;}
    IOUSBDeviceInterface500 **dev=openDevice(svc);IOObjectRelease(svc); if(!dev)return 2;
    IOReturn r=(*dev)->USBDeviceOpen(dev); if(r==kIOReturnExclusiveAccess)r=(*dev)->USBDeviceOpenSeize(dev);
    if(r!=kIOReturnSuccess){printf("  open failed 0x%x\n",r);return 3;} (*dev)->SetConfiguration(dev,1);
    gDev=dev;
    uint8_t clk = getenv("ER_CLOCK")?atoi(getenv("ER_CLOCK")):1;   // 1=internal
    IOReturn cs=setClockSource(dev, clk);
    printf("  SET clock source %d (1=internal): 0x%x %s\n", clk, cs, cs==kIOReturnSuccess?"OK":"(failed)");
    // Adopt the CoreAudio device's current (persisted) rate so the hardware and
    // the UI agree at startup. Retry briefly in case coreaudiod is still loading.
    uint32_t rate;
    if(getenv("ER_SAMPLERATE")) rate=atoi(getenv("ER_SAMPLERATE"));
    else { rate=queryDeviceRateBounded(1.5);   // bounded: never hang on a busy coreaudiod
           if(!rate) rate=ER_RATE; else printf("  adopting CoreAudio device rate %u Hz\n",rate); }
    IOReturn sr=setSampleRate(dev, rate);
    gHwRate=rate;
    printf("  SET_CUR sample rate %u Hz: 0x%x %s\n", rate, sr, sr==kIOReturnSuccess?"OK":"(failed)");
    { uint32_t got=0; IOUSBDevRequest q={0xA1,0x01,0x0100,0x8101,4,&got,0};
      (*dev)->DeviceRequest(dev,&q); printf("  GET_CUR readback: device reports %u Hz\n", got); }
    gIn=openIface(dev,ER_IF_IN); gOut=openIface(dev,ER_IF_OUT);
    if(!gIn||!gOut){printf("  audio interfaces missing\n");return 4;}
    (*gIn)->USBInterfaceOpen(gIn);(*gOut)->USBInterfaceOpen(gOut);
    (*gIn)->SetAlternateInterface(gIn,ER_ALT);(*gOut)->SetAlternateInterface(gOut,ER_ALT);
    UInt16 im=0,om=0; gInPipe=findPipe(gIn,true,&im); gOutPipe=findPipe(gOut,false,&om);
    // HIGH-SPEED: interval=1 = 8 microframes/ms. Request one microframe (maxPacket)
    // per frame-list entry; FPR entries per request, N_REQ requests in flight.
    int base = im & 0x7FF, mult = ((im>>11)&3)+1;
    gInReqLen = getenv("ER_REQLEN") ? atoi(getenv("ER_REQLEN")) : base*mult;
    if(!gInPipe||!gOutPipe){printf("  isoc pipes missing\n");return 5;}
    printf("  USB: claimed, config 1, IN pipe %u (maxPkt base=%d mult=%d, req %u B/frame), OUT pipe %u\n",
           gInPipe,base,mult,gInReqLen,gOutPipe);

    if(!gWavMode && !gRawFile && !gFramesFile){
        int created=0; gRing=er_ring_create(&created);
        if(!gRing){printf("  WARNING: shared ring unavailable (audio won't reach CoreAudio)\n");}
        else { er_store32(&gRing->engineRunning,1); er_store32(&gRing->sampleRate,gHwRate);
               // Reset the time-addressed playback heads: an attached (persisted) ring
               // may hold a stale outWriteMax from a previous session whose sample-time
               // origin differs from coreaudiod's fresh StartIO timeline (which starts
               // at 0). Without this, coreaudiod's new small sample times never exceed
               // the stale value, outWriteMax never advances, and playback is silent.
               er_store(&gRing->outWriteMax,0); er_store(&gRing->outPlayHead,0); gPlayInit=0;
               printf("  ring: %s (rate %u Hz)\n",created?"created":"attached",gHwRate); }
        // MIDI needs nothing from us: the Eleven Rack's USB-MIDI interface is a
        // standard class device that macOS exposes directly ("Eleven Rack Rig" /
        // "Eleven Rack External" ports via AppleMIDIUSBDriver).
    }

    for(int i=0;i<N_REQ;i++){gInReqs[i].data=malloc(FPR*MAX_PKT);memset(gInReqs[i].data,0,FPR*MAX_PKT);
                             gOutReqs[i].data=malloc(FPR*MAX_PKT);memset(gOutReqs[i].data,0,FPR*MAX_PKT);}
    CFRunLoopSourceRef si=NULL,so=NULL;
    (*gIn)->CreateInterfaceAsyncEventSource(gIn,&si);(*gOut)->CreateInterfaceAsyncEventSource(gOut,&so);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),si,kCFRunLoopDefaultMode);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),so,kCFRunLoopDefaultMode);
    // Do any slow output setup BEFORE scheduling USB, so the scheduled isoc
    // frames don't go stale while we initialize (which stalls streaming).
    if(gMonitor){ printf("  decode: %d-word unit, lanes %d/%d\n",SF_WORDS,SF_LANE_L,SF_LANE_R); startMonitor(monRate); }

    UInt64 bus=0;AbsoluteTime at;(*gIn)->GetBusFrameNumber(gIn,&bus,&at); gInFrame=gOutFrame=bus+25;
    uint64_t t0=mach_absolute_time();
    resetMeters();
    for(int i=0;i<N_REQ;i++){armOut(&gOutReqs[i]);armIn(&gInReqs[i]);}

    signal(SIGINT,onSig);
    signal(SIGTERM,onSig);   // clean shutdown on app-quit / agent-stop, releasing the device
    if(gWavMode||gRawFile||gFramesFile){ printf("  recording %.1fs — play a sustained note now...\n",wavSecs);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,wavSecs,false);
        gStop=1;
    } else {
        // Poll the ring for DAW/AMS sample-rate changes and retune the hardware.
        CFRunLoopTimerRef rt=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+0.05,0.05,0,0,rateFollow,NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(),rt,kCFRunLoopDefaultMode);
        printf("  streaming (rate-follow active). Ctrl-C to stop.\n");
        CFRunLoopRun();
    }

    // teardown
    (*gIn)->SetAlternateInterface(gIn,0);(*gOut)->SetAlternateInterface(gOut,0);
    (*gIn)->USBInterfaceClose(gIn);(*gOut)->USBInterfaceClose(gOut);
    (*gIn)->Release(gIn);(*gOut)->Release(gOut);(*dev)->USBDeviceClose(dev);(*dev)->Release(dev);

    // measured capture rate = superframes / elapsed seconds
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double elapsed = (mach_absolute_time()-t0)*(double)tb.numer/tb.denom/1e9;
    double measuredRate = gSFCount/elapsed;
    printf("\n  captured %llu stereo frames in %.2fs → measured rate %.0f Hz\n",
           gSFCount, elapsed, measuredRate);

    if(gRawFile){ fclose(gRawFile);
        double wordRate=gRawWords/elapsed;
        printf("  wrote %s: %llu int32 words in %.2fs → %.0f words/s\n",rawPath,gRawWords,elapsed,wordRate);
    }
    if(gFramesFile){ fclose(gFramesFile);
        printf("  wrote %s: %llu USB packets in %.2fs (%.0f/s)\n",rawPath,gFrameCount,elapsed,gFrameCount/elapsed);
    }
    if(gWavMode && gWav && gWavN) writeWav(wavPath, measuredRate>1000?measuredRate:48000);
    // Unmap but do NOT unlink: keep the shared ring as one persistent object so a
    // restarting engine reuses it and every consumer (the menu-bar app, coreaudiod)
    // keeps a valid mapping across restarts instead of orphaning onto a dead ring.
    if(gRing){ er_store32(&gRing->engineRunning,0); er_ring_close(gRing,0); }
    printf("  done.\n");
    return 0;
}
