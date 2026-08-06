/**
 * @file erprobe.c
 * @brief Diagnostic: user-space USB + MIDI probe for the Eleven Rack.
 *
 * Confirms that the Eleven Rack can be claimed and configured from user space
 * via IOUSBLib — with no kext, no DriverKit system extension, and no "Reduced
 * Security" toggle. If this works, the "no driver install" architecture is
 * viable.
 *
 * What it does:
 *   1. Matches the device by VID/PID on the USB bus.
 *   2. Reports whether another driver (e.g. AppleUSBAudio) currently owns it.
 *   3. Opens the device (plain open, then Seize if arbitration blocks it).
 *   4. Selects the vendor-specific audio configuration.
 *   5. Enumerates interfaces, locates the audio-streaming interfaces
 *      (3 = OUT, 4 = IN) and their isochronous endpoints, and prints max packet
 *      sizes.
 *   6. Creates a CoreMIDI virtual source + destination as a smoke test (needs no
 *      hardware and always runs).
 *
 * It does not stream audio — that is handled by the engine. This tool is
 * read-mostly and safe to run repeatedly.
 *
 * Build:
 * @code
 *   clang -o erprobe erprobe.c \
 *       -framework IOKit -framework CoreFoundation -framework CoreMIDI \
 *       -Wno-deprecated-declarations
 * @endcode
 * Run (plug the Eleven Rack in first for the USB half): @c ./erprobe
 *
 * IOUSBLib's device/interface plug-in interfaces are formally deprecated but are
 * the only user-space path to isochronous USB on macOS, and remain fully
 * functional on Apple Silicon. The deprecation warnings are silenced.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach.h>
#include <libkern/OSByteOrder.h>
#include <stdio.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Eleven Rack constants
// ---------------------------------------------------------------------------
#define ER_VID              0x0DBA
#define ER_PID              0xB011
#define ER_AUDIO_CONFIG     2       // vendor-specific config that enables audio
#define ER_IF_OUT           3       // playback OUT interface number
#define ER_IF_IN            4       // capture  IN  interface number
#define ER_ALT_ACTIVE       1       // alt-setting 1 = active isochronous bandwidth

// ANSI helpers for readable output
#define OK   "\033[32m[ok]\033[0m"
#define WARN "\033[33m[--]\033[0m"
#define BAD  "\033[31m[XX]\033[0m"

/** @brief Map an IOReturn code to a short human-readable description. */
static const char *usbErr(IOReturn r) {
    switch (r) {
        case kIOReturnSuccess:        return "success";
        case kIOReturnExclusiveAccess:return "exclusive access (another driver owns it)";
        case kIOReturnNotOpen:        return "not open";
        case kIOReturnNoDevice:       return "no device";
        case kIOReturnNotResponding:  return "not responding";
        case kIOReturnNotPermitted:   return "not permitted";
        case kIOReturnBadArgument:    return "bad argument";
        default:                      return "see IOReturn code";
    }
}

/** @brief CoreMIDI read callback: print the size and first byte of each packet. */
static void midiReadProc(const MIDIPacketList *pktlist, void *refCon, void *srcConn) {
    (void)refCon; (void)srcConn;
    const MIDIPacket *p = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; i++) {
        printf("      MIDI in: %u bytes, first=0x%02x\n", p->length,
               p->length ? p->data[0] : 0);
        p = MIDIPacketNext(p);
    }
}

/**
 * @brief Create system-wide CoreMIDI virtual source + destination as a smoke test.
 *
 * Uses only user-space CoreMIDI (no driver). If the endpoints show up in Audio
 * MIDI Setup while the probe runs, the MIDI half of the design is proven. The
 * endpoints are torn down when the probe exits.
 */
static void midiSmokeTest(void) {
    printf("\n── CoreMIDI virtual endpoints (no hardware needed) ──\n");
    MIDIClientRef client = 0;
    OSStatus s = MIDIClientCreate(CFSTR("Eleven Rack"), NULL, NULL, &client);
    if (s != noErr) { printf("  %s MIDIClientCreate failed: %d\n", BAD, (int)s); return; }

    MIDIEndpointRef src = 0, dst = 0;
    s = MIDISourceCreate(client, CFSTR("Eleven Rack MIDI Out"), &src);
    printf("  %s virtual SOURCE  'Eleven Rack MIDI Out'  (status %d)\n",
           s == noErr ? OK : BAD, (int)s);
    s = MIDIDestinationCreate(client, CFSTR("Eleven Rack MIDI In"), midiReadProc, NULL, &dst);
    printf("  %s virtual DEST    'Eleven Rack MIDI In'   (status %d)\n",
           s == noErr ? OK : BAD, (int)s);

    printf("  → These appear in Audio MIDI Setup > MIDI Studio while this runs.\n");
    printf("  → (endpoints are torn down when the probe exits)\n");
}

/** @brief Match the Eleven Rack IOUSBDevice service by VID/PID. */
static io_service_t findDevice(void) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return IO_OBJECT_NULL;

    SInt32 vid = ER_VID, pid = ER_PID;
    CFNumberRef vidRef = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef pidRef = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID),  vidRef);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), pidRef);
    CFRelease(vidRef);
    CFRelease(pidRef);

    // IOServiceGetMatchingService consumes a reference on `match`.
    return IOServiceGetMatchingService(kIOMainPortDefault, match);
}

/**
 * @brief Report which kernel driver, if any, has already matched on the device.
 *
 * This is the arbitration question that decides whether user-space claiming is
 * clean: any child that is not an interface nub is a driver that has claimed the
 * device.
 */
static void reportOwningDriver(io_service_t dev) {
    io_iterator_t children = IO_OBJECT_NULL;
    kern_return_t kr = IORegistryEntryGetChildIterator(dev, kIOServicePlane, &children);
    if (kr != KERN_SUCCESS) { printf("  %s could not read device children\n", WARN); return; }

    io_object_t child;
    bool found = false;
    while ((child = IOIteratorNext(children))) {
        io_name_t name = {0};
        if (IORegistryEntryGetName(child, name) == KERN_SUCCESS) {
            // The interface nubs are named IOUSBHostInterface / IOUSBInterface;
            // anything else attached here is a driver that has claimed the device.
            if (strstr(name, "Interface") == NULL) {
                printf("  %s attached driver: %s\n", WARN, name);
                found = true;
            }
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(children);
    if (!found)
        printf("  %s no vendor/class driver attached at device level — good sign\n", OK);
}

/**
 * @brief Dump every configuration descriptor the device currently exposes.
 *
 * Reports the real configuration values and which config (if any) carries the
 * audio-streaming interfaces with isochronous endpoints, rather than assuming a
 * fixed value.
 *
 * @param dev      The opened device interface.
 * @param nConfig  Number of configuration descriptors to examine.
 * @return The @c bConfigurationValue of the config that contains isochronous
 *         endpoints (the audio config), or 0 if none is currently exposed.
 */
static UInt8 dumpConfigurations(IOUSBDeviceInterface500 **dev, UInt8 nConfig) {
    UInt8 audioConfigValue = 0;
    for (UInt8 idx = 0; idx < nConfig; idx++) {
        IOUSBConfigurationDescriptorPtr cfg = NULL;
        IOReturn r = (*dev)->GetConfigurationDescriptorPtr(dev, idx, &cfg);
        if (r != kIOReturnSuccess || !cfg) {
            printf("  config index %u: could not read (0x%x)\n", idx, r);
            continue;
        }
        UInt16 total = OSSwapLittleToHostInt16(cfg->wTotalLength);
        printf("  config index %u: value=%u  interfaces=%u  totalLen=%u\n",
               idx, cfg->bConfigurationValue, cfg->bNumInterfaces, total);

        const UInt8 *p   = (const UInt8 *)cfg;
        const UInt8 *end = p + total;
        bool cfgHasIsoc = false;
        int curIf = -1, curAlt = -1;
        while (p + 2 <= end) {
            UInt8 len = p[0], type = p[1];
            if (len < 2 || p + len > end) break;
            if (type == kUSBInterfaceDesc) {           // 0x04
                curIf  = p[2];   // bInterfaceNumber
                curAlt = p[3];   // bAlternateSetting
                UInt8 cls = p[5], sub = p[6];
                printf("      iface %d alt %d  class=0x%02x sub=0x%02x endpoints=%u\n",
                       curIf, curAlt, cls, sub, p[4]);
            } else if (type == kUSBEndpointDesc) {     // 0x05
                UInt8  epAddr = p[2];
                UInt8  attr   = p[3];
                UInt16 maxPkt = OSSwapLittleToHostInt16(*(const UInt16 *)(p + 4));
                UInt8  interval = p[6];
                const char *tt = (attr & 0x03) == 0 ? "control"
                               : (attr & 0x03) == 1 ? "ISOC"
                               : (attr & 0x03) == 2 ? "bulk" : "interrupt";
                printf("          ep 0x%02x %s %-9s maxPkt=%u interval=%u\n",
                       epAddr, (epAddr & 0x80) ? "IN " : "OUT", tt, maxPkt, interval);
                if ((attr & 0x03) == 1) cfgHasIsoc = true;
            }
            p += len;
        }
        if (cfgHasIsoc && audioConfigValue == 0)
            audioConfigValue = cfg->bConfigurationValue;
    }
    return audioConfigValue;
}

/** @brief Build an IOUSBDeviceInterface from a matched service. */
static IOUSBDeviceInterface500 **openDeviceInterface(io_service_t service) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        printf("  %s IOCreatePlugInInterfaceForService failed: 0x%x\n", BAD, kr);
        return NULL;
    }
    IOUSBDeviceInterface500 **dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID *)&dev);
    (*plugin)->Release(plugin);
    if (hr != S_OK || !dev) {
        printf("  %s QueryInterface(device) failed: 0x%lx\n", BAD, (long)hr);
        return NULL;
    }
    return dev;
}

/**
 * @brief Walk the interfaces of the configured device and report the audio ones.
 *
 * For each audio interface, opens it, selects the active alt-setting (which turns
 * on isochronous bandwidth), reports each pipe's geometry, then parks it back at
 * the idle alt-setting.
 */
static void enumerateInterfaces(IOUSBDeviceInterface500 **dev) {
    IOUSBFindInterfaceRequest req = {
        .bInterfaceClass    = kIOUSBFindInterfaceDontCare,
        .bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
        .bAlternateSetting  = kIOUSBFindInterfaceDontCare,
    };
    io_iterator_t iter = IO_OBJECT_NULL;
    IOReturn r = (*dev)->CreateInterfaceIterator(dev, &req, &iter);
    if (r != kIOReturnSuccess) {
        printf("  %s CreateInterfaceIterator: 0x%x (%s)\n", BAD, r, usbErr(r));
        return;
    }

    io_service_t usbIf;
    while ((usbIf = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(usbIf, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS && plugin) {
            IOUSBInterfaceInterface500 **intf = NULL;
            if ((*plugin)->QueryInterface(plugin,
                    CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),
                    (LPVOID *)&intf) == S_OK && intf) {

                UInt8 ifNum = 0xFF, cls = 0, sub = 0, nEnd = 0;
                (*intf)->GetInterfaceNumber(intf, &ifNum);
                (*intf)->GetInterfaceClass(intf, &cls);
                (*intf)->GetInterfaceSubClass(intf, &sub);

                bool isAudio = (ifNum == ER_IF_OUT || ifNum == ER_IF_IN);
                const char *tag = (ifNum == ER_IF_OUT) ? " [audio OUT]"
                                : (ifNum == ER_IF_IN)  ? " [audio IN]"  : "";
                printf("  • interface %u  class=0x%02x sub=0x%02x%s\n",
                       ifNum, cls, sub, tag);

                if (isAudio) {
                    IOReturn o = (*intf)->USBInterfaceOpen(intf);
                    if (o != kIOReturnSuccess) {
                        printf("      %s open: 0x%x (%s)\n", WARN, o, usbErr(o));
                    } else {
                        // Selecting the active alt-setting is what turns on isoc
                        // bandwidth; report endpoint pipe geometry for streaming.
                        IOReturn a = (*intf)->SetAlternateInterface(intf, ER_ALT_ACTIVE);
                        printf("      %s SetAlternateInterface(%d): 0x%x (%s)\n",
                               a == kIOReturnSuccess ? OK : WARN,
                               ER_ALT_ACTIVE, a, usbErr(a));
                        (*intf)->GetNumEndpoints(intf, &nEnd);
                        for (UInt8 pipe = 1; pipe <= nEnd; pipe++) {
                            UInt8 dir=0, num=0, tt=0, interval=0;
                            UInt16 maxPkt=0;
                            IOReturn pr = (*intf)->GetPipeProperties(
                                intf, pipe, &dir, &num, &tt, &maxPkt, &interval);
                            if (pr == kIOReturnSuccess) {
                                const char *ttName = tt==kUSBIsoc ? "isoc"
                                                   : tt==kUSBBulk ? "bulk"
                                                   : tt==kUSBInterrupt ? "intr" : "ctrl";
                                printf("      %s pipe %u: %s %s maxPacket=%u interval=%u\n",
                                       tt==kUSBIsoc ? OK : "  ", pipe,
                                       dir==kUSBIn ? "IN " : "OUT", ttName, maxPkt, interval);
                            }
                        }
                        // Park back at idle; this tool does not stream audio.
                        (*intf)->SetAlternateInterface(intf, 0);
                        (*intf)->USBInterfaceClose(intf);
                    }
                }
                (*intf)->Release(intf);
            }
            (*plugin)->Release(plugin);
        }
        IOObjectRelease(usbIf);
    }
    IOObjectRelease(iter);
}

/**
 * @brief Run the full USB probe: find, claim, configure and enumerate the device.
 * @return 0 on success; 1 if absent; other non-zero codes on failure stages.
 */
static int probeUSB(void) {
    printf("── USB device probe (VID 0x%04X / PID 0x%04X) ──\n", ER_VID, ER_PID);

    io_service_t service = findDevice();
    if (service == IO_OBJECT_NULL) {
        printf("  %s Eleven Rack not found on the USB bus.\n", WARN);
        printf("    Plug it in and re-run. (MIDI test below still runs.)\n");
        return 1;
    }
    printf("  %s device found\n", OK);
    reportOwningDriver(service);

    IOUSBDeviceInterface500 **dev = openDeviceInterface(service);
    IOObjectRelease(service);
    if (!dev) return 2;

    UInt16 vid=0, pid=0, rel=0;
    UInt8  nConfig=0, curConfig=0;
    (*dev)->GetDeviceVendor(dev, &vid);
    (*dev)->GetDeviceProduct(dev, &pid);
    (*dev)->GetDeviceReleaseNumber(dev, &rel);
    (*dev)->GetNumberOfConfigurations(dev, &nConfig);
    (*dev)->GetConfiguration(dev, &curConfig);
    printf("  descriptors: vid=0x%04x pid=0x%04x bcdDevice=0x%04x configs=%u current=%u\n",
           vid, pid, rel, nConfig, curConfig);

    // The critical test: can we take ownership from user space?
    IOReturn r = (*dev)->USBDeviceOpen(dev);
    if (r == kIOReturnExclusiveAccess) {
        printf("  %s plain open refused (%s) — trying Seize...\n", WARN, usbErr(r));
        r = (*dev)->USBDeviceOpenSeize(dev);
    }
    if (r != kIOReturnSuccess) {
        printf("  %s could NOT open device: 0x%x (%s)\n", BAD, r, usbErr(r));
        printf("    → A kernel driver is holding it exclusively. We may need a\n");
        printf("      codeless matching plist to keep AppleUSBAudio off config 2.\n");
        (*dev)->Release(dev);
        return 3;
    }
    printf("  %s device opened from user space — claiming works!\n", OK);

    // Dump what the device ACTUALLY exposes right now, and discover which
    // configuration value carries the isochronous audio endpoints.
    printf("\n  configuration descriptors:\n");
    UInt8 audioConfig = dumpConfigurations(dev, nConfig);

    if (audioConfig == 0) {
        printf("\n  %s no configuration with isochronous endpoints is exposed right now.\n", WARN);
        printf("    The device is in a non-audio personality (note the DFU interface).\n");
        printf("    Next step: identify the vendor request / mode-switch that makes it\n");
        printf("    re-enumerate with the audio configuration.\n");
        (*dev)->USBDeviceClose(dev);
        (*dev)->Release(dev);
        return 4;
    }

    // Select the real audio configuration value we just discovered.
    r = (*dev)->SetConfiguration(dev, audioConfig);
    printf("\n  %s SetConfiguration(%u): 0x%x (%s)\n",
           r == kIOReturnSuccess ? OK : BAD, audioConfig, r, usbErr(r));
    if (r == kIOReturnSuccess) {
        printf("\n  interfaces after configuration:\n");
        enumerateInterfaces(dev);
    }

    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return 0;
}

/** @brief Run the USB probe and the MIDI smoke test, then print a summary. */
int main(void) {
    printf("Eleven Rack — user-space USB + MIDI probe\n");
    printf("=========================================\n");
    int usbResult = probeUSB();
    midiSmokeTest();

    printf("\nSummary:\n");
    if (usbResult == 0)
        printf("  %s USB: device claimed and configured from user space. "
               "No driver needed.\n", OK);
    else if (usbResult == 1)
        printf("  %s USB: device absent — re-run with the Eleven Rack plugged in.\n", WARN);
    else
        printf("  %s USB: could not fully claim the device (see above).\n", BAD);
    printf("  %s MIDI: virtual endpoints can be created from user space.\n", OK);
    printf("\nProbe complete.\n");
    return 0;
}
