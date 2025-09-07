/**
 * @file tests_gc_latency.c
 * @brief Induce GC and assert P99 write latency stays under a quota-bound cap.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000ull + (uint64_t)(ts.tv_nsec/1000000ull); }
static void reset_sim(void){ remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin"); }

/** @brief Write a stream of samples and compute P99 latency bound. */
int main(void){
  // Keep flash reasonably small so that GC triggers without long test time
  setenv("STAMPDB_SIM_FLASH_BYTES","262144",1); // 256 KiB -> 64 segments
  reset_sim();
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"open fail\n"); return 1; }
  // Generate steady writes to induce GC. Track latencies per write.
  const int N=2000; uint64_t *lat = (uint64_t*)malloc(sizeof(uint64_t)*N);
  uint32_t ts=0; float v=0.0f;
  for (int i=0;i<N;i++){
    uint64_t t0=now_ms();
    stampdb_write(db, 8, ts, v);
    uint64_t t1=now_ms();
    lat[i]=t1-t0;
    ts+=10; v+=0.01f;
    if ((i%100)==99) stampdb_flush(db);
  }
  stampdb_flush(db);
  stampdb_close(db); free(ws);
  // compute P99
  // simple nth element: sort copy
  uint64_t *cp=(uint64_t*)malloc(sizeof(uint64_t)*N); memcpy(cp,lat,sizeof(uint64_t)*N);
  for (int i=0;i<N-1;i++) for (int j=i+1;j<N;j++) if (cp[j]<cp[i]){ uint64_t tmp=cp[i]; cp[i]=cp[j]; cp[j]=tmp; }
  uint64_t p99 = cp[(N*99)/100];
  // bound P99 under 1500ms (quota delays can block up to ~1s)
  if (p99 > 1500){ fprintf(stderr,"P99 too high: %llums\n", (unsigned long long)p99); return 2; }
  free(cp); free(lat);
  return 0;
}
