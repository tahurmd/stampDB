/**
 * @file tests_recovery.c
 * @brief Power-cut simulation (torn header) and recovery correctness.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Populate data and tear the last header to simulate power cut. */
static int write_powercut_pattern(void){
  // open fresh DB and write some rows, then simulate torn page by writing payload only
  remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin");
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK) return 1;
  for (int i=0;i<200;i++){ stampdb_write(db, 2, (uint32_t)(i*5), (float)i); }
  stampdb_flush(db);
  stampdb_close(db); free(ws);
  // Corrupt last page header (simulate power cut before header publish)
  FILE *f=fopen("flash.bin","r+b"); if(!f) return 2; fseek(f,0,SEEK_END); long sz=ftell(f); if (sz<256) { fclose(f); return 3; }
  // zero last 32 bytes of last programmed page
  fseek(f, (long)(sz - 256 + 224), SEEK_SET);
  unsigned char ff[32]; memset(ff,0xFF,32); fwrite(ff,1,32,f); fclose(f);
  return 0;
}

/** @brief Ensure data prior to torn page remains readable. */
int main(void){
  if (write_powercut_pattern()!=0) { fprintf(stderr,"setup failed\n"); return 1; }
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"reopen failed\n"); return 2; }
  // Ensure we can still read preceding data, and no crash on torn page
  stampdb_it_t it; stampdb_query_begin(db,2,0,2000,&it); int n=0; uint32_t ts; float v; while (stampdb_next(&it,&ts,&v)) n++; stampdb_query_end(&it);
  if (n==0){ fprintf(stderr,"no rows after recovery\n"); return 3; }
  stampdb_close(db); free(ws); return 0;
}
