/**
 * @file ElevenRackAudioPlugin.cpp
 * @brief Core Audio @c AudioServerPlugin that exposes the Eleven Rack as a device.
 *
 * Loaded by @c coreaudiod, this plugin publishes the device object model and
 * exchanges float32 audio with the user-space USB engine through a shared-memory
 * ring (::ERRing). It contains no USB or hardware code itself.
 *
 * Architecture:
 * @code
 *   coreaudiod  ↔  [this plugin]  ↔  shared-memory ring  ↔  user-space engine  ↔  USB device
 * @endcode
 *
 * Object model:
 * - Plugin  (::kObjectID_PlugIn = 1)
 *   - Device  (::kObjectID_Device = 2)
 *     - Output Stream (::kObjectID_Stream_Output = 3) — playback (6 ch)
 *     - Input  Stream (::kObjectID_Stream_Input  = 4) — capture  (8 ch)
 *
 * The @c AudioServerPlugInDriverInterface exposes a single set of five property
 * functions; the @c ERP_*Router functions dispatch them by object ID to the
 * per-object @c ERP_PlugIn/@c ERP_Device/@c ERP_Stream handlers.
 *
 * @see AudioServerPlugIn.h (Apple's NullAudio sample follows the same pattern).
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>          // kAudioDevicePropertyBufferFrameSize et al.
#include <CoreFoundation/CoreFoundation.h>
#include "../ElevenRackBridge/ERAudioRing.h"   // shared-memory transport to the app's USB engine
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/** @brief Fixed object IDs for the plugin's static object model. */
enum : AudioObjectID {
    kObjectID_PlugIn        = kAudioObjectPlugInObject,   /**< The plugin object (= 1). */
    kObjectID_Device        = 2,                          /**< The Eleven Rack device object. */
    kObjectID_Stream_Output = 3,                          /**< The playback stream object. */
    kObjectID_Stream_Input  = 4,                          /**< The capture stream object. */
};

/** @name Device identity and format constants @{ */
#define kDeviceUID      "com.TR6SHJFRF8.elevenrack.device" /**< Persistent device UID. */
#define kDeviceModelUID "com.TR6SHJFRF8.elevenrack.model"  /**< Device model UID. */
#define kDeviceName     "Eleven Rack"                      /**< Human-readable device name. */
#define kManufacturer   "Avid"                             /**< Manufacturer string. */

// All four rates the Eleven Rack supports. Changing the rate makes the plugin
// publish it to the ring; the engine then retunes the hardware clock to match.
static const Float64 kSampleRates[]     = { 44100.0, 48000.0, 88200.0, 96000.0 };
static const UInt32  kNumSampleRates    = 4;
static const UInt32  kNumOutputChannels = ER_OUT_CH;   /**< Playback channels (6). */
static const UInt32  kNumInputChannels  = ER_IN_CH;    /**< Capture channels (8). */

/** @brief Capture channel names (order verified empirically), reported per element. */
static const char* const kInputChannelNames[ER_IN_CH] = {
    "Guitar In", "Mic In", "Eleven Rig L", "Eleven Rig R",
    "Digital In L", "Digital In R", "Line In L", "Line In R"
};
/** @brief Playback channel names, reported per element. */
static const char* const kOutputChannelNames[ER_OUT_CH] = {
    "Main Out L", "Main Out R", "Re-Amp L", "Re-Amp R", "Digital Out L", "Digital Out R"
};
static const UInt32  kMinBufferFrames   = 64;    /**< Minimum IO buffer size (frames). */
static const UInt32  kMaxBufferFrames   = 2048;  /**< Maximum IO buffer size (frames). */
static const UInt32  kDefaultFrameSize  = 256;   /**< Default IO buffer size (frames). */

static const UInt32  kOutputHWLatency   = 256;   /**< Reported playback latency (frames). */
static const UInt32  kInputHWLatency    = 256;   /**< Reported capture latency (frames). */
/** @} */

/** @name Global state (guarded by ::sMutex except where noted) @{ */
static os_log_t       sLog              = nullptr;                  /**< Unified-logging handle. */
static pthread_mutex_t sMutex           = PTHREAD_MUTEX_INITIALIZER; /**< Guards the state below. */

static AudioServerPlugInHostRef sHost   = nullptr;          /**< Host interface from Initialize. */
static Float64  sSampleRate             = 48000.0;          /**< Current nominal sample rate. */
static UInt32   sIOBufferFrameSize      = kDefaultFrameSize;/**< Current IO buffer size (frames). */
static UInt32   sIORunningCount         = 0;                /**< Number of clients currently doing IO. */
static bool     sIORunning              = false;            /**< True while IO is active. */

/** @name Timing anchor (updated each GetZeroTimeStamp period; lock-free) @{ */
static volatile UInt64  sAnchorSampleTime   = 0;   /**< Anchor sample time (legacy; kept for config-change). */
static volatile UInt64  sAnchorHostTime     = 0;   /**< Anchor host time (mach ticks) set at StartIO. */
static volatile UInt64  sAnchorSeed         = 1;   /**< Timeline seed; bumped on discontinuities. */
static volatile Float64 sHostTicksPerFrame  = 0.0; /**< Host ticks per audio frame (nominal). */
// Zero-timestamp state (BlackHole model): the timeline advances by ER_ZTS_PERIOD
// frames each time a full ring-period of host ticks elapses from the anchor.
static UInt64           sNumTimeStamps      = 0;   /**< Count of elapsed zero-timestamp periods. */
static double           sPrevTicks          = 0.0; /**< Host ticks accumulated at the last period. */
static pthread_mutex_t  sIOTimeMutex        = PTHREAD_MUTEX_INITIALIZER; /**< Guards the zero-timestamp state. */
/** @} */

/** @brief Shared transport to the USB engine (attached lazily; NULL when unavailable). */
static ERRing*  sRing                   = nullptr;
/** @} */

// ---------------------------------------------------------------------------
// Forward declarations for all AudioServerPlugIn functions
// ---------------------------------------------------------------------------

// IUnknown
static HRESULT  ERP_QueryInterface(void*, REFIID, LPVOID*);
static ULONG    ERP_AddRef(void*);
static ULONG    ERP_Release(void*);

// Plugin lifecycle
static OSStatus ERP_Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus ERP_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus ERP_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus ERP_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus ERP_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus ERP_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus ERP_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);

// Plugin properties
static Boolean  ERP_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus ERP_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus ERP_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus ERP_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus ERP_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);

// Device properties
static Boolean  ERP_Device_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus ERP_Device_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus ERP_Device_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus ERP_Device_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus ERP_Device_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);

// Stream properties
static Boolean  ERP_Stream_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus ERP_Stream_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus ERP_Stream_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus ERP_Stream_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus ERP_Stream_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);

// Control properties (stub — no controls implemented)
static Boolean  ERP_Control_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus ERP_Control_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus ERP_Control_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus ERP_Control_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus ERP_Control_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);

// IO
static OSStatus ERP_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus ERP_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus ERP_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus ERP_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus ERP_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus ERP_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus ERP_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

/**
 * @name Property routers
 * The @c AudioServerPlugInDriverInterface has a single set of five property
 * functions; these routers dispatch by object ID to the per-object handlers
 * (plugin / device / stream). Control objects are not instantiated, so the
 * @c ERP_Control_* handlers remain unused.
 * @{
 */

/** @brief True if @p o is one of the stream objects. */
static bool ERP_IsStream(AudioObjectID o) {
    return o == kObjectID_Stream_Input || o == kObjectID_Stream_Output;
}

/** @brief Route @c HasProperty to the handler for object @p o. */
static Boolean ERP_HasPropertyRouter(AudioServerPlugInDriverRef d, AudioObjectID o,
                                     pid_t c, const AudioObjectPropertyAddress* a) {
    if (o == kObjectID_PlugIn) return ERP_HasProperty(d, o, c, a);
    if (o == kObjectID_Device) return ERP_Device_HasProperty(d, o, c, a);
    if (ERP_IsStream(o))       return ERP_Stream_HasProperty(d, o, c, a);
    return false;
}
/** @brief Route @c IsPropertySettable to the handler for object @p o. */
static OSStatus ERP_IsPropertySettableRouter(AudioServerPlugInDriverRef d, AudioObjectID o,
                                             pid_t c, const AudioObjectPropertyAddress* a, Boolean* out) {
    if (o == kObjectID_PlugIn) return ERP_IsPropertySettable(d, o, c, a, out);
    if (o == kObjectID_Device) return ERP_Device_IsPropertySettable(d, o, c, a, out);
    if (ERP_IsStream(o))       return ERP_Stream_IsPropertySettable(d, o, c, a, out);
    return kAudioHardwareBadObjectError;
}
/** @brief Route @c GetPropertyDataSize to the handler for object @p o. */
static OSStatus ERP_GetPropertyDataSizeRouter(AudioServerPlugInDriverRef d, AudioObjectID o,
                                              pid_t c, const AudioObjectPropertyAddress* a,
                                              UInt32 qs, const void* q, UInt32* outSize) {
    if (o == kObjectID_PlugIn) return ERP_GetPropertyDataSize(d, o, c, a, qs, q, outSize);
    if (o == kObjectID_Device) return ERP_Device_GetPropertyDataSize(d, o, c, a, qs, q, outSize);
    if (ERP_IsStream(o))       return ERP_Stream_GetPropertyDataSize(d, o, c, a, qs, q, outSize);
    return kAudioHardwareBadObjectError;
}
/** @brief Route @c GetPropertyData to the handler for object @p o. */
static OSStatus ERP_GetPropertyDataRouter(AudioServerPlugInDriverRef d, AudioObjectID o,
                                          pid_t c, const AudioObjectPropertyAddress* a,
                                          UInt32 qs, const void* q, UInt32 inSize,
                                          UInt32* ioSize, void* out) {
    if (o == kObjectID_PlugIn) return ERP_GetPropertyData(d, o, c, a, qs, q, inSize, ioSize, out);
    if (o == kObjectID_Device) return ERP_Device_GetPropertyData(d, o, c, a, qs, q, inSize, ioSize, out);
    if (ERP_IsStream(o))       return ERP_Stream_GetPropertyData(d, o, c, a, qs, q, inSize, ioSize, out);
    return kAudioHardwareBadObjectError;
}
/** @brief Route @c SetPropertyData to the handler for object @p o. */
static OSStatus ERP_SetPropertyDataRouter(AudioServerPlugInDriverRef d, AudioObjectID o,
                                          pid_t c, const AudioObjectPropertyAddress* a,
                                          UInt32 qs, const void* q, UInt32 size, const void* data) {
    if (o == kObjectID_PlugIn) return ERP_SetPropertyData(d, o, c, a, qs, q, size, data);
    if (o == kObjectID_Device) return ERP_Device_SetPropertyData(d, o, c, a, qs, q, size, data);
    if (ERP_IsStream(o))       return ERP_Stream_SetPropertyData(d, o, c, a, qs, q, size, data);
    return kAudioHardwareBadObjectError;
}
/** @} */

/** @brief The driver interface vtable handed to @c coreaudiod (real 23-entry layout). */
static AudioServerPlugInDriverInterface sInterface = {
    nullptr,                          // _reserved
    ERP_QueryInterface,
    ERP_AddRef,
    ERP_Release,
    ERP_Initialize,
    ERP_CreateDevice,
    ERP_DestroyDevice,
    ERP_AddDeviceClient,
    ERP_RemoveDeviceClient,
    ERP_PerformDeviceConfigurationChange,
    ERP_AbortDeviceConfigurationChange,
    ERP_HasPropertyRouter,
    ERP_IsPropertySettableRouter,
    ERP_GetPropertyDataSizeRouter,
    ERP_GetPropertyDataRouter,
    ERP_SetPropertyDataRouter,
    ERP_StartIO,
    ERP_StopIO,
    ERP_GetZeroTimeStamp,
    ERP_WillDoIOOperation,
    ERP_BeginIOOperation,
    ERP_DoIOOperation,
    ERP_EndIOOperation,
};

/**
 * @brief The plugin instance. A pointer to it is the @c AudioServerPlugInDriverRef;
 *        its first member is the vtable, per the COM-style interface convention.
 */
struct ElevenRackPlugIn {
    AudioServerPlugInDriverInterface* mInterface;   /**< Interface vtable (must be first). */
    atomic_uint mRefCount;                          /**< COM reference count. */
};

/** @brief The single plugin instance returned by the factory. */
static ElevenRackPlugIn sPlugIn = { &sInterface, 1u };

/** @name IUnknown @{ */

/** @brief COM @c QueryInterface: hand out the driver interface for the known UUIDs. */
static HRESULT ERP_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (!outInterface) return E_POINTER;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(nullptr, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID) ||
        CFEqual(uuid, IUnknownUUID)) {
        ERP_AddRef(inDriver);
        *outInterface = inDriver;
        result = S_OK;
    }
    CFRelease(uuid);
    return result;
}

/** @brief COM @c AddRef: increment and return the reference count. */
static ULONG ERP_AddRef(void* inDriver)
{
    auto* plug = static_cast<ElevenRackPlugIn*>(inDriver);
    return atomic_fetch_add(&plug->mRefCount, 1u) + 1;
}

/** @brief COM @c Release: decrement and return the reference count. */
static ULONG ERP_Release(void* inDriver)
{
    auto* plug = static_cast<ElevenRackPlugIn*>(inDriver);
    ULONG prev = atomic_fetch_sub(&plug->mRefCount, 1u);
    return (prev > 1) ? prev - 1 : 0;
}
/** @} */

/** @name Internal helpers @{ */

/** @brief Recompute ::sHostTicksPerFrame from the mach timebase and ::sSampleRate. */
static void RecomputeHostTicksPerFrame()
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    // ticks per nanosecond = denom/numer; ticks per second = denom/numer × 1e9
    sHostTicksPerFrame = ((double)tb.denom / (double)tb.numer) * 1e9 / sSampleRate;
}

/**
 * @brief Attach to the shared ring created by the USB engine (idempotent).
 *
 * A no-op once attached. If the engine is not running the region does not exist
 * and the plugin stays detached, reading/writing silence until it appears.
 */
static void TryAttachRing()
{
    if (sRing) return;
    sRing = er_ring_attach();
    if (sRing) os_log(sLog, "ElevenRack plugin: attached to audio ring (rate=%u)",
                      er_load32(&sRing->sampleRate));
}

/** @brief Detach from the shared ring (does not remove the shared name). */
static void DetachRing()
{
    if (sRing) { er_ring_close(sRing, /*unlink=*/0); sRing = nullptr; }
}

/** @brief Signal the engine that Core Audio wants audio flowing (power hint). */
static void RingStartStreaming()
{
    TryAttachRing();
    if (sRing) er_store32(&sRing->streamingRequested, 1);
}

/** @brief Clear the streaming-requested hint. */
static void RingStopStreaming()
{
    if (sRing) er_store32(&sRing->streamingRequested, 0);
}
/** @} */

/** @name Plugin lifecycle @{ */

/**
 * @brief Initialize the plugin: create the log and record the host interface.
 * @param inDriver  This driver reference.
 * @param inHost    Host interface for notifications (stored in ::sHost).
 * @return @c kAudioHardwareNoError.
 */
static OSStatus ERP_Initialize(AudioServerPlugInDriverRef inDriver,
                                AudioServerPlugInHostRef   inHost)
{
    (void)inDriver;
    sLog  = os_log_create("com.TR6SHJFRF8.ElevenRackAudioPlugin", "driver");
    sHost = inHost;

    RecomputeHostTicksPerFrame();
    // No buffers to pre-allocate: audio flows through the shared ring, attached
    // lazily on StartIO. The device object model is static (see the property
    // handlers), so there is nothing to announce to the host here.
    os_log(sLog, "ElevenRack plugin: initialized");
    return kAudioHardwareNoError;
}

/** @brief Not supported: this plugin publishes a single static device. */
static OSStatus ERP_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                                  const AudioServerPlugInClientInfo*, AudioObjectID*)
{
    return kAudioHardwareUnsupportedOperationError;
}

/** @brief Not supported: the device is static and cannot be destroyed. */
static OSStatus ERP_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID)
{
    return kAudioHardwareUnsupportedOperationError;
}

/** @brief A client began using the device (no per-client state is kept). */
static OSStatus ERP_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID,
                                     const AudioServerPlugInClientInfo*)
{
    return kAudioHardwareNoError;
}

/** @brief A client stopped using the device (no per-client state is kept). */
static OSStatus ERP_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID,
                                        const AudioServerPlugInClientInfo*)
{
    return kAudioHardwareNoError;
}

/**
 * @brief Apply a device configuration change (a sample-rate change).
 *
 * @p changeAction carries the new rate as a bit-cast @c Float64. Updates the
 * timing state, resets the timeline anchor and bumps the seed (so Core Audio
 * re-locks cleanly), and publishes the new rate to the ring for the engine.
 */
static OSStatus ERP_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                      AudioObjectID, UInt64 changeAction,
                                                      void*)
{
    pthread_mutex_lock(&sMutex);
    if (changeAction != 0) {
        // changeAction encodes new sample rate as a bitcast double
        Float64 newRate;
        memcpy(&newRate, &changeAction, sizeof(newRate));
        sSampleRate = newRate;
        RecomputeHostTicksPerFrame();
        // Reset the zero-timestamp state and bump the seed so CoreAudio treats the
        // new rate as a fresh timeline (avoids a kink → glitches while it re-locks).
        pthread_mutex_lock(&sIOTimeMutex);
        sNumTimeStamps    = 0;
        sAnchorSampleTime = 0;
        sAnchorHostTime   = mach_absolute_time();
        sPrevTicks        = 0.0;
        sAnchorSeed++;
        pthread_mutex_unlock(&sIOTimeMutex);
        if (!sRing) TryAttachRing();      // publish rate even if IO isn't running yet
        if (sRing) er_store32(&sRing->sampleRate, (uint32_t)newRate);
    }
    pthread_mutex_unlock(&sMutex);
    return kAudioHardwareNoError;
}

/** @brief Abort a pending configuration change (nothing to roll back). */
static OSStatus ERP_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef,
                                                    AudioObjectID, UInt64, void*)
{
    return kAudioHardwareNoError;
}
/** @} */

/**
 * @name Plugin-object properties
 * Handlers for the ::kObjectID_PlugIn object (class, manufacturer, device list, …).
 * @{
 */

/** @brief Whether the plugin object has a given property. */
static Boolean ERP_HasProperty(AudioServerPlugInDriverRef,
                                AudioObjectID inObjectID, pid_t,
                                const AudioObjectPropertyAddress* inAddress)
{
    if (inObjectID != kObjectID_PlugIn) return false;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyName:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyResourceBundle:
        return true;
    default:
        return false;
    }
}

/** @brief Whether a plugin-object property is settable (none are). */
static OSStatus ERP_IsPropertySettable(AudioServerPlugInDriverRef,
                                        AudioObjectID, pid_t,
                                        const AudioObjectPropertyAddress* inAddress,
                                        Boolean* outIsSettable)
{
    *outIsSettable = false;
    return kAudioHardwareNoError;
}

/** @brief Byte size of a plugin-object property's value. */
static OSStatus ERP_GetPropertyDataSize(AudioServerPlugInDriverRef,
                                         AudioObjectID inObjectID, pid_t,
                                         const AudioObjectPropertyAddress* inAddress,
                                         UInt32, const void*, UInt32* outDataSize)
{
    if (inObjectID != kObjectID_PlugIn) return kAudioHardwareBadObjectError;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:     *outDataSize = sizeof(AudioClassID);  break;
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyName:      *outDataSize = sizeof(CFStringRef);   break;
    case kAudioPlugInPropertyDeviceList:*outDataSize = sizeof(AudioObjectID); break;
    case kAudioPlugInPropertyTranslateUIDToDevice:
                                        *outDataSize = sizeof(AudioObjectID); break;
    case kAudioPlugInPropertyResourceBundle:
                                        *outDataSize = sizeof(CFStringRef);   break;
    default: return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareNoError;
}

/** @brief Read a plugin-object property (class, manufacturer, device list, …). */
static OSStatus ERP_GetPropertyData(AudioServerPlugInDriverRef,
                                     AudioObjectID inObjectID, pid_t,
                                     const AudioObjectPropertyAddress* inAddress,
                                     UInt32 inQualifierDataSize,
                                     const void* inQualifierData,
                                     UInt32 /*inDataSize*/,
                                     UInt32* ioDataSize, void* outData)
{
    if (inObjectID != kObjectID_PlugIn) return kAudioHardwareBadObjectError;
    OSStatus err = kAudioHardwareNoError;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
        *static_cast<AudioClassID*>(outData) = kAudioObjectClassID;
        *ioDataSize = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyClass:
        *static_cast<AudioClassID*>(outData) = kAudioPlugInClassID;
        *ioDataSize = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyOwner:
        *static_cast<AudioObjectID*>(outData) = kAudioObjectUnknown;
        *ioDataSize = sizeof(AudioObjectID);
        break;
    case kAudioObjectPropertyManufacturer:
        *static_cast<CFStringRef*>(outData) = CFSTR(kManufacturer);
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    case kAudioObjectPropertyName:
        *static_cast<CFStringRef*>(outData) = CFSTR("ElevenRack AudioServerPlugin");
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    case kAudioPlugInPropertyDeviceList:
        *static_cast<AudioObjectID*>(outData) = kObjectID_Device;
        *ioDataSize = sizeof(AudioObjectID);
        break;
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        // Qualifier is a CFStringRef UID
        if (inQualifierDataSize < sizeof(CFStringRef)) { err = kAudioHardwareBadPropertySizeError; break; }
        CFStringRef uid = *static_cast<const CFStringRef*>(inQualifierData);
        CFStringRef devUID = CFSTR(kDeviceUID);
        *static_cast<AudioObjectID*>(outData) =
            CFEqual(uid, devUID) ? kObjectID_Device : kAudioObjectUnknown;
        *ioDataSize = sizeof(AudioObjectID);
        break;
    }
    case kAudioPlugInPropertyResourceBundle:
        *static_cast<CFStringRef*>(outData) = CFSTR("");
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    default:
        err = kAudioHardwareUnknownPropertyError;
        break;
    }
    return err;
}

/** @brief Write a plugin-object property (none are writable). */
static OSStatus ERP_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                                     const AudioObjectPropertyAddress*, UInt32, const void*,
                                     UInt32, const void*)
{
    return kAudioHardwareUnsupportedOperationError;
}
/** @} */

/**
 * @name Device-object properties
 * Handlers for the ::kObjectID_Device object: name, transport, streams, sample
 * rate, buffer size, channel layout/names, etc.
 * @{
 */

/** @brief Whether the device object has a given property. */
static Boolean ERP_Device_HasProperty(AudioServerPlugInDriverRef,
                                       AudioObjectID inObjectID, pid_t,
                                       const AudioObjectPropertyAddress* inAddress)
{
    if (inObjectID != kObjectID_Device) return false;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertyStreams:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioDevicePropertyBufferFrameSize:
    case kAudioDevicePropertyBufferFrameSizeRange:
        return true;
    case kAudioObjectPropertyElementName: {
        UInt32 el = inAddress->mElement;   // 1-based channel number
        if (inAddress->mScope == kAudioObjectPropertyScopeInput)
            return el >= 1 && el <= kNumInputChannels;
        if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
            return el >= 1 && el <= kNumOutputChannels;
        return false;
    }
    default:
        return false;
    }
}

/** @brief Whether a device property is settable (sample rate and buffer size are). */
static OSStatus ERP_Device_IsPropertySettable(AudioServerPlugInDriverRef,
                                               AudioObjectID, pid_t,
                                               const AudioObjectPropertyAddress* inAddress,
                                               Boolean* outIsSettable)
{
    switch (inAddress->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyBufferFrameSize:
        *outIsSettable = true;
        break;
    default:
        *outIsSettable = false;
        break;
    }
    return kAudioHardwareNoError;
}

/** @brief Byte size of a device property's value. */
static OSStatus ERP_Device_GetPropertyDataSize(AudioServerPlugInDriverRef,
                                                AudioObjectID inObjectID, pid_t,
                                                const AudioObjectPropertyAddress* inAddress,
                                                UInt32, const void*, UInt32* outDataSize)
{
    if (inObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioDevicePropertyBufferFrameSize:
        *outDataSize = sizeof(UInt32); break;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioObjectPropertyElementName:
        *outDataSize = sizeof(CFStringRef); break;
    case kAudioObjectPropertyOwnedObjects:
        *outDataSize = 2 * sizeof(AudioObjectID); break;  // 2 streams
    case kAudioDevicePropertyRelatedDevices:
        *outDataSize = sizeof(AudioObjectID); break;
    case kAudioDevicePropertyStreams: {
        UInt32 n = (inAddress->mScope == kAudioObjectPropertyScopeInput)  ? 1 :
                   (inAddress->mScope == kAudioObjectPropertyScopeOutput) ? 1 : 2;
        *outDataSize = n * sizeof(AudioObjectID); break;
    }
    case kAudioObjectPropertyControlList:
        *outDataSize = 0; break;
    case kAudioDevicePropertyNominalSampleRate:
        *outDataSize = sizeof(Float64); break;
    case kAudioDevicePropertyAvailableNominalSampleRates:
        *outDataSize = kNumSampleRates * sizeof(AudioValueRange); break;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        *outDataSize = 2 * sizeof(UInt32); break;
    case kAudioDevicePropertyPreferredChannelLayout: {
        UInt32 ch = (inAddress->mScope == kAudioObjectPropertyScopeInput)
                    ? kNumInputChannels : kNumOutputChannels;
        *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions)
                       + ch * sizeof(AudioChannelDescription);
        break;
    }
    case kAudioDevicePropertyBufferFrameSizeRange:
        *outDataSize = sizeof(AudioValueRange); break;
    default: return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareNoError;
}

/** @brief Read a device property (name, streams, rate, layout, channel names, …). */
static OSStatus ERP_Device_GetPropertyData(AudioServerPlugInDriverRef,
                                            AudioObjectID inObjectID, pid_t,
                                            const AudioObjectPropertyAddress* inAddress,
                                            UInt32, const void*, UInt32 /*inDataSize*/,
                                            UInt32* ioDataSize, void* outData)
{
    if (inObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    OSStatus err = kAudioHardwareNoError;

    pthread_mutex_lock(&sMutex);
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
        *static_cast<AudioClassID*>(outData) = kAudioDeviceClassID;
        *ioDataSize = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyClass:
        *static_cast<AudioClassID*>(outData) = kAudioDeviceClassID;
        *ioDataSize = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyOwner:
        *static_cast<AudioObjectID*>(outData) = kObjectID_PlugIn;
        *ioDataSize = sizeof(AudioObjectID);
        break;
    case kAudioObjectPropertyName:
        *static_cast<CFStringRef*>(outData) = CFSTR(kDeviceName);
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    case kAudioObjectPropertyManufacturer:
        *static_cast<CFStringRef*>(outData) = CFSTR(kManufacturer);
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    case kAudioDevicePropertyDeviceUID:
        *static_cast<CFStringRef*>(outData) = CFSTR(kDeviceUID);
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    case kAudioDevicePropertyModelUID:
        *static_cast<CFStringRef*>(outData) = CFSTR(kDeviceModelUID);
        CFRetain(*static_cast<CFStringRef*>(outData));
        *ioDataSize = sizeof(CFStringRef);
        break;
    case kAudioObjectPropertyElementName: {
        UInt32 el = inAddress->mElement;   // 1-based channel number
        const char* nm = nullptr;
        if (inAddress->mScope == kAudioObjectPropertyScopeInput && el >= 1 && el <= kNumInputChannels)
            nm = kInputChannelNames[el - 1];
        else if (inAddress->mScope == kAudioObjectPropertyScopeOutput && el >= 1 && el <= kNumOutputChannels)
            nm = kOutputChannelNames[el - 1];
        if (!nm) { err = kAudioHardwareUnknownPropertyError; break; }
        *static_cast<CFStringRef*>(outData) =
            CFStringCreateWithCString(nullptr, nm, kCFStringEncodingUTF8);
        *ioDataSize = sizeof(CFStringRef);
        break;
    }
    case kAudioDevicePropertyTransportType:
        *static_cast<UInt32*>(outData) = kAudioDeviceTransportTypeUSB;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyRelatedDevices:
        *static_cast<AudioObjectID*>(outData) = kObjectID_Device;
        *ioDataSize = sizeof(AudioObjectID);
        break;
    case kAudioDevicePropertyClockDomain:
        *static_cast<UInt32*>(outData) = 0;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyDeviceIsAlive:
        *static_cast<UInt32*>(outData) = 1;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyDeviceIsRunning:
        *static_cast<UInt32*>(outData) = sIORunning ? 1 : 0;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        *static_cast<UInt32*>(outData) = 1;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyLatency:
        *static_cast<UInt32*>(outData) =
            (inAddress->mScope == kAudioObjectPropertyScopeInput) ? kInputHWLatency : kOutputHWLatency;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertySafetyOffset:
        *static_cast<UInt32*>(outData) = 0;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyStreams: {
        AudioObjectID* ids = static_cast<AudioObjectID*>(outData);
        if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
            if (*ioDataSize >= sizeof(AudioObjectID)) {
                ids[0] = kObjectID_Stream_Input;
                *ioDataSize = sizeof(AudioObjectID);
            }
        } else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) {
            if (*ioDataSize >= sizeof(AudioObjectID)) {
                ids[0] = kObjectID_Stream_Output;
                *ioDataSize = sizeof(AudioObjectID);
            }
        } else {
            // Global: both streams
            if (*ioDataSize >= 2 * sizeof(AudioObjectID)) {
                ids[0] = kObjectID_Stream_Output;
                ids[1] = kObjectID_Stream_Input;
                *ioDataSize = 2 * sizeof(AudioObjectID);
            }
        }
        break;
    }
    case kAudioObjectPropertyOwnedObjects: {
        AudioObjectID* ids = static_cast<AudioObjectID*>(outData);
        ids[0] = kObjectID_Stream_Output;
        ids[1] = kObjectID_Stream_Input;
        *ioDataSize = 2 * sizeof(AudioObjectID);
        break;
    }
    case kAudioObjectPropertyControlList:
        *ioDataSize = 0;
        break;
    case kAudioDevicePropertyIsHidden:
        *static_cast<UInt32*>(outData) = 0;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyNominalSampleRate:
        *static_cast<Float64*>(outData) = sSampleRate;
        *ioDataSize = sizeof(Float64);
        break;
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        AudioValueRange* ranges = static_cast<AudioValueRange*>(outData);
        for (UInt32 i = 0; i < kNumSampleRates; ++i) {
            ranges[i].mMinimum = kSampleRates[i];
            ranges[i].mMaximum = kSampleRates[i];
        }
        *ioDataSize = kNumSampleRates * sizeof(AudioValueRange);
        break;
    }
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        UInt32* ch = static_cast<UInt32*>(outData);
        ch[0] = 1; ch[1] = 2;
        *ioDataSize = 2 * sizeof(UInt32);
        break;
    }
    case kAudioDevicePropertyPreferredChannelLayout: {
        UInt32 numCh = (inAddress->mScope == kAudioObjectPropertyScopeInput)
                       ? kNumInputChannels : kNumOutputChannels;
        AudioChannelLayout* layout = static_cast<AudioChannelLayout*>(outData);
        layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
        layout->mChannelBitmap    = 0;
        layout->mNumberChannelDescriptions = numCh;
        for (UInt32 i = 0; i < numCh; ++i) {
            layout->mChannelDescriptions[i].mChannelLabel =
                (i == 0) ? kAudioChannelLabel_Left : kAudioChannelLabel_Right;
            layout->mChannelDescriptions[i].mChannelFlags = 0;
            memset(layout->mChannelDescriptions[i].mCoordinates, 0,
                   sizeof(layout->mChannelDescriptions[i].mCoordinates));
        }
        *ioDataSize = offsetof(AudioChannelLayout, mChannelDescriptions)
                      + numCh * sizeof(AudioChannelDescription);
        break;
    }
    case kAudioDevicePropertyZeroTimeStampPeriod:
        // The timeline's ring-buffer period — must match GetZeroTimeStamp's stride
        // and the sample-time addressing in DoIOOperation (NOT the IO buffer size,
        // which is what wedged coreaudiod when it disagreed with the addressing).
        *static_cast<UInt32*>(outData) = ER_ZTS_PERIOD;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyBufferFrameSize:
        *static_cast<UInt32*>(outData) = sIOBufferFrameSize;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioDevicePropertyBufferFrameSizeRange: {
        AudioValueRange* range = static_cast<AudioValueRange*>(outData);
        range->mMinimum = kMinBufferFrames;
        range->mMaximum = kMaxBufferFrames;
        *ioDataSize = sizeof(AudioValueRange);
        break;
    }
    default:
        err = kAudioHardwareUnknownPropertyError;
        break;
    }
    pthread_mutex_unlock(&sMutex);
    return err;
}

/**
 * @brief Write a settable device property.
 *
 * For the nominal sample rate this requests a device configuration change (applied
 * in ::ERP_PerformDeviceConfigurationChange); for the buffer size it updates state
 * and notifies the host. @}
 */
static OSStatus ERP_Device_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inObjectID, pid_t,
                                            const AudioObjectPropertyAddress* inAddress,
                                            UInt32, const void*,
                                            UInt32 inDataSize, const void* inData)
{
    if (inObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    switch (inAddress->mSelector) {
    case kAudioDevicePropertyNominalSampleRate: {
        if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 newRate = *static_cast<const Float64*>(inData);
        bool valid = false;
        for (UInt32 i = 0; i < kNumSampleRates; ++i)
            if (newRate == kSampleRates[i]) { valid = true; break; }
        if (!valid) return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&sMutex);
        bool changed = (newRate != sSampleRate);
        pthread_mutex_unlock(&sMutex);
        if (changed) {
            UInt64 action;
            memcpy(&action, &newRate, sizeof(action));
            sHost->RequestDeviceConfigurationChange(sHost, kObjectID_Device, action, nullptr);
        }
        return kAudioHardwareNoError;
    }
    case kAudioDevicePropertyBufferFrameSize: {
        if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        UInt32 newSize = *static_cast<const UInt32*>(inData);
        if (newSize < kMinBufferFrames || newSize > kMaxBufferFrames)
            return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&sMutex);
        sIOBufferFrameSize = newSize;   // ring is fixed-capacity; nothing to reallocate
        pthread_mutex_unlock(&sMutex);
        AudioObjectPropertyAddress addr = { kAudioDevicePropertyBufferFrameSize,
                                            kAudioObjectPropertyScopeGlobal, 0 };
        sHost->PropertiesChanged(sHost, kObjectID_Device, 1, &addr);
        return kAudioHardwareNoError;
    }
    default:
        return kAudioHardwareUnsupportedOperationError;
    }
}

// ---------------------------------------------------------------------------
// Stream-level properties
// ---------------------------------------------------------------------------

static AudioStreamBasicDescription StreamFormat(AudioObjectID streamID)
{
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate       = sSampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 32;
    UInt32 numCh = (streamID == kObjectID_Stream_Input) ? kNumInputChannels : kNumOutputChannels;
    fmt.mChannelsPerFrame = numCh;
    fmt.mBytesPerFrame    = numCh * sizeof(float);
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    return fmt;
}

/**
 * @name Stream-object properties
 * Handlers for the input/output stream objects: direction, format, available
 * formats, per-channel names, etc.
 * @{
 */

/** @brief Whether a stream object has a given property. */
static Boolean ERP_Stream_HasProperty(AudioServerPlugInDriverRef,
                                       AudioObjectID inObjectID, pid_t,
                                       const AudioObjectPropertyAddress* inAddress)
{
    if (inObjectID != kObjectID_Stream_Input && inObjectID != kObjectID_Stream_Output)
        return false;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        return true;
    case kAudioObjectPropertyElementName: {
        UInt32 el = inAddress->mElement;
        if (inObjectID == kObjectID_Stream_Input)  return el >= 1 && el <= kNumInputChannels;
        if (inObjectID == kObjectID_Stream_Output) return el >= 1 && el <= kNumOutputChannels;
        return false;
    }
    default:
        return false;
    }
}

/** @brief Whether a stream property is settable (the format is). */
static OSStatus ERP_Stream_IsPropertySettable(AudioServerPlugInDriverRef,
                                               AudioObjectID, pid_t,
                                               const AudioObjectPropertyAddress* inAddress,
                                               Boolean* outIsSettable)
{
    switch (inAddress->mSelector) {
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *outIsSettable = true; break;
    default:
        *outIsSettable = false; break;
    }
    return kAudioHardwareNoError;
}

/** @brief Byte size of a stream property's value. */
static OSStatus ERP_Stream_GetPropertyDataSize(AudioServerPlugInDriverRef,
                                                AudioObjectID inObjectID, pid_t,
                                                const AudioObjectPropertyAddress* inAddress,
                                                UInt32, const void*, UInt32* outDataSize)
{
    if (inObjectID != kObjectID_Stream_Input && inObjectID != kObjectID_Stream_Output)
        return kAudioHardwareBadObjectError;
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
        *outDataSize = sizeof(UInt32); break;
    case kAudioObjectPropertyOwnedObjects:
        *outDataSize = 0; break;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *outDataSize = sizeof(AudioStreamBasicDescription); break;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        *outDataSize = kNumSampleRates * sizeof(AudioStreamRangedDescription); break;
    case kAudioObjectPropertyElementName:
        *outDataSize = sizeof(CFStringRef); break;
    default: return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareNoError;
}

/** @brief Read a stream property (direction, format, available formats, names). */
static OSStatus ERP_Stream_GetPropertyData(AudioServerPlugInDriverRef,
                                            AudioObjectID inObjectID, pid_t,
                                            const AudioObjectPropertyAddress* inAddress,
                                            UInt32, const void*, UInt32 /*inDataSize*/,
                                            UInt32* ioDataSize, void* outData)
{
    if (inObjectID != kObjectID_Stream_Input && inObjectID != kObjectID_Stream_Output)
        return kAudioHardwareBadObjectError;
    OSStatus err = kAudioHardwareNoError;

    pthread_mutex_lock(&sMutex);
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:
        *static_cast<AudioClassID*>(outData) = kAudioStreamClassID;
        *ioDataSize = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyClass:
        *static_cast<AudioClassID*>(outData) = kAudioStreamClassID;
        *ioDataSize = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyOwner:
        *static_cast<AudioObjectID*>(outData) = kObjectID_Device;
        *ioDataSize = sizeof(AudioObjectID);
        break;
    case kAudioObjectPropertyElementName: {
        UInt32 el = inAddress->mElement;   // 1-based channel within this stream
        const char* nm = nullptr;
        if (inObjectID == kObjectID_Stream_Input && el >= 1 && el <= kNumInputChannels)
            nm = kInputChannelNames[el - 1];
        else if (inObjectID == kObjectID_Stream_Output && el >= 1 && el <= kNumOutputChannels)
            nm = kOutputChannelNames[el - 1];
        if (!nm) { err = kAudioHardwareUnknownPropertyError; break; }
        *static_cast<CFStringRef*>(outData) = CFStringCreateWithCString(nullptr, nm, kCFStringEncodingUTF8);
        *ioDataSize = sizeof(CFStringRef);
        break;
    }
    case kAudioObjectPropertyOwnedObjects:
        *ioDataSize = 0;
        break;
    case kAudioStreamPropertyIsActive:
        *static_cast<UInt32*>(outData) = 1;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioStreamPropertyDirection:
        *static_cast<UInt32*>(outData) =
            (inObjectID == kObjectID_Stream_Input) ? 1 : 0;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioStreamPropertyTerminalType:
        *static_cast<UInt32*>(outData) =
            (inObjectID == kObjectID_Stream_Input) ? kAudioStreamTerminalTypeLine
                                                    : kAudioStreamTerminalTypeLine;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioStreamPropertyStartingChannel:
        *static_cast<UInt32*>(outData) = 1;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioStreamPropertyLatency:
        *static_cast<UInt32*>(outData) = 0;
        *ioDataSize = sizeof(UInt32);
        break;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *static_cast<AudioStreamBasicDescription*>(outData) = StreamFormat(inObjectID);
        *ioDataSize = sizeof(AudioStreamBasicDescription);
        break;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription* descs = static_cast<AudioStreamRangedDescription*>(outData);
        UInt32 numCh = (inObjectID == kObjectID_Stream_Input) ? kNumInputChannels
                                                               : kNumOutputChannels;
        for (UInt32 i = 0; i < kNumSampleRates; ++i) {
            descs[i].mFormat.mSampleRate       = kSampleRates[i];
            descs[i].mFormat.mFormatID         = kAudioFormatLinearPCM;
            descs[i].mFormat.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            descs[i].mFormat.mBitsPerChannel   = 32;
            descs[i].mFormat.mChannelsPerFrame  = numCh;
            descs[i].mFormat.mBytesPerFrame     = numCh * sizeof(float);
            descs[i].mFormat.mFramesPerPacket   = 1;
            descs[i].mFormat.mBytesPerPacket    = descs[i].mFormat.mBytesPerFrame;
            descs[i].mSampleRateRange.mMinimum  = kSampleRates[i];
            descs[i].mSampleRateRange.mMaximum  = kSampleRates[i];
        }
        *ioDataSize = kNumSampleRates * sizeof(AudioStreamRangedDescription);
        break;
    }
    default:
        err = kAudioHardwareUnknownPropertyError;
        break;
    }
    pthread_mutex_unlock(&sMutex);
    return err;
}

/** @brief Write a stream property (accepts a matching format). @} */
static OSStatus ERP_Stream_SetPropertyData(AudioServerPlugInDriverRef,
                                            AudioObjectID inObjectID, pid_t,
                                            const AudioObjectPropertyAddress* inAddress,
                                            UInt32, const void*,
                                            UInt32 inDataSize, const void* inData)
{
    if (inObjectID != kObjectID_Stream_Input && inObjectID != kObjectID_Stream_Output)
        return kAudioHardwareBadObjectError;
    switch (inAddress->mSelector) {
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (inDataSize < sizeof(AudioStreamBasicDescription))
            return kAudioHardwareBadPropertySizeError;
        const auto* fmt = static_cast<const AudioStreamBasicDescription*>(inData);
        if (fmt->mFormatID != kAudioFormatLinearPCM) return kAudioDeviceUnsupportedFormatError;
        bool validRate = false;
        for (UInt32 i = 0; i < kNumSampleRates; ++i)
            if (fmt->mSampleRate == kSampleRates[i]) { validRate = true; break; }
        if (!validRate) return kAudioDeviceUnsupportedFormatError;
        if (fmt->mSampleRate != sSampleRate) {
            UInt64 action;
            Float64 rate = fmt->mSampleRate;
            memcpy(&action, &rate, sizeof(action));
            sHost->RequestDeviceConfigurationChange(sHost, kObjectID_Device, action, nullptr);
        }
        return kAudioHardwareNoError;
    }
    case kAudioStreamPropertyIsActive:
        return kAudioHardwareNoError;   // streams always active
    default:
        return kAudioHardwareUnsupportedOperationError;
    }
}

/**
 * @name Control-object properties
 * Stubs — the device exposes no control objects (volume/mute/etc.). Present so the
 * routers can compile; all return "no such object".
 * @{
 */

/** @brief Control @c HasProperty stub (no controls). */
static Boolean  ERP_Control_HasProperty(AudioServerPlugInDriverRef, AudioObjectID,
                                         pid_t, const AudioObjectPropertyAddress*)
{ return false; }

/** @brief Control @c IsPropertySettable stub (no controls). */
static OSStatus ERP_Control_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID,
                                                pid_t, const AudioObjectPropertyAddress*,
                                                Boolean* outIsSettable)
{ *outIsSettable = false; return kAudioHardwareBadObjectError; }

/** @brief Control @c GetPropertyDataSize stub (no controls). */
static OSStatus ERP_Control_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID,
                                                 pid_t, const AudioObjectPropertyAddress*,
                                                 UInt32, const void*, UInt32* outDataSize)
{ *outDataSize = 0; return kAudioHardwareBadObjectError; }

/** @brief Control @c GetPropertyData stub (no controls). */
static OSStatus ERP_Control_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID,
                                             pid_t, const AudioObjectPropertyAddress*,
                                             UInt32, const void*, UInt32, UInt32* ioDataSize, void*)
{ *ioDataSize = 0; return kAudioHardwareBadObjectError; }

/** @brief Control @c SetPropertyData stub (no controls). @} */
static OSStatus ERP_Control_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID,
                                             pid_t, const AudioObjectPropertyAddress*,
                                             UInt32, const void*, UInt32, const void*)
{ return kAudioHardwareBadObjectError; }

/**
 * @name IO operations
 * The audio IO cycle: start/stop, timeline anchoring, and per-cycle data movement.
 * @{
 */

/**
 * @brief Begin IO for a client: on the first client, reset timing, attach the ring,
 *        and signal the engine.
 */
static OSStatus ERP_StartIO(AudioServerPlugInDriverRef,
                             AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&sMutex);
    if (sIORunningCount == 0) {
        sIORunning = true;
        // Reset the zero-timestamp state (BlackHole StartIO init).
        RecomputeHostTicksPerFrame();
        pthread_mutex_lock(&sIOTimeMutex);
        sNumTimeStamps    = 0;
        sAnchorSampleTime = 0;
        sAnchorHostTime   = mach_absolute_time();
        sPrevTicks        = 0.0;
        sAnchorSeed       = 1;
        pthread_mutex_unlock(&sIOTimeMutex);
        pthread_mutex_unlock(&sMutex);
        RingStartStreaming();
        pthread_mutex_lock(&sMutex);
    }
    ++sIORunningCount;
    pthread_mutex_unlock(&sMutex);

    os_log(sLog, "ElevenRack: StartIO (clients=%u)", sIORunningCount);
    return kAudioHardwareNoError;
}

/** @brief End IO for a client; on the last client, detach the ring and clear the hint. */
static OSStatus ERP_StopIO(AudioServerPlugInDriverRef,
                            AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&sMutex);
    if (sIORunningCount > 0) {
        --sIORunningCount;
        if (sIORunningCount == 0) {
            sIORunning = false;
            pthread_mutex_unlock(&sMutex);
            RingStopStreaming();
            DetachRing();
            pthread_mutex_lock(&sMutex);
        }
    }
    pthread_mutex_unlock(&sMutex);

    os_log(sLog, "ElevenRack: StopIO (clients=%u)", sIORunningCount);
    return kAudioHardwareNoError;
}

/**
 * @brief Report the device timeline: return the current anchor and advance it by
 *        one buffer period. Core Audio uses this to schedule the IO cycle.
 * @param[out] outSampleTime  Anchor sample time.
 * @param[out] outHostTime    Anchor host time (mach ticks).
 * @param[out] outSeed        Timeline seed (changes signal a discontinuity).
 */
static OSStatus ERP_GetZeroTimeStamp(AudioServerPlugInDriverRef,
                                      AudioObjectID inDeviceObjectID, UInt32,
                                      Float64* outSampleTime,
                                      UInt64*  outHostTime,
                                      UInt64*  outSeed)
{
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    // The device timeline is a series of (sampleTime, hostTime) anchors spaced
    // ER_ZTS_PERIOD sample frames apart (the canonical ring-buffer model). Sample
    // time advances by ER_ZTS_PERIOD each time a full ring-period of host ticks has
    // elapsed from the StartIO anchor; host time advances by the matching ticks.
    // (Faithful to BlackHole_GetZeroTimeStamp.)
    pthread_mutex_lock(&sIOTimeMutex);
    UInt64 now = mach_absolute_time();
    double ticksPerPeriod = sHostTicksPerFrame * (double)ER_ZTS_PERIOD;
    double nextTickOffset  = sPrevTicks + ticksPerPeriod;
    UInt64 nextHostTime    = sAnchorHostTime + (UInt64)nextTickOffset;
    if (nextHostTime <= now) {
        ++sNumTimeStamps;
        sPrevTicks = nextTickOffset;
    }
    *outSampleTime = (Float64)(sNumTimeStamps * (UInt64)ER_ZTS_PERIOD);
    *outHostTime   = sAnchorHostTime + (UInt64)sPrevTicks;
    *outSeed       = sAnchorSeed;
    pthread_mutex_unlock(&sIOTimeMutex);
    return kAudioHardwareNoError;
}

/** @brief Declare which IO operations this plugin handles (read-input, write-mix). */
static OSStatus ERP_WillDoIOOperation(AudioServerPlugInDriverRef,
                                       AudioObjectID inDeviceObjectID, UInt32,
                                       UInt32 inOperationID,
                                       Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    switch (inOperationID) {
    case kAudioServerPlugInIOOperationReadInput:
    case kAudioServerPlugInIOOperationWriteMix:
        *outWillDo        = true;
        *outWillDoInPlace = false;
        break;
    default:
        *outWillDo        = false;
        *outWillDoInPlace = false;
        break;
    }
    return kAudioHardwareNoError;
}

/** @brief Called before an IO phase; nothing to prepare. */
static OSStatus ERP_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32,
                                      UInt32, UInt32,
                                      const AudioServerPlugInIOCycleInfo*)
{
    return kAudioHardwareNoError;
}

/**
 * @brief Move one IO cycle's audio between Core Audio and the ring.
 *
 * For the input stream, reads capture frames from the input ring into @p ioMainBuffer
 * (zero-filled first, so underruns are silence). For the output stream, writes
 * @p ioMainBuffer into the output ring for the engine to send to the device.
 *
 * @param inDeviceObjectID    Must be ::kObjectID_Device.
 * @param inStreamObjectID    ::kObjectID_Stream_Input or ::kObjectID_Stream_Output.
 * @param inIOBufferFrameSize Frames in this cycle.
 * @param ioMainBuffer        Interleaved float32 buffer for the stream.
 */
static OSStatus ERP_DoIOOperation(AudioServerPlugInDriverRef,
                                   AudioObjectID inDeviceObjectID,
                                   AudioObjectID inStreamObjectID,
                                   UInt32 inClientID, UInt32 /*inOperationID*/,
                                   UInt32 inIOBufferFrameSize,
                                   const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                                   void* ioMainBuffer, void* /*ioSecondaryBuffer*/)
{
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (!ioMainBuffer) return kAudioHardwareNoError;

    UInt32 numCh  = (inStreamObjectID == kObjectID_Stream_Input) ? kNumInputChannels
                                                                  : kNumOutputChannels;
    // Compute in size_t so the multiplication can't overflow a 32-bit intermediate
    // before it becomes the byte count.
    size_t bytes  = (size_t)inIOBufferFrameSize * numCh * sizeof(float);

    if (!sRing) TryAttachRing();   // app may have started after StartIO

    if (inStreamObjectID == kObjectID_Stream_Input) {
        // Capture: pull frames the app's USB engine wrote into the input ring.
        // Zero-fill first so any underrun (or a not-yet-running app) yields silence
        // rather than stale data.
        memset(ioMainBuffer, 0, bytes);
        if (sRing && numCh == ER_IN_CH)
            er_in_read(sRing, static_cast<float*>(ioMainBuffer), inIOBufferFrameSize);
    } else if (inStreamObjectID == kObjectID_Stream_Output) {
        // Playback: overwrite CoreAudio's frames into the time-addressed output ring
        // at this cycle's absolute output sample time. Per-client WriteMix calls
        // thus land at their correct positions instead of being appended in call
        // order (which scrambled interleaved clients — the multi-client bug).
        if (sRing && numCh == ER_OUT_CH && inIOCycleInfo)
            er_out_write_at(sRing, (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime,
                            static_cast<const float*>(ioMainBuffer), inIOBufferFrameSize);
    }
    return kAudioHardwareNoError;
}

/** @brief Called after an IO phase; nothing to finalize. @} */
static OSStatus ERP_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32,
                                    UInt32, UInt32,
                                    const AudioServerPlugInIOCycleInfo*)
{
    return kAudioHardwareNoError;
}

/**
 * @brief CFPlugIn factory entry point named in Info.plist's @c CFPlugInFactories.
 *
 * @c coreaudiod calls this to obtain the driver interface. Returns the single
 * ::sPlugIn instance (ref-counted) when asked for @c kAudioServerPlugInTypeUUID.
 *
 * @param requestedTypeUUID  The requested plugin type UUID.
 * @return The plugin instance, or @c nullptr if the type does not match.
 */
extern "C" void* ElevenRackAudioPluginCreate(CFAllocatorRef, CFUUIDRef requestedTypeUUID);

extern "C" void* ElevenRackAudioPluginCreate(CFAllocatorRef, CFUUIDRef requestedTypeUUID)
{
    if (!CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    ERP_AddRef(&sPlugIn);
    return &sPlugIn;
}
