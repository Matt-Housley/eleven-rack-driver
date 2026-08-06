/**
 * @file ringstat.c
 * @brief Diagnostic: attach to a running engine's shared ring and report health.
 *
 * Standalone tool. Attaches to the shared-memory audio ring published by
 * @c erengine and prints the ring header (sample rate, channel counts, run
 * flags) followed by a five-second sampling of the input-ring fill level,
 * write/read throughput and xrun count. Useful for confirming the engine is
 * streaming and for spotting under/overrun.
 */
#include "ERAudioRing.h"
#include <stdio.h>
#include <unistd.h>

/** @brief Attach to the ring and watch fill/throughput for ~5 s. */
int main(){
  ERRing* r=er_ring_attach();
  if(!r){printf("ring not found (is erengine running?)\n");return 1;}
  printf("ring: magic ok, sampleRate=%u inCh=%u outCh=%u engineRunning=%u streamingRequested=%u\n",
    r->sampleRate,r->inChannels,r->outChannels,r->engineRunning,r->streamingRequested);
  printf("cap=%u frames (%.0f ms). watching fill level for 5s...\n",ER_RING_FRAMES,1000.0*ER_RING_FRAMES/48000);
  uint64_t lastW=er_load(&r->inWrite), lastR=er_load(&r->inRead); uint32_t lastX=r->xrunCount;
  for(int i=0;i<10;i++){
    usleep(500000);
    uint64_t w=er_load(&r->inWrite), rd=er_load(&r->inRead);
    uint32_t fill=(uint32_t)(w-rd);
    printf("  t=%.1fs  fill=%6u frames (%.0f ms)  wrote=%llu/0.5s (%.0f/s)  read=%llu/0.5s  xruns=%u\n",
      (i+1)*0.5, fill, 1000.0*fill/48000, w-lastW, (w-lastW)*2.0, rd-lastR, r->xrunCount);
    lastW=w; lastR=rd; lastX=r->xrunCount;
  }
  er_ring_close(r,0);
  return 0;
}
