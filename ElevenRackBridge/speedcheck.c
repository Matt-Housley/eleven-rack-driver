/**
 * @file speedcheck.c
 * @brief Diagnostic: report the Eleven Rack's USB link speed and endpoint bandwidth.
 *
 * Standalone tool. Locates the device by VID/PID, prints its negotiated USB
 * speed (low/full/high/super), then walks interface 4 alt 1 and reports each
 * isochronous pipe's max-packet size, high-speed multiplier and the resulting
 * per-microframe / per-second bandwidth. Used to confirm the device enumerated
 * at high speed with the expected isochronous payload size.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <stdio.h>

/** @brief Query device speed and interface-4 endpoint bandwidth, then print. */
int main(void){
  CFMutableDictionaryRef m=IOServiceMatching(kIOUSBDeviceClassName);
  SInt32 vid=0x0DBA,pid=0xB011;
  CFNumberRef v=CFNumberCreate(0,kCFNumberSInt32Type,&vid),p=CFNumberCreate(0,kCFNumberSInt32Type,&pid);
  CFDictionarySetValue(m,CFSTR(kUSBVendorID),v);CFDictionarySetValue(m,CFSTR(kUSBProductID),p);CFRelease(v);CFRelease(p);
  io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,m);
  if(!s){printf("not found\n");return 1;}
  // Device Speed property: 0=low,1=full,2=high,3=super
  CFTypeRef sp=IORegistryEntrySearchCFProperty(s,kIOServicePlane,CFSTR("Device Speed"),0,kIORegistryIterateRecursively);
  int speed=-1; if(sp){CFNumberGetValue(sp,kCFNumberIntType,&speed);CFRelease(sp);}
  const char* names[]={"low(1.5M)","full(12M)","high(480M)","super"};
  printf("Device Speed = %d (%s)\n", speed, (speed>=0&&speed<4)?names[speed]:"?");
  IOCFPlugInInterface **pl=0;SInt32 sc=0;
  IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc);
  IOUSBDeviceInterface500 **dev=0;(*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&dev);(*pl)->Release(pl);
  (*dev)->USBDeviceOpen(dev);(*dev)->SetConfiguration(dev,1);
  UInt8 sp2=0;(*dev)->GetDeviceSpeed(dev,&sp2); printf("GetDeviceSpeed = %d\n",sp2);
  // walk interface 4 alt 1 endpoint, show maxPacketSize + mult
  IOUSBFindInterfaceRequest req={0xFFFF,0xFFFF,0xFFFF,0xFFFF};io_iterator_t it;
  (*dev)->CreateInterfaceIterator(dev,&req,&it);io_service_t u;
  while((u=IOIteratorNext(it))){IOCFPlugInInterface**p2=0;SInt32 s2;
    if(IOCreatePlugInInterfaceForService(u,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&p2,&s2)==0&&p2){
      IOUSBInterfaceInterface500**intf=0;(*p2)->QueryInterface(p2,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),(LPVOID*)&intf);(*p2)->Release(p2);
      UInt8 n=0xFF;if(intf)(*intf)->GetInterfaceNumber(intf,&n);
      if(intf&&n==4){(*intf)->USBInterfaceOpen(intf);(*intf)->SetAlternateInterface(intf,1);
        UInt8 ne=0;(*intf)->GetNumEndpoints(intf,&ne);
        for(UInt8 pipe=1;pipe<=ne;pipe++){UInt8 d,nu,tt,iv;UInt16 mp;
          if((*intf)->GetPipeProperties(intf,pipe,&d,&nu,&tt,&mp,&iv)==0){
            int mult=((mp>>11)&3)+1; int base=mp&0x7FF;
            printf("IF4 pipe %d: dir=%d type=%d maxPkt=%u (base=%d mult=%d => %d B/microframe) interval=%d\n",
                   pipe,d,tt,mp,base,mult,base*mult,iv);
            printf("  => high-speed max/ms = %d B, /s = %d (%.1f KB/s)\n", base*mult*8, base*mult*8*1000, base*mult*8*1000/1024.0);
          }}
        (*intf)->USBInterfaceClose(intf);}
      if(intf)(*intf)->Release(intf);}
    IOObjectRelease(u);}
  (*dev)->USBDeviceClose(dev);
  return 0;
}
