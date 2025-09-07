/**
 * @file tests_powercut_matrix.c
 * @brief Matrix of power-cut scenarios: torn header, payload, footer.
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Remove sim artifacts to start from a blank device. */
static void reset_sim(void){
  remove("flash.bin"); remove("meta_snap_a.bin"); remove("meta_snap_b.bin"); remove("meta_head_hint.bin");
}

/** @brief Write 300 rows to series 3 to span several blocks. */
static int populate(void){
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK) return 1;
  for (int i=0;i<300;i++) stampdb_write(db, 3, (uint32_t)(i*10), (float)i);
  stampdb_flush(db);
  stampdb_close(db); free(ws); return 0;
}

/** @brief Overwrite the last 32 bytes of page (header) with 0xFF. */
static int corrupt_header_only(void){
  FILE *f=fopen("flash.bin","r+b"); if(!f) return -1; fseek(f,0,SEEK_END); long sz=ftell(f); if (sz<256) { fclose(f); return -1; }
  fseek(f, (long)(sz - 256 + 224), SEEK_SET); unsigned char ff[32]; memset(ff,0xFF,32); fwrite(ff,1,32,f); fclose(f); return 0;
}

/** @brief Flip a payload byte to force CRC mismatch. */
static int corrupt_payload_only(void){
  FILE *f=fopen("flash.bin","r+b"); if(!f) return -1; fseek(f,0,SEEK_END); long sz=ftell(f); if (sz<256) { fclose(f); return -1; }
  fseek(f, (long)(sz - 256 + 0), SEEK_SET); unsigned char z=0; fwrite(&z,1,1,f); fclose(f); return 0;
}

/** @brief Wipe the last 256 B footer page of the last segment. */
static int corrupt_footer(void){
  // wipe last 256B (footer)
  FILE *f=fopen("flash.bin","r+b"); if(!f) return -1; fseek(f,0,SEEK_END); long sz=ftell(f); if (sz<4096) { fclose(f); return -1; }
  fseek(f, (long)( (sz/4096)*4096 - 256 ), SEEK_SET); unsigned char ff[256]; memset(ff,0xFF,256); fwrite(ff,1,256,f); fclose(f); return 0;
}

/** @brief Reopen DB and count rows in [0..5000] for series 3. */
static int check_reads(void){
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ fprintf(stderr,"open fail\n"); free(ws); return -1; }
  stampdb_it_t it; stampdb_query_begin(db,3,0,5000,&it); int n=0; uint32_t ts; float v; while (stampdb_next(&it,&ts,&v)) n++; stampdb_query_end(&it);
  fprintf(stderr,"read rows=%d\n", n);
  stampdb_close(db); free(ws);
  return (n>0)?0:-1;
}

/** @brief Execute all four phases and ensure non-zero reads each time. */
int main(void){
  reset_sim(); fprintf(stderr,"phase1 populate\n"); if (populate()!=0) return 1; fprintf(stderr,"phase1 check\n"); if (check_reads()!=0) return 2;
  reset_sim(); fprintf(stderr,"phase2 populate\n"); if (populate()!=0) return 3; fprintf(stderr,"phase2 corrupt_header\n"); if (corrupt_header_only()!=0) return 4; fprintf(stderr,"phase2 check\n"); if (check_reads()!=0) return 5;
  reset_sim(); fprintf(stderr,"phase3 populate\n"); if (populate()!=0) return 6; fprintf(stderr,"phase3 corrupt_payload\n"); if (corrupt_payload_only()!=0) return 7; fprintf(stderr,"phase3 check\n"); if (check_reads()!=0) return 8;
  reset_sim(); fprintf(stderr,"phase4 populate\n"); if (populate()!=0) return 9; fprintf(stderr,"phase4 corrupt_footer\n"); if (corrupt_footer()!=0) return 10; fprintf(stderr,"phase4 check\n"); if (check_reads()!=0) return 11;
  return 0;
}
