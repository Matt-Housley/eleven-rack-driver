/**
 * @file erstream.c
 * @brief Diagnostic: sustained user-space isochronous READ test.
 *
 * Confirms that isochronous USB audio can be sustained from user space
 * (IOUSBLib) with no kext/dext. Reads the capture pipe (interface 4, alt 1, isoc
 * IN) for a few seconds with several transfers in flight, and reports:
 *   - sustained frame/byte throughput and implied sample rate
 *   - the real per-frame byte count (the descriptor only gives maxPacket)
 *   - isoc frame error counts (underruns/overruns)
 *   - whether live, non-zero audio samples are arriving
 *
 * Read-only: never writes the playback pipe, so nothing comes out of the
 * amp/monitors. Safe to run anytime the Rack is connected. Plug a guitar in and
 * play to see non-zero sample levels.
 *
 * Build:
 * @code
 *   clang -o erstream erstream.c \
 *       -framework IOKit -framework CoreFoundation -Wno-deprecated-declarations
 * @endcode
 * Run: @c ./erstream @c [seconds] (default 3).
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <libkern/OSByteOrder.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define ER_VID          0x0DBA
#define ER_PID          0xB011
#define ER_CONFIG_VALUE 1        // corrected: audio lives in config value 1
#define ER_IF_IN        4        // capture interface
#define ER_ALT_ACTIVE   1
#define ER_EP_IN        0x83     // isoc IN endpoint address (informational)

// Transfer geometry: a small pool of in-flight isoc reads keeps the pipe fed.
#define N_REQ           8        // transfers in flight
#define FRAMES_PER_REQ  16       // isoc frames per transfer (1 frame ≈ 1 ms full-speed)
#define MAX_PKT         512      // >= descriptor maxPacket (416); per-frame request size

typedef struct {
    int              index;
    IOUSBIsocFrame   frames[FRAMES_PER_REQ];
    UInt8           *data;       // FRAMES_PER_REQ * MAX_PKT
} Req;

static IOUSBInterfaceInterface500 **gIntf = NULL;
static UInt8   gPipeRef   = 0;
static UInt16  gReqLen    = MAX_PKT;   // set to the pipe's real maxPacket
static UInt64  gNextFrame = 0;
static Req     gReqs[N_REQ];
static int     gCallbacks = 0;

// Stats (touched only on the run-loop thread)
static unsigned long long gTotalBytes  = 0;
static unsigned long long gTotalFrames = 0;
static unsigned long long gErrFrames   = 0;
static unsigned long long gZeroFrames  = 0;
static int               gMinFrameLen  = 1 << 30;
static int               gMaxFrameLen  = 0;
static long              gPeakSample   = 0;
static bool              gStop         = false;

/** @brief Open the USB device interface for a matched IOKit service. */
static IOUSBDeviceInterface500  **openDevice(io_service_t s) {
    IOCFPlugInInterface **plugin = NULL; SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID,
            kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS || !plugin) return NULL;
    IOUSBDeviceInterface500 **dev = NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
    (*plugin)->Release(plugin);
    return dev;
}

/** @brief Find and open the interface whose number equals @p wantIf. */
static IOUSBInterfaceInterface500 **openInterface(IOUSBDeviceInterface500 **dev, UInt8 wantIf) {
    IOUSBFindInterfaceRequest req = {
        kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t iter = 0;
    if ((*dev)->CreateInterfaceIterator(dev, &req, &iter) != kIOReturnSuccess) return NULL;
    io_service_t s; IOUSBInterfaceInterface500 **found = NULL;
    while ((s = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plugin = NULL; SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS && plugin) {
            IOUSBInterfaceInterface500 **intf = NULL;
            (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
            (*plugin)->Release(plugin);
            UInt8 n = 0xFF;
            if (intf) (*intf)->GetInterfaceNumber(intf, &n);
            if (intf && n == wantIf) { found = intf; IOObjectRelease(s); break; }
            if (intf) (*intf)->Release(intf);
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(iter);
    return found;
}

/**
 * @brief Track the peak magnitude of a frame read as signed 24-bit LE samples.
 *
 * Interprets the payload as signed 24-bit little-endian samples and records the
 * largest magnitude seen, which tells whether live audio is arriving.
 *
 * @param p    Pointer to the frame's bytes.
 * @param len  Byte count in this frame.
 */
static void scanLevels(const UInt8 *p, int len) {
    for (int i = 0; i + 3 <= len; i += 3) {
        long v = (p[i]) | (p[i+1] << 8) | (p[i+2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFFL;   // sign-extend 24->long
        long mag = v < 0 ? -v : v;
        if (mag > gPeakSample) gPeakSample = mag;
    }
}

static void armRequest(Req *r);

/** @brief Isoc completion: tally throughput/errors, scan levels, then re-arm. */
static void isocComplete(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    Req *r = (Req *)refcon;
    if (gCallbacks < 3) {
        printf("  [cb %d] req=%d result=0x%x frame0: status=0x%x act=%u\n",
               gCallbacks, r->index, result, r->frames[0].frStatus, r->frames[0].frActCount);
    }
    gCallbacks++;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        // A hard error (device unplugged, aborted) — stop the loop.
        gStop = true;
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    for (int f = 0; f < FRAMES_PER_REQ; f++) {
        IOUSBIsocFrame *fr = &r->frames[f];
        int len = fr->frActCount;
        gTotalFrames++;
        gTotalBytes += len;
        // kIOReturnUnderrun on an isoc IN frame = normal short packet (the device
        // sent fewer bytes than we offered). Only OTHER non-success statuses are
        // real errors.
        if (fr->frStatus != kIOReturnSuccess && fr->frStatus != kIOReturnUnderrun)
            gErrFrames++;
        if (len == 0) { gZeroFrames++; continue; }
        if (len < gMinFrameLen) gMinFrameLen = len;
        if (len > gMaxFrameLen) gMaxFrameLen = len;
        scanLevels(r->data + f * MAX_PKT, len);
    }
    if (!gStop) armRequest(r);
}

/** @brief Submit one asynchronous isoc read request of @c FRAMES_PER_REQ frames. */
static void armRequest(Req *r) {
    for (int f = 0; f < FRAMES_PER_REQ; f++) {
        r->frames[f].frReqCount = gReqLen;   // == pipe maxPacket
        r->frames[f].frActCount = 0;
        r->frames[f].frStatus   = 0;
    }
    IOReturn ret = (*gIntf)->ReadIsochPipeAsync(
        gIntf, gPipeRef, r->data, gNextFrame, FRAMES_PER_REQ,
        r->frames, isocComplete, r);
    if (ret != kIOReturnSuccess) {
        printf("  ReadIsochPipeAsync(frame %llu) failed: 0x%x\n", gNextFrame, ret);
        gStop = true;
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    gNextFrame += FRAMES_PER_REQ;
}

/** @brief Timer callback: stop capturing and exit the run loop. */
static void stopTimer(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    gStop = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

/** @brief Stream the IN pipe for a few seconds, then report throughput and levels. */
int main(int argc, char **argv) {
    double seconds = (argc > 1) ? atof(argv[1]) : 3.0;
    printf("Eleven Rack — sustained isochronous READ test (%.1fs)\n", seconds);
    printf("=====================================================\n");

    SInt32 vid = ER_VID, pid = ER_PID;
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
    CFRelease(v); CFRelease(p);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) { printf("  device not found — plug in the Eleven Rack.\n"); return 1; }

    IOUSBDeviceInterface500 **dev = openDevice(svc);
    IOObjectRelease(svc);
    if (!dev) { printf("  could not build device interface.\n"); return 2; }

    IOReturn r = (*dev)->USBDeviceOpen(dev);
    if (r == kIOReturnExclusiveAccess) r = (*dev)->USBDeviceOpenSeize(dev);
    if (r != kIOReturnSuccess) { printf("  open failed: 0x%x\n", r); return 3; }
    (*dev)->SetConfiguration(dev, ER_CONFIG_VALUE);

    gIntf = openInterface(dev, ER_IF_IN);
    if (!gIntf) { printf("  capture interface %d not found.\n", ER_IF_IN); return 4; }
    if ((*gIntf)->USBInterfaceOpen(gIntf) != kIOReturnSuccess) { printf("  interface open failed.\n"); return 5; }
    if ((*gIntf)->SetAlternateInterface(gIntf, ER_ALT_ACTIVE) != kIOReturnSuccess) { printf("  alt-setting failed.\n"); return 6; }

    // Locate the isoc IN pipe index and confirm its geometry.
    UInt8 nEnd = 0; (*gIntf)->GetNumEndpoints(gIntf, &nEnd);
    for (UInt8 pipe = 1; pipe <= nEnd; pipe++) {
        UInt8 dir=0,num=0,tt=0,intv=0; UInt16 mp=0;
        if ((*gIntf)->GetPipeProperties(gIntf, pipe, &dir, &num, &tt, &mp, &intv) == kIOReturnSuccess
            && tt == kUSBIsoc && dir == kUSBIn) {
            gPipeRef = pipe;
            gReqLen  = mp ? mp : MAX_PKT;
            printf("  isoc IN pipe %u: ep num=%u maxPacket=%u interval=%u\n", pipe, num, mp, intv);
        }
    }
    if (!gPipeRef) { printf("  no isoc IN pipe.\n"); return 7; }

    // Allocate the transfer pool.
    for (int i = 0; i < N_REQ; i++) {
        gReqs[i].index = i;
        gReqs[i].data  = malloc(FRAMES_PER_REQ * MAX_PKT);
        memset(gReqs[i].data, 0, FRAMES_PER_REQ * MAX_PKT);
    }

    // Wire the interface's async completions into this thread's run loop.
    CFRunLoopSourceRef src = NULL;
    if ((*gIntf)->CreateInterfaceAsyncEventSource(gIntf, &src) != kIOReturnSuccess || !src) {
        printf("  CreateInterfaceAsyncEventSource failed.\n"); return 8;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    // Schedule the first reads a few frames in the future.
    UInt64 busFrame = 0; AbsoluteTime at;
    IOReturn bf = (*gIntf)->GetBusFrameNumber(gIntf, &busFrame, &at);
    printf("  GetBusFrameNumber: ret=0x%x frame=%llu\n", bf, busFrame);
    gNextFrame = busFrame + 25;   // schedule comfortably in the future
    printf("  scheduling %d transfers x %d frames, reqLen=%u, startFrame=%llu\n",
           N_REQ, FRAMES_PER_REQ, gReqLen, gNextFrame);
    for (int i = 0; i < N_REQ; i++) armRequest(&gReqs[i]);
    printf("  entering run loop...\n");

    // Run for the requested duration.
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL,
        CFAbsoluteTimeGetCurrent() + seconds, 0, 0, 0, stopTimer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    CFRunLoopRun();

    // Teardown.
    (*gIntf)->SetAlternateInterface(gIntf, 0);
    (*gIntf)->USBInterfaceClose(gIntf);
    (*gIntf)->Release(gIntf);
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);

    // Report.
    double avgLen = gTotalFrames ? (double)gTotalBytes / gTotalFrames : 0;
    double bytesPerSec = gTotalBytes / seconds;
    double impliedRate6 = bytesPerSec / 6.0;   // if 2ch x 24-bit = 6 bytes/frame-sample
    printf("\n── results ──\n");
    printf("  frames serviced : %llu  (%.0f/sec)\n", gTotalFrames, gTotalFrames / seconds);
    printf("  bytes received  : %llu  (%.0f KB/s)\n", gTotalBytes, bytesPerSec / 1024.0);
    printf("  frame payload   : min=%d max=%d avg=%.1f bytes\n",
           gMinFrameLen == (1<<30) ? 0 : gMinFrameLen, gMaxFrameLen, avgLen);
    printf("  empty frames    : %llu\n", gZeroFrames);
    printf("  error frames    : %llu\n", gErrFrames);
    printf("  implied rate    : %.0f Hz  (assuming 2ch × 24-bit)\n", impliedRate6);
    printf("  peak sample mag : %ld / %d  (%.1f%%)  %s\n",
           gPeakSample, 0x7FFFFF, 100.0 * gPeakSample / (double)0x7FFFFF,
           gPeakSample > 0 ? "← live audio detected" : "(silent — play a note to test)");

    bool sustained = gTotalFrames > 0 && gErrFrames < gTotalFrames / 10;
    printf("\n  %s sustained user-space isochronous capture %s\n",
           sustained ? "[ok]" : "[XX]",
           sustained ? "WORKS — no driver required." : "did NOT sustain cleanly.");
    return sustained ? 0 : 9;
}
