/**
 * @file erduplex.c
 * @brief Diagnostic: full-duplex isochronous capture + playback test from user space.
 *
 * Runs capture (IF4, isoc IN) and playback (IF3, isoc OUT) at the same time from
 * user space. Playback sends silence (all zeros), so nothing extra is pushed to
 * the amp — only the Rack's own monitoring of the guitar is heard.
 *
 * Goals:
 *   1. Confirm concurrent duplex isochronous streaming works with no driver.
 *   2. Identify the capture sample format: hex-dump a live frame and print level
 *      meters under three interpretations (16/24/32-bit stereo) to see which one
 *      tracks playing.
 *   3. Sanity-check the implied sample rate with playback clocking the device.
 *
 * Build:
 * @code
 *   clang -o erduplex erduplex.c \
 *       -framework IOKit -framework CoreFoundation -Wno-deprecated-declarations
 * @endcode
 * Run (play some notes during the capture window): @c ./erduplex @c [seconds]
 * (default 4).
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

#define ER_VID          0x0DBA
#define ER_PID          0xB011
#define ER_CONFIG_VALUE 1
#define ER_IF_OUT       3
#define ER_IF_IN        4
#define ER_ALT_ACTIVE   1

#define N_REQ           8
#define FRAMES_PER_REQ  16
#define MAX_PKT         512
#define OUT_BYTES_FRAME 288      // ~48 samples/frame × 6 bytes; silence, so exact size is uncritical

typedef struct {
    int            index;
    IOUSBIsocFrame frames[FRAMES_PER_REQ];
    UInt8         *data;
    bool           isRead;
} Req;

static IOUSBInterfaceInterface500 **gIn  = NULL;
static IOUSBInterfaceInterface500 **gOut = NULL;
static UInt8   gInPipe = 0, gOutPipe = 0;
static UInt16  gInReqLen = MAX_PKT;
static UInt64  gInFrame = 0, gOutFrame = 0;
static Req     gInReqs[N_REQ], gOutReqs[N_REQ];
static bool    gStop = false;

// Capture stats
static unsigned long long gFrames = 0, gBytes = 0, gErr = 0, gZero = 0;
static int gMin = 1<<30, gMax = 0;
static long gPeak16L=0,gPeak16R=0,gPeak24L=0,gPeak24R=0,gPeak32L=0,gPeak32R=0;
static bool gDumped = false;

// Playback stats
static unsigned long long gOutFrames = 0, gOutErr = 0;

/** @brief Open the USB device interface for a matched IOKit service. */
static IOUSBDeviceInterface500 **openDevice(io_service_t s){
    IOCFPlugInInterface **pl=NULL; SInt32 sc=0;
    if(IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc)!=KERN_SUCCESS||!pl)return NULL;
    IOUSBDeviceInterface500 **d=NULL;
    (*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&d);
    (*pl)->Release(pl); return d;
}
/** @brief Find and open the interface whose number equals @p want. */
static IOUSBInterfaceInterface500 **openInterface(IOUSBDeviceInterface500 **dev,UInt8 want){
    IOUSBFindInterfaceRequest r={kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare};
    io_iterator_t it=0; if((*dev)->CreateInterfaceIterator(dev,&r,&it)!=kIOReturnSuccess)return NULL;
    io_service_t s; IOUSBInterfaceInterface500 **found=NULL;
    while((s=IOIteratorNext(it))){
        IOCFPlugInInterface **pl=NULL; SInt32 sc=0;
        if(IOCreatePlugInInterfaceForService(s,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc)==KERN_SUCCESS&&pl){
            IOUSBInterfaceInterface500 **intf=NULL;
            (*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),(LPVOID*)&intf);
            (*pl)->Release(pl);
            UInt8 n=0xFF; if(intf)(*intf)->GetInterfaceNumber(intf,&n);
            if(intf&&n==want){found=intf;IOObjectRelease(s);break;}
            if(intf)(*intf)->Release(intf);
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it); return found;
}
/**
 * @brief Locate the isochronous pipe of the requested direction on an interface.
 * @param intf    The opened interface.
 * @param wantIn  @c true for the IN pipe, @c false for the OUT pipe.
 * @param maxPkt  Receives the pipe's max-packet size (may be @c NULL).
 * @return The pipe index, or 0 if none matched.
 */
static UInt8 findPipe(IOUSBInterfaceInterface500 **intf,bool wantIn,UInt16 *maxPkt){
    UInt8 nEnd=0;(*intf)->GetNumEndpoints(intf,&nEnd);
    for(UInt8 p=1;p<=nEnd;p++){
        UInt8 dir=0,num=0,tt=0,iv=0;UInt16 mp=0;
        if((*intf)->GetPipeProperties(intf,p,&dir,&num,&tt,&mp,&iv)==kIOReturnSuccess&&tt==kUSBIsoc){
            if((wantIn&&dir==kUSBIn)||(!wantIn&&dir==kUSBOut)){ if(maxPkt)*maxPkt=mp; return p; }
        }
    }
    return 0;
}

/** @brief Print up to 64 bytes of a frame as a hex dump. */
static void hexDump(const UInt8 *p,int len){
    printf("  first data frame, %d bytes (hex):\n", len);
    for(int i=0;i<len && i<64;i+=16){
        printf("    %04d: ", i);
        for(int j=0;j<16 && i+j<len;j++) printf("%02x ", p[i+j]);
        printf("\n");
    }
}

/** @brief Interpret the payload as 16/24/32-bit stereo and track per-channel peaks. */
static void scanAll(const UInt8 *p,int len){
    // 16-bit stereo, stride 4 (2 bytes/ch)
    for(int i=0;i+4<=len;i+=4){
        long l=(short)(p[i]|(p[i+1]<<8)), r=(short)(p[i+2]|(p[i+3]<<8));
        if(labs(l)>gPeak16L)gPeak16L=labs(l); if(labs(r)>gPeak16R)gPeak16R=labs(r);
    }
    // 24-bit stereo, stride 6 (3 bytes/ch)
    for(int i=0;i+6<=len;i+=6){
        long l=p[i]|(p[i+1]<<8)|(p[i+2]<<16); if(l&0x800000)l|=~0xFFFFFFL;
        long r=p[i+3]|(p[i+4]<<8)|(p[i+5]<<16); if(r&0x800000)r|=~0xFFFFFFL;
        if(labs(l)>gPeak24L)gPeak24L=labs(l); if(labs(r)>gPeak24R)gPeak24R=labs(r);
    }
    // 32-bit stereo, stride 8 (4 bytes/ch)
    for(int i=0;i+8<=len;i+=8){
        long l=(int)(p[i]|(p[i+1]<<8)|(p[i+2]<<16)|(p[i+3]<<24));
        long r=(int)(p[i+4]|(p[i+5]<<8)|(p[i+6]<<16)|(p[i+7]<<24));
        if(labs(l)>gPeak32L)gPeak32L=labs(l); if(labs(r)>gPeak32R)gPeak32R=labs(r);
    }
}

static void armRead(Req *r);
static void armWrite(Req *r);

/** @brief IN completion: tally frames, dump/scan the first payload, then re-arm. */
static void readDone(void *refcon,IOReturn result,void *a0){
    (void)a0; Req *r=(Req*)refcon;
    if(result!=kIOReturnSuccess && result!=kIOReturnUnderrun){ gStop=true; CFRunLoopStop(CFRunLoopGetCurrent()); return; }
    for(int f=0;f<FRAMES_PER_REQ;f++){
        IOUSBIsocFrame *fr=&r->frames[f]; int len=fr->frActCount;
        gFrames++; gBytes+=len;
        if(fr->frStatus!=kIOReturnSuccess && fr->frStatus!=kIOReturnUnderrun) gErr++;
        if(len==0){gZero++;continue;}
        if(len<gMin)gMin=len; if(len>gMax)gMax=len;
        if(!gDumped && len>=32){ hexDump(r->data+f*MAX_PKT,len); gDumped=true; }
        scanAll(r->data+f*MAX_PKT,len);
    }
    if(!gStop)armRead(r);
}
/** @brief OUT completion: tally sent frames/errors, then re-arm. */
static void writeDone(void *refcon,IOReturn result,void *a0){
    (void)a0; Req *r=(Req*)refcon;
    if(result!=kIOReturnSuccess && result!=kIOReturnUnderrun){ return; } // ignore benign
    for(int f=0;f<FRAMES_PER_REQ;f++){
        gOutFrames++;
        if(r->frames[f].frStatus!=kIOReturnSuccess && r->frames[f].frStatus!=kIOReturnUnderrun) gOutErr++;
    }
    if(!gStop)armWrite(r);
}

/** @brief Submit one asynchronous isoc IN read request. */
static void armRead(Req *r){
    for(int f=0;f<FRAMES_PER_REQ;f++){ r->frames[f].frReqCount=gInReqLen; r->frames[f].frActCount=0; r->frames[f].frStatus=0; }
    IOReturn ret=(*gIn)->ReadIsochPipeAsync(gIn,gInPipe,r->data,gInFrame,FRAMES_PER_REQ,r->frames,readDone,r);
    if(ret!=kIOReturnSuccess){ printf("  read arm failed: 0x%x\n",ret); gStop=true; CFRunLoopStop(CFRunLoopGetCurrent()); return; }
    gInFrame+=FRAMES_PER_REQ;
}
/** @brief Submit one asynchronous isoc OUT write request of silence. */
static void armWrite(Req *r){
    // data buffer is already all zeros (silence); just set the per-frame counts.
    for(int f=0;f<FRAMES_PER_REQ;f++){ r->frames[f].frReqCount=OUT_BYTES_FRAME; r->frames[f].frActCount=0; r->frames[f].frStatus=0; }
    IOReturn ret=(*gOut)->WriteIsochPipeAsync(gOut,gOutPipe,r->data,gOutFrame,FRAMES_PER_REQ,r->frames,writeDone,r);
    if(ret!=kIOReturnSuccess){ printf("  write arm failed: 0x%x\n",ret); return; }
    gOutFrame+=FRAMES_PER_REQ;
}

/** @brief Timer callback: stop streaming and exit the run loop. */
static void stopTimer(CFRunLoopTimerRef t,void *i){ (void)t;(void)i; gStop=true; CFRunLoopStop(CFRunLoopGetCurrent()); }

/** @brief Print one interpretation's peak levels as absolute values and % of full-scale. */
static void pk(const char*name,long l,long r,long full){
    printf("    %-7s L=%8ld R=%8ld   (%.1f%% / %.1f%%)\n",name,l,r,100.0*l/full,100.0*r/full);
}

/** @brief Run concurrent capture + silent playback, then report format and rate. */
int main(int argc,char**argv){
    double seconds=(argc>1)?atof(argv[1]):4.0;
    printf("Eleven Rack — full-duplex isochronous test (%.1fs)\n",seconds);
    printf("Playback = silence.  Play your guitar during the capture window.\n");
    printf("==================================================================\n");

    SInt32 vid=ER_VID,pid=ER_PID;
    CFMutableDictionaryRef m=IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v=CFNumberCreate(NULL,kCFNumberSInt32Type,&vid),p=CFNumberCreate(NULL,kCFNumberSInt32Type,&pid);
    CFDictionarySetValue(m,CFSTR(kUSBVendorID),v); CFDictionarySetValue(m,CFSTR(kUSBProductID),p);
    CFRelease(v);CFRelease(p);
    io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,m);
    if(!svc){printf("  device not found.\n");return 1;}
    IOUSBDeviceInterface500 **dev=openDevice(svc); IOObjectRelease(svc);
    if(!dev){printf("  device iface failed.\n");return 2;}
    IOReturn r=(*dev)->USBDeviceOpen(dev);
    if(r==kIOReturnExclusiveAccess)r=(*dev)->USBDeviceOpenSeize(dev);
    if(r!=kIOReturnSuccess){printf("  open failed 0x%x\n",r);return 3;}
    (*dev)->SetConfiguration(dev,ER_CONFIG_VALUE);

    gIn =openInterface(dev,ER_IF_IN);
    gOut=openInterface(dev,ER_IF_OUT);
    if(!gIn||!gOut){printf("  could not open both audio interfaces (in=%p out=%p)\n",(void*)gIn,(void*)gOut);return 4;}
    if((*gIn)->USBInterfaceOpen(gIn)!=kIOReturnSuccess||(*gOut)->USBInterfaceOpen(gOut)!=kIOReturnSuccess){printf("  iface open failed\n");return 5;}
    if((*gIn)->SetAlternateInterface(gIn,ER_ALT_ACTIVE)!=kIOReturnSuccess||(*gOut)->SetAlternateInterface(gOut,ER_ALT_ACTIVE)!=kIOReturnSuccess){printf("  alt setting failed\n");return 6;}

    UInt16 inMax=0,outMax=0;
    gInPipe =findPipe(gIn,true,&inMax);
    gOutPipe=findPipe(gOut,false,&outMax);
    if(!gInPipe||!gOutPipe){printf("  isoc pipe(s) missing (in=%u out=%u)\n",gInPipe,gOutPipe);return 7;}
    gInReqLen=inMax?inMax:MAX_PKT;
    printf("  IN  pipe %u maxPkt=%u   OUT pipe %u maxPkt=%u  (OUT sends %d B/frame silence)\n",
           gInPipe,inMax,gOutPipe,outMax,OUT_BYTES_FRAME);

    for(int i=0;i<N_REQ;i++){
        gInReqs[i].index=i;  gInReqs[i].data =malloc(FRAMES_PER_REQ*MAX_PKT); memset(gInReqs[i].data,0,FRAMES_PER_REQ*MAX_PKT); gInReqs[i].isRead=true;
        gOutReqs[i].index=i; gOutReqs[i].data=malloc(FRAMES_PER_REQ*MAX_PKT); memset(gOutReqs[i].data,0,FRAMES_PER_REQ*MAX_PKT); gOutReqs[i].isRead=false;
    }

    CFRunLoopSourceRef si=NULL,so=NULL;
    (*gIn)->CreateInterfaceAsyncEventSource(gIn,&si);
    (*gOut)->CreateInterfaceAsyncEventSource(gOut,&so);
    if(!si||!so){printf("  async source failed\n");return 8;}
    CFRunLoopAddSource(CFRunLoopGetCurrent(),si,kCFRunLoopDefaultMode);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),so,kCFRunLoopDefaultMode);

    UInt64 bus=0;AbsoluteTime at;(*gIn)->GetBusFrameNumber(gIn,&bus,&at);
    gInFrame=gOutFrame=bus+25;
    printf("  streaming from frame %llu ... play now!\n\n",gInFrame);
    for(int i=0;i<N_REQ;i++){ armWrite(&gOutReqs[i]); armRead(&gInReqs[i]); }

    CFRunLoopTimerRef timer=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+seconds,0,0,0,stopTimer,NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(),timer,kCFRunLoopDefaultMode);
    CFRunLoopRun();

    (*gIn)->SetAlternateInterface(gIn,0);  (*gOut)->SetAlternateInterface(gOut,0);
    (*gIn)->USBInterfaceClose(gIn);        (*gOut)->USBInterfaceClose(gOut);
    (*gIn)->Release(gIn);                  (*gOut)->Release(gOut);
    (*dev)->USBDeviceClose(dev);           (*dev)->Release(dev);

    printf("\n── capture results ──\n");
    printf("  frames=%llu (%.0f/s)  bytes=%llu (%.1f KB/s)  err=%llu empty=%llu\n",
           gFrames,gFrames/seconds,gBytes,gBytes/seconds/1024.0,gErr,gZero);
    printf("  frame payload: min=%d max=%d avg=%.1f B\n", gMin==(1<<30)?0:gMin,gMax, gFrames?(double)gBytes/gFrames:0);
    double bps=gBytes/seconds;
    printf("  implied sample rate if 4 B/frame-sample: %.0f Hz\n", bps/4.0);
    printf("  implied sample rate if 6 B/frame-sample: %.0f Hz\n", bps/6.0);
    printf("  implied sample rate if 8 B/frame-sample: %.0f Hz\n", bps/8.0);
    printf("\n  peak levels by interpretation (the one that jumps when you play is correct):\n");
    pk("16-bit",gPeak16L,gPeak16R,0x7FFF);
    pk("24-bit",gPeak24L,gPeak24R,0x7FFFFF);
    pk("32-bit",gPeak32L,gPeak32R,0x7FFFFFFF);

    printf("\n── playback results ──\n");
    printf("  frames=%llu (%.0f/s)  err=%llu\n",gOutFrames,gOutFrames/seconds,gOutErr);

    bool ok = gFrames>0 && gErr<gFrames/10 && gOutFrames>0;
    printf("\n  %s full-duplex user-space isochronous streaming %s\n",
           ok?"[ok]":"[XX]", ok?"WORKS — no driver required.":"did not run cleanly.");
    return ok?0:9;
}
