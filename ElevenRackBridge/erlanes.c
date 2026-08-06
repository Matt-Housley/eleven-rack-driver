/**
 * @file erlanes.c
 * @brief Diagnostic: capture-format analyzer for the isochronous IN stream.
 *
 * The capture payload is a multi-lane interleaved superframe rather than plain
 * PCM. This tool captures the isochronous IN stream while a note is played,
 * treats the bytes as 32-bit little-endian lanes for several candidate channel
 * counts, and reports which lanes carry audio (large, smoothly-varying signal)
 * versus fixed marker/status lanes (constant). That identifies the channel map
 * and word size.
 *
 * Read-only capture. Play a sustained, fairly loud note for the whole window.
 *
 * Build:
 * @code
 *   clang -o erlanes erlanes.c -framework IOKit -framework CoreFoundation -Wno-deprecated-declarations
 * @endcode
 * Run: @c ./erlanes @c [seconds] (default 4).
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define ER_VID 0x0DBA
#define ER_PID 0xB011
#define ER_IF_IN 4
#define ER_ALT 1
#define N_REQ 8
#define FPR 16
#define MAX_PKT 512

#define MAX_LANES 16           // analyze up to 16 int32 lanes per superframe
#define KEEP 3000              // samples kept per lane for waveform preview

static IOUSBInterfaceInterface500 **gIn=NULL;
static UInt8 gPipe=0; static UInt16 gReqLen=MAX_PKT;
static UInt64 gFrame=0; static bool gStop=false;

typedef struct { IOUSBIsocFrame fr[FPR]; UInt8 *data; } Req;
static Req gReqs[N_REQ];

// Rolling int32 stream reconstructed from all frames, plus per-lane stats for a
// chosen period. We collect the raw int32 stream first, analyze at the end.
static int32_t *gStream=NULL; static size_t gCount=0, gCap=0;

/** @brief Open the USB device interface for a matched IOKit service. */
static IOUSBDeviceInterface500 **openDevice(io_service_t s){
    IOCFPlugInInterface **pl=NULL; SInt32 sc=0;
    if(IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc)!=KERN_SUCCESS||!pl)return NULL;
    IOUSBDeviceInterface500 **d=NULL;(*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&d);(*pl)->Release(pl);return d;
}
/** @brief Find and open the interface whose number equals @p want. */
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

static void arm(Req *r);
/** @brief Isoc completion: append each frame's int32 words to the capture buffer, then re-arm. */
static void done(void *rc,IOReturn res,void *a0){ (void)a0; Req *r=(Req*)rc;
    if(res!=kIOReturnSuccess&&res!=kIOReturnUnderrun){gStop=true;CFRunLoopStop(CFRunLoopGetCurrent());return;}
    for(int f=0;f<FPR;f++){ int len=r->fr[f].frActCount; if(len<4)continue;
        const UInt8 *p=r->data+f*MAX_PKT; int n=len/4;
        if(gCount+n>gCap){ gCap=(gCap?gCap*2:65536)+n; gStream=realloc(gStream,gCap*sizeof(int32_t)); }
        for(int i=0;i<n;i++){ const UInt8 *q=p+i*4; gStream[gCount++]=(int32_t)(q[0]|(q[1]<<8)|(q[2]<<16)|(q[3]<<24)); }
    }
    if(!gStop)arm(r);
}
/** @brief Submit one asynchronous isoc read request of @c FPR frames. */
static void arm(Req *r){ for(int f=0;f<FPR;f++){r->fr[f].frReqCount=gReqLen;r->fr[f].frActCount=0;r->fr[f].frStatus=0;}
    if((*gIn)->ReadIsochPipeAsync(gIn,gPipe,r->data,gFrame,FPR,r->fr,done,r)!=kIOReturnSuccess){gStop=true;CFRunLoopStop(CFRunLoopGetCurrent());return;} gFrame+=FPR; }
/** @brief Timer callback: stop capturing and exit the run loop. */
static void stopT(CFRunLoopTimerRef t,void*i){(void)t;(void)i;gStop=true;CFRunLoopStop(CFRunLoopGetCurrent());}

/**
 * @brief Score each lane for a candidate superframe period @p P.
 *
 * For @p P lanes per superframe, reports each lane's value range and mean
 * absolute first-difference (smoothness). Audio lanes have a large range;
 * among active lanes, real audio is smoother than noise, while constant lanes
 * are markers.
 *
 * @param P  Candidate number of int32 lanes per superframe.
 */
static void analyzePeriod(int P){
    if((size_t)P>gCount) return;
    printf("\n  === assuming %d lanes/superframe (%d-bit words) ===\n", P, 32);
    size_t groups=gCount/P;
    for(int lane=0;lane<P;lane++){
        int32_t mn=INT32_MAX,mx=INT32_MIN; long long prev=0,sumd=0; bool first=true;
        for(size_t g=0;g<groups;g++){
            int32_t v=gStream[g*P+lane];
            if(v<mn)mn=v; if(v>mx)mx=v;
            if(!first) sumd+=llabs((long long)v-prev);
            prev=v; first=false;
        }
        double range=(double)mx-(double)mn;
        double smooth= groups>1 ? (double)sumd/(groups-1) : 0;
        // ratio of step size to range: low => smooth waveform, high => noise/marker-jumps
        double ratio = range>0 ? smooth/range : 0;
        const char *tag = range<8 ? "constant (marker?)"
                        : ratio<0.15 ? "<< AUDIO (smooth)"
                        : "noisy/mixed";
        printf("    lane %2d: min=%11d max=%11d range=%.2e  step/range=%.3f  %s\n",
               lane,mn,mx,range,ratio,tag);
    }
}

/** @brief Capture the IN stream for a few seconds, then analyze lane structure. */
int main(int argc,char**argv){
    double sec=(argc>1)?atof(argv[1]):4.0;
    printf("Eleven Rack — capture format analyzer (%.1fs). PLAY a sustained note now.\n",sec);
    printf("========================================================================\n");
    SInt32 vid=ER_VID,pid=ER_PID;
    CFMutableDictionaryRef m=IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v=CFNumberCreate(NULL,kCFNumberSInt32Type,&vid),p=CFNumberCreate(NULL,kCFNumberSInt32Type,&pid);
    CFDictionarySetValue(m,CFSTR(kUSBVendorID),v);CFDictionarySetValue(m,CFSTR(kUSBProductID),p);CFRelease(v);CFRelease(p);
    io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,m); if(!svc){printf("device not found\n");return 1;}
    IOUSBDeviceInterface500 **dev=openDevice(svc);IOObjectRelease(svc); if(!dev)return 2;
    IOReturn r=(*dev)->USBDeviceOpen(dev); if(r==kIOReturnExclusiveAccess)r=(*dev)->USBDeviceOpenSeize(dev);
    if(r!=kIOReturnSuccess){printf("open 0x%x\n",r);return 3;} (*dev)->SetConfiguration(dev,1);
    gIn=openIface(dev,ER_IF_IN); if(!gIn){printf("no IN iface\n");return 4;}
    (*gIn)->USBInterfaceOpen(gIn); (*gIn)->SetAlternateInterface(gIn,ER_ALT);
    UInt8 nE=0;(*gIn)->GetNumEndpoints(gIn,&nE);
    for(UInt8 pp=1;pp<=nE;pp++){UInt8 d=0,nu=0,tt=0,iv=0;UInt16 mp=0;
        if((*gIn)->GetPipeProperties(gIn,pp,&d,&nu,&tt,&mp,&iv)==kIOReturnSuccess&&tt==kUSBIsoc&&d==kUSBIn){gPipe=pp;gReqLen=mp?mp:MAX_PKT;}}
    if(!gPipe){printf("no isoc IN pipe\n");return 5;}
    for(int i=0;i<N_REQ;i++){gReqs[i].data=malloc(FPR*MAX_PKT);memset(gReqs[i].data,0,FPR*MAX_PKT);}
    CFRunLoopSourceRef src=NULL;(*gIn)->CreateInterfaceAsyncEventSource(gIn,&src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),src,kCFRunLoopDefaultMode);
    UInt64 bus=0;AbsoluteTime at;(*gIn)->GetBusFrameNumber(gIn,&bus,&at); gFrame=bus+25;
    for(int i=0;i<N_REQ;i++)arm(&gReqs[i]);
    CFRunLoopTimerRef t=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+sec,0,0,0,stopT,NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(),t,kCFRunLoopDefaultMode); CFRunLoopRun();
    (*gIn)->SetAlternateInterface(gIn,0);(*gIn)->USBInterfaceClose(gIn);(*gIn)->Release(gIn);(*dev)->USBDeviceClose(dev);(*dev)->Release(dev);

    printf("\ncaptured %zu int32 words (%.0f words/s)\n", gCount, gCount/sec);
    // Try the most likely periods. 187KB/s ≈ 48k×4 → if these are 32-bit words,
    // ~48000 words/s means ~1 audio lane; more words/s scales the lane count.
    double wps=gCount/sec;
    printf("words/sec=%.0f  → if P lanes, per-lane rate = words/sec / P:\n", wps);
    for(int P=1;P<=MAX_LANES;P*=2)
        printf("    P=%2d → %.0f Hz/lane %s\n", P, wps/P,
               (wps/P>44000&&wps/P<49000)?"  <- standard 44.1/48k range":"");
    for(int P=1;P<=8;P*=2) analyzePeriod(P);

    // Show a short slice of each lane for the best-looking period so we can eyeball it.
    int P=8; if((size_t)P<gCount){
        printf("\n  first 16 superframes (P=%d), audio lanes should oscillate smoothly:\n",P);
        for(size_t g=0; g<16 && (g+1)*P<=gCount; g++){
            printf("   ");
            for(int l=0;l<P;l++) printf(" %9d", gStream[g*P+l]);
            printf("\n");
        }
    }
    free(gStream);
    return 0;
}
