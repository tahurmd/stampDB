/**
 * @file stampctl.c
 * @brief Command-line tools: export, retention, info, and ingest helpers.
 *
 * Subcommands:
 *  - export    → CSV/NDJSON by time range
 *  - retention → rough capacity estimator
 *  - info      → print DB stats (head/tail, blocks, CRCs, GC, recovery)
 *  - ingest    → write N rows for demos/tests
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Print brief usage. */
static void usage(void){
  fprintf(stderr, "Usage: stampctl export --series S --t0 ms --t1 ms [--csv|--ndjson]\n");
  fprintf(stderr, "       stampctl retention --days D\n");
  fprintf(stderr, "       stampctl info\n");
  fprintf(stderr, "       stampctl ingest --series S --rows N [--period-ms P] [--start 0]\n");
  fprintf(stderr, "\nOne-word helpers:\n");
  fprintf(stderr, "  stampctl reset   # delete sim files (flash.bin, meta_*)\n");
  fprintf(stderr, "  stampctl hello   # write 20 rows to series 1 and print a short CSV\n");
  fprintf(stderr, "  stampctl peek    # print latest row for series 1\n");
  fprintf(stderr, "  stampctl dump    # export all rows for series 1 as CSV\n");
}

/**
 * @brief Export rows for a series in [t0..t1] to CSV or NDJSON on stdout.
 */
static int cmd_export(int argc, char **argv){
  uint16_t series=0; uint32_t t0=0,t1=0; int fmt=0; // 0=csv,1=ndjson
  for (int i=2;i<argc;i++){
    if (strcmp(argv[i],"--series")==0 && i+1<argc){ series=(uint16_t)atoi(argv[++i]); }
    else if (strcmp(argv[i],"--t0")==0 && i+1<argc){ t0=(uint32_t)strtoul(argv[++i],NULL,10); }
    else if (strcmp(argv[i],"--t1")==0 && i+1<argc){ t1=(uint32_t)strtoul(argv[++i],NULL,10); }
    else if (strcmp(argv[i],"--csv")==0){ fmt=0; }
    else if (strcmp(argv[i],"--ndjson")==0){ fmt=1; }
  }
  if (t1<t0) t1=t0;
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes); // 1 MiB workspace (host only)
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db, &cfg)!=STAMPDB_OK){ fprintf(stderr, "open failed\n"); return 1; }
  stampdb_it_t it; if (stampdb_query_begin(db, series, t0, t1, &it)!=STAMPDB_OK){ fprintf(stderr, "query begin failed\n"); return 2; }
  if (fmt==0) printf("ts_ms,value\n");
  uint32_t ts; float v;
  while (stampdb_next(&it,&ts,&v)){
    if (fmt==0) printf("%u,%.9g\n", ts, v);
    else printf("{\"ts_ms\":%u,\"value\":%.9g}\n", ts, v);
  }
  stampdb_query_end(&it);
  stampdb_close(db); free(ws);
  return 0;
}

/**
 * @brief Print a rough capacity estimate and rows per day given flash size.
 */
static int cmd_retention(int argc, char **argv){
  if (argc<3) { usage(); return 1; }
  double days = atof(argv[2]);
  // Very rough capacity calculator for host: rows per segment
  double rows_per_block = 64.0; // avg
  double blocks_per_seg = (double)(4096/256 - 1);
  double rows_per_seg = rows_per_block * blocks_per_seg;
  uint32_t flash_bytes = 4*1024*1024; const char* env = getenv("STAMPDB_SIM_FLASH_BYTES"); if (env) { unsigned long v=strtoul(env,NULL,10); if (v>0) flash_bytes=(uint32_t)v; }
  double segs = (double)flash_bytes / 4096.0;
  double cap_rows = segs * rows_per_seg;
  printf("Estimated capacity: %.0f rows (~%.2f days @ 1 row/s)\n", cap_rows, cap_rows / 86400.0);
  if (days>0) printf("Target days %.2f => max rows %.0f\n", days, days*86400.0);
  return 0;
}

static int cmd_info(void){
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  if (!ws) { fprintf(stderr, "oom\n"); return 1; }
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db, &cfg)!=STAMPDB_OK){ fprintf(stderr, "open failed\n"); free(ws); return 2; }
  stampdb_stats_t st; stampdb_info(db,&st);
  printf("seg_seq_head=%u seg_seq_tail=%u blocks_written=%u crc_errors=%u gc_warn_events=%u gc_busy_events=%u recovery_truncations=%u\n",
         st.seg_seq_head, st.seg_seq_tail, st.blocks_written, st.crc_errors,
         st.gc_warn_events, st.gc_busy_events, st.recovery_truncations);
  stampdb_close(db); free(ws); return 0;
}

static int cmd_ingest(int argc, char **argv){
  uint16_t series=0; int rows=0; uint32_t period=100; uint32_t start=0;
  for (int i=2;i<argc;i++){
    if (strcmp(argv[i],"--series")==0 && i+1<argc) series=(uint16_t)atoi(argv[++i]);
    else if (strcmp(argv[i],"--rows")==0 && i+1<argc) rows=atoi(argv[++i]);
    else if (strcmp(argv[i],"--period-ms")==0 && i+1<argc) period=(uint32_t)strtoul(argv[++i],NULL,10);
    else if (strcmp(argv[i],"--start")==0 && i+1<argc) start=(uint32_t)strtoul(argv[++i],NULL,10);
  }
  if (rows<=0){ fprintf(stderr,"ingest: --rows N required\n"); return 1; }
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  if (!ws) { fprintf(stderr, "oom\n"); return 1; }
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db, &cfg)!=STAMPDB_OK){ fprintf(stderr, "open failed\n"); free(ws); return 2; }
  uint32_t ts = start; float v = 25.0f;
  for (int i=0;i<rows;i++){ stampdb_write(db, series, ts, v); ts += period; v += 0.1f; }
  stampdb_flush(db);
  stampdb_close(db); free(ws);
  printf("ingested %d rows to series %u\n", rows, series);
  return 0;
}

static int rm_path(const char* p){ return remove(p)==0 ? 0 : -1; }

static void build_meta_path(char *out, size_t cap, const char *fname){
  const char *dir = getenv("STAMPDB_META_DIR"); if (!dir || !*dir) dir = ".";
  snprintf(out, cap, "%s/%s", dir, fname);
}

static int cmd_reset(void){
  const char *flash = getenv("STAMPDB_FLASH_PATH");
  char meta_a[512], meta_b[512], head[512];
  build_meta_path(meta_a,sizeof meta_a,"meta_snap_a.bin");
  build_meta_path(meta_b,sizeof meta_b,"meta_snap_b.bin");
  build_meta_path(head ,sizeof head ,"meta_head_hint.bin");
  int removed=0;
  if (!flash || !*flash) flash = "flash.bin";
  if (rm_path(flash)==0) { printf("removed %s\n", flash); removed++; }
  if (rm_path(meta_a)==0) { printf("removed %s\n", meta_a); removed++; }
  if (rm_path(meta_b)==0) { printf("removed %s\n", meta_b); removed++; }
  if (rm_path(head )==0) { printf("removed %s\n", head ); removed++; }
  if (removed==0) printf("nothing to remove\n");
  return 0;
}

static int cmd_peek(void){
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  if (!ws) { fprintf(stderr, "oom\n"); return 1; }
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db, &cfg)!=STAMPDB_OK){ fprintf(stderr, "open failed\n"); free(ws); return 2; }
  uint32_t ts=0; float v=0;
  if (stampdb_query_latest(db,1,&ts,&v)==STAMPDB_OK){ printf("%u,%.9g\n", ts, v); }
  else { fprintf(stderr,"no data for series 1\n"); }
  stampdb_close(db); free(ws); return 0;
}

static int cmd_dump(void){
  // export series 1 across full range as CSV to stdout
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  if (!ws) { fprintf(stderr, "oom\n"); return 1; }
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db, &cfg)!=STAMPDB_OK){ fprintf(stderr, "open failed\n"); free(ws); return 2; }
  printf("ts_ms,value\n");
  stampdb_it_t it; if (stampdb_query_begin(db,1,0,0xFFFFFFFFu,&it)!=STAMPDB_OK){ fprintf(stderr,"query begin failed\n"); stampdb_close(db); free(ws); return 3; }
  uint32_t ts; float v; while (stampdb_next(&it,&ts,&v)) printf("%u,%.9g\n", ts, v);
  stampdb_query_end(&it);
  stampdb_close(db); free(ws); return 0;
}

static int cmd_hello(void){
  // write 20 rows to series 1 and print a short CSV sample
  const int rows=20; const uint32_t period=100; const uint32_t t0=0; const uint32_t t1=rows*period;
  char *argv_ingest[]={(char*)"stampctl",(char*)"ingest",(char*)"--series",(char*)"1",(char*)"--rows",(char*)"20",(char*)"--period-ms",(char*)"100"};
  (void)cmd_ingest(8, argv_ingest);
  // now dump a small range
  size_t ws_bytes = 1<<20; void *ws = malloc(ws_bytes);
  stampdb_t *db=NULL; stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=(uint32_t)ws_bytes,.read_batch_rows=512,.commit_interval_ms=0};
  if (stampdb_open(&db, &cfg)!=STAMPDB_OK){ fprintf(stderr, "open failed\n"); free(ws); return 2; }
  printf("ts_ms,value\n");
  stampdb_it_t it; if (stampdb_query_begin(db,1,t0,t1,&it)!=STAMPDB_OK){ fprintf(stderr,"query begin failed\n"); stampdb_close(db); free(ws); return 3; }
  uint32_t ts; float v; int printed=0; while (printed<10 && stampdb_next(&it,&ts,&v)) { printf("%u,%.9g\n", ts, v); printed++; }
  stampdb_query_end(&it);
  stampdb_close(db); free(ws); return 0;
}

int main(int argc, char **argv){
  if (argc<2){ usage(); return 1; }
  if (strcmp(argv[1],"export")==0) return cmd_export(argc,argv);
  if (strcmp(argv[1],"retention")==0) return cmd_retention(argc,argv);
  if (strcmp(argv[1],"info")==0) return cmd_info();
  if (strcmp(argv[1],"ingest")==0) return cmd_ingest(argc,argv);
  if (strcmp(argv[1],"reset")==0) return cmd_reset();
  if (strcmp(argv[1],"peek")==0) return cmd_peek();
  if (strcmp(argv[1],"dump")==0) return cmd_dump();
  if (strcmp(argv[1],"hello")==0) return cmd_hello();
  usage(); return 1;
}
