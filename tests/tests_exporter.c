/**
 * @file tests_exporter.c
 * @brief Populate deterministic data and validate CLI exporter produces rows.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Remove sim artifacts to start from a blank device. */
static void reset_sim(void){
  remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin");
}

/** @brief Export a limited range and ensure output file has data lines. */
int main(void){
  reset_sim();
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"open fail\n"); return 1; }
  // write deterministic data for series 5 in [0..9990]
  for (int i=0;i<1000;i++){ stampdb_write(db, 5, (uint32_t)(i*10), (float)(i%100)); }
  stampdb_flush(db); stampdb_close(db); free(ws);
  // run exporter
  int rc = system("./build/mk/stampctl export --series 5 --t0 0 --t1 5000 --csv > out.csv");
  (void)rc;
  FILE *f=fopen("out.csv","rb"); if(!f){ fprintf(stderr,"no export\n"); return 2; }
  char line[256]; int lines=0; while (fgets(line,sizeof(line),f)) lines++; fclose(f);
  if (lines < 2){ fprintf(stderr,"no rows exported\n"); return 3; }
  return 0;
}
