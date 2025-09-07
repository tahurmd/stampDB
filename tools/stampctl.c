/**
 * @file stampctl.c
 * @brief Command-line tools for exporting StampDB data and retention estimation.
 *
 * What it owns:
 *  - `export` subcommand (CSV/NDJSON by time range)
 *  - `retention` calculator (rough capacity estimator)
 */
#include "stampdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Print brief usage. */
static void usage(void){
  fprintf(stderr, "Usage: stampctl export --series S --t0 ms --t1 ms [--csv|--ndjson]\n");
  fprintf(stderr, "       stampctl retention --days D\n");
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

int main(int argc, char **argv){
  if (argc<2){ usage(); return 1; }
  if (strcmp(argv[1],"export")==0) return cmd_export(argc,argv);
  if (strcmp(argv[1],"retention")==0) return cmd_retention(argc,argv);
  usage(); return 1;
}
