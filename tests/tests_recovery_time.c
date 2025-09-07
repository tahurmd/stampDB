/**
 * @file tests_recovery_time.c
 * @brief Bound recovery time proportional to segments since last snapshot.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000ull + (uint64_t)(ts.tv_nsec/1000000ull); }
static void reset_sim(void){ remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin"); }

/** @brief Fill segs, snapshot, add K more segs; measure reopen time bound. */
int main(void){
  reset_sim();
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK) return 1;
  // write enough to fill N segments, then snapshot
  int rows_per_block=74; int blocks_per_seg=(4096/256)-1; int rows_per_seg = rows_per_block*blocks_per_seg;
  int segs0=8; for (int i=0;i<segs0*rows_per_seg;i++){ stampdb_write(db, 7, (uint32_t)(i*10), (float)i); }
  stampdb_flush(db);
  if (stampdb_snapshot_save(db)!=STAMPDB_OK) return 2;
  // write K more segments after snapshot
  int k=6; int start=segs0*rows_per_seg; for (int i=0;i<k*rows_per_seg;i++){ stampdb_write(db, 7, (uint32_t)((start+i)*10), (float)i); }
  stampdb_flush(db);
  stampdb_close(db); free(ws);

  // reopen and measure recovery time
  ws = malloc(ws_bytes);
  cfg.workspace = ws;
  uint64_t t0=now_ms();
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ free(ws); return 3; }
  uint64_t t1=now_ms();
  stampdb_close(db); free(ws);
  uint64_t dur = t1 - t0;
  // bound: c1 + c2*k with slack for CI variance
  uint64_t c1=400; uint64_t c2=130; // ms (include slack for host variance)
  if (dur > c1 + c2*(uint64_t)k){ fprintf(stderr,"recovery too slow: %llums > %llums\n", (unsigned long long)dur, (unsigned long long)(c1+c2*k)); return 4; }
  return 0;
}
