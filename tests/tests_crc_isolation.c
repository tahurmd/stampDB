/**
 * @file tests_crc_isolation.c
 * @brief Corrupt a middle page and verify earlier data remains readable.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Remove sim artifacts to start from a blank device. */
static void reset_sim(void){
  remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin");
}

/** @brief Corrupt payload and verify early range still returns rows. */
int main(void){
  reset_sim();
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"open fail\n"); return 1; }
  for (int i=0;i<150;i++){ stampdb_write(db, 4, (uint32_t)(i*10), (float)i); }
  stampdb_flush(db); stampdb_close(db); free(ws);
  // Corrupt a middle page payload
  FILE *f=fopen("flash.bin","r+b"); if(!f) return 2; fseek(f, 4096*0 + 256*10 + 0, SEEK_SET); unsigned char z=0; fwrite(&z,1,1,f); fclose(f);
  // Reopen and ensure early rows are still readable
  ws = malloc(ws_bytes); if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"reopen fail\n"); return 3; }
  stampdb_it_t it; stampdb_query_begin(db,4,0,1000,&it); int n=0; uint32_t ts; float v; while (stampdb_next(&it,&ts,&v)) n++; stampdb_query_end(&it);
  if (n==0){ fprintf(stderr,"no rows before CRC\n"); return 4; }
  stampdb_close(db); free(ws); return 0;
}
