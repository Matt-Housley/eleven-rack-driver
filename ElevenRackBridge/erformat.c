/**
 * @file erformat.c
 * @brief Diagnostic: determine the capture stream's channel layout and word size.
 *
 * Identifies the capture stream's layout using two statistical signals:
 *   1. Lag-k autocorrelation of the 32-bit word stream gives the interleave
 *      period P (channel count): interleaved multichannel audio correlates
 *      strongly at lag = P (same channel, adjacent samples) and weakly at other
 *      lags.
 *   2. A mute-vs-play differential: per-lane RMS rises on audio lanes when a
 *      note is played and stays flat on marker/unused lanes.
 *
 * Runs in two prompted phases:
 *   - ~2 s "MUTE" — do not play (roll guitar volume to 0 or mute).
 *   - ~3 s "PLAY" — play a sustained, fairly loud note the whole time.
 *
 * Read-only capture (no playback). Build:
 * @code
 *   clang -o erformat erformat.c -framework IOKit -framework CoreFoundation -Wno-deprecated-declarations
 * @endcode
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
#include <math.h>

#define ER_VID 0x0DBA
#define ER_PID 0xB011
#define ER_IF_IN 4
#define ER_ALT 1
#define N_REQ 8
#define FPR 16
#define MAX_PKT 512
#define MAXLAG 16

static IOUSBInterfaceInterface500 **gIn=NULL;
static UInt8 gPipe=0; static UInt16 gReqLen=MAX_PKT;
static UInt64 gFrame=0; static bool gStop=false;

typedef struct { IOUSBIsocFrame fr[FPR]; UInt8 *data; } Req;
static Req gReqs[N_REQ];

// Continuous byte accumulation (carry partial word across frames), plus phase mark.
static UInt8  gCarry[4]; static int gCarryN=0;
static int32_t *gW=NULL; static size_t gN=0, gCap=0;
static size_t gPlayStart=0;         // word index where PLAY phase began
static int gPhase=0;                // 0=mute, 1=play
static int gLowByteNonZero=0;       // counts words whose low byte != 0

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

/**
 * @brief Accumulate raw bytes into a continuous int32 LE word stream.
 *
 * Prepends any carried bytes, emits whole little-endian int32 words, and carries
 * the remainder to the next call. This keeps a single global word phase even
 * when frames end mid-word.
 *
 * @param p    Pointer to the frame's bytes.
 * @param len  Byte count in this frame.
 */
static void pushBytes(const UInt8 *p,int len){
    static UInt8 tmp[MAX_PKT+4];
    int t=0;
    for(int i=0;i<gCarryN;i++) tmp[t++]=gCarry[i];
    memcpy(tmp+t,p,len); t+=len;
    int words=t/4, rem=t%4;
    if(gN+words>gCap){ gCap=(gCap?gCap*2:1<<18)+words; gW=realloc(gW,gCap*sizeof(int32_t)); }
    for(int i=0;i<words;i++){ const UInt8 *q=tmp+i*4;
        if(q[0]) gLowByteNonZero++;
        gW[gN++]=(int32_t)(q[0]|(q[1]<<8)|(q[2]<<16)|(q[3]<<24)); }
    gCarryN=rem; for(int i=0;i<rem;i++) gCarry[i]=tmp[words*4+i];
}

static void arm(Req *r);
/** @brief Isoc completion: push each frame's bytes into the word stream, then re-arm. */
static void done(void *rc,IOReturn res,void *a0){ (void)a0; Req *r=(Req*)rc;
    if(res!=kIOReturnSuccess&&res!=kIOReturnUnderrun){gStop=true;CFRunLoopStop(CFRunLoopGetCurrent());return;}
    for(int f=0;f<FPR;f++){ int len=r->fr[f].frActCount; if(len>0) pushBytes(r->data+f*MAX_PKT,len); }
    if(!gStop)arm(r);
}
/** @brief Submit one asynchronous isoc read request of @c FPR frames. */
static void arm(Req *r){ for(int f=0;f<FPR;f++){r->fr[f].frReqCount=gReqLen;r->fr[f].frActCount=0;r->fr[f].frStatus=0;}
    if((*gIn)->ReadIsochPipeAsync(gIn,gPipe,r->data,gFrame,FPR,r->fr,done,r)!=kIOReturnSuccess){gStop=true;CFRunLoopStop(CFRunLoopGetCurrent());return;} gFrame+=FPR; }

/** @brief Timer callback: switch from the MUTE phase to the PLAY phase. */
static void toPlay(CFRunLoopTimerRef t,void*i){(void)t;(void)i; gPhase=1; gPlayStart=gN;
    printf("\n  >>> PLAY NOW — sustained loud note <<<\n"); fflush(stdout); }
/** @brief Timer callback: stop capturing and exit the run loop. */
static void stopT(CFRunLoopTimerRef t,void*i){(void)t;(void)i;gStop=true;CFRunLoopStop(CFRunLoopGetCurrent());}

/** @brief Capture MUTE then PLAY phases, then report period and per-lane RMS. */
int main(int argc,char**argv){
    (void)argc;(void)argv;
    printf("Eleven Rack — capture format LOCK\n=================================\n");
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

    printf("  >>> MUTE — do NOT play for ~2 seconds <<<\n"); fflush(stdout);
    CFRunLoopTimerRef t1=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+2.0,0,0,0,toPlay,NULL);
    CFRunLoopTimerRef t2=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+5.0,0,0,0,stopT,NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(),t1,kCFRunLoopDefaultMode);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(),t2,kCFRunLoopDefaultMode);
    CFRunLoopRun();
    (*gIn)->SetAlternateInterface(gIn,0);(*gIn)->USBInterfaceClose(gIn);(*gIn)->Release(gIn);(*dev)->USBDeviceClose(dev);(*dev)->Release(dev);

    double sec=5.0, wps=gN/sec;
    printf("\ncaptured %zu words  (%.0f words/s)  playStart@%zu  lowByteNonZero=%d/%zu\n",
           gN,wps,gPlayStart,gLowByteNonZero,gN);

    // --- 1) autocorrelation over the PLAY phase to find the interleave period ---
    size_t a=gPlayStart+16, b=gN;               // skip a few words after the switch
    if(b<=a+64){ printf("  not enough play-phase data.\n"); return 9; }
    // scale to double, remove DC per candidate later; use raw here
    double *x=malloc((b-a)*sizeof(double)); size_t nx=b-a;
    for(size_t i=0;i<nx;i++) x[i]=(double)gW[a+i]/65536.0;
    double mean=0; for(size_t i=0;i<nx;i++)mean+=x[i]; mean/=nx;
    for(size_t i=0;i<nx;i++)x[i]-=mean;
    double e0=0; for(size_t i=0;i<nx;i++)e0+=x[i]*x[i];
    printf("\n  lag-k autocorrelation (peak at k = channel count / period):\n");
    int bestK=1; double bestC=-2;
    for(int k=1;k<=MAXLAG;k++){
        double s=0; for(size_t i=0;i+k<nx;i++) s+=x[i]*x[i+k];
        double c = e0>0 ? s/e0 : 0;
        printf("    k=%2d  corr=%+.3f  %s\n", k, c, c>0.6?"strong":c>0.3?"med":"");
        if(c>bestC){bestC=c;bestK=k;}
    }
    free(x);
    printf("  → strongest correlation at k=%d (corr=%.3f)\n", bestK, bestC);

    // --- 2) per-lane RMS, mute vs play, for the detected period P ---
    int P=bestK;
    printf("\n  per-lane RMS at P=%d  (audio lanes jump from MUTE→PLAY):\n", P);
    printf("    lane |     mute RMS |     play RMS | ratio\n");
    for(int lane=0;lane<P;lane++){
        double mS=0,pS=0; size_t mC=0,pC=0;
        for(size_t i=lane; i<gN; i+=P){
            double val=(double)gW[i]/65536.0;
            if(i<gPlayStart){ mS+=val*val; mC++; } else { pS+=val*val; pC++; }
        }
        double mR=mC?sqrt(mS/mC):0, pR=pC?sqrt(pS/pC):0;
        double ratio = mR>1e-6 ? pR/mR : (pR>1?999:0);
        printf("    %4d | %12.1f | %12.1f | %6.1fx %s\n", lane,mR,pR,ratio,
               ratio>3?"<< AUDIO":(pR<1&&mR<1)?"(silent/marker)":"");
    }

    printf("\n  word justification: low byte was %s → %s\n",
           gLowByteNonZero < (int)(gN/100) ? "almost always 0x00" : "often non-zero",
           gLowByteNonZero < (int)(gN/100) ? "24-bit data left-justified in 32-bit"
                                           : "full 32-bit (or different word size)");
    printf("  implied per-channel rate if P=%d: %.0f Hz\n", P, wps/P);
    free(gW);
    return 0;
}
