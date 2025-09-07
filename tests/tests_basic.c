/**
 * @file tests_basic.c
 * @brief Sanity test: write a few rows, query range, check latest.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int approx_eq(float a, float b, float eps){ return fabsf(a-b) <= eps; }

/** @brief Entry point for basic integration test. */
int main(void){
  // fresh DB
  remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin");
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"open failed\n"); return 1; }
  // write some rows
  for (int i=0;i<500;i++){
    float v = sinf(0.01f*(float)i);
    if (stampdb_write(db, 1, (uint32_t)(i*10), v)!=STAMPDB_OK){ fprintf(stderr,"write failed @%d\n",i); return 2; }
  }
  stampdb_flush(db);
  // query a range
  stampdb_it_t it; if (stampdb_query_begin(db,1, 100, 2200, &it)!=STAMPDB_OK) { fprintf(stderr,"query begin failed\n"); return 3; }
  uint32_t ts; float val; int count=0; while (stampdb_next(&it,&ts,&val)) { count++; }
  stampdb_query_end(&it);
  if (count==0){ fprintf(stderr,"no rows returned\n"); return 4; }
  // latest
  uint32_t lts; float lv; if (stampdb_query_latest(db,1,&lts,&lv)!=STAMPDB_OK){ fprintf(stderr,"latest failed\n"); return 5; }
  if (lts < 4990){ fprintf(stderr,"latest timestamp wrong: %u\n", lts); return 6; }
  stampdb_close(db); free(ws);
  return 0;
}
