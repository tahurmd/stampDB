/**
 * @file main.c
 * @brief Pico firmware: Core0 serial bridge, Core1 DB runner via multicore FIFO.
 *
 * What it owns:
 *  - FIFO protocol (write/flush/snapshot/latest/export)
 *  - USB-serial command parser emitting lines to stdout (CDC)
 *
 * Role in system:
 *  - Demonstration firmware used to validate the Pico build and interact via USB
 */
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "stampdb.h"

// Dual-core split: Core1 runs DB; Core0 sends commands via FIFO (SPSC)
// FIFO protocol (each command is 4 words):
// w0=cmd (1=write,2=flush,3=snapshot,4=close,5=latest,6=export), w1=series (u16) in low 16 bits, w2=ts_ms (u32), w3=value (u32 raw float bits)
// For cmd=5 (latest), Core1 replies with 3 words: resp_tag=0xDEAD0005, ts_ms, val_bits

static uint8_t ws[64*1024];

/**
 * @brief Core1 entry: owns the DB and processes FIFO commands from Core0.
 *
 * Commands:
 *  - 1: write(series, ts, value)
 *  - 2: flush
 *  - 3: snapshot
 *  - 4: close
 *  - 5: latest(series) → reply (tag, ts, val_bits)
 *  - 6: export(series, t0, t1) → print CSV lines + END
 */
static void core1_entry(void){
  stampdb_t *db=NULL;
  stampdb_cfg_t cfg={.workspace=ws,.workspace_bytes=sizeof(ws),.read_batch_rows=256,.commit_interval_ms=0};
  if (stampdb_open(&db,&cfg)!=STAMPDB_OK){ for(;;) tight_loop_contents(); }
  for(;;){
    uint32_t cmd = multicore_fifo_pop_blocking();
    uint32_t w1 = multicore_fifo_pop_blocking();
    uint32_t w2 = multicore_fifo_pop_blocking();
    uint32_t w3 = multicore_fifo_pop_blocking();
    if (cmd==1){
      uint16_t series = (uint16_t)(w1 & 0xFFFFu);
      uint32_t ts = w2;
      float val; memcpy(&val,&w3,sizeof(val));
      stampdb_write(db, series, ts, val);
    } else if (cmd==2){
      stampdb_flush(db);
    } else if (cmd==3){
      stampdb_snapshot_save(db);
    } else if (cmd==4){
      stampdb_close(db); db=NULL; break;
    } else if (cmd==5){
      uint16_t series = (uint16_t)(w1 & 0xFFFFu);
      uint32_t ts=0; float v=0.0f; stampdb_query_latest(db, series, &ts, &v);
      uint32_t vb; memcpy(&vb,&v,sizeof(v));
      multicore_fifo_push_blocking(0xDEAD0005u);
      multicore_fifo_push_blocking(ts);
      multicore_fifo_push_blocking(vb);
    } else if (cmd==6){
      uint16_t series = (uint16_t)(w1 & 0xFFFFu);
      uint32_t t0 = w2; uint32_t t1 = w3;
      stampdb_it_t it; if (stampdb_query_begin(db, series, t0, t1, &it)==STAMPDB_OK){
        uint32_t ts; float val;
        while (stampdb_next(&it,&ts,&val)){
          printf("%u,%.9g\n", (unsigned)ts, (double)val);
        }
        stampdb_query_end(&it);
      }
      printf("END\n");
    }
  }
}

/** @brief Core0 helper: enqueue a write command to Core1. */
static inline void send_write(uint16_t series, uint32_t ts, float v){
  uint32_t cmd=1, w1=series, w2=ts, w3; memcpy(&w3,&v,sizeof(v));
  multicore_fifo_push_blocking(cmd); multicore_fifo_push_blocking(w1); multicore_fifo_push_blocking(w2); multicore_fifo_push_blocking(w3);
}

/** @brief Read a line from USB CDC without blocking forever. */
static bool read_line(char *buf, int maxlen){
  int n=0;
  while (n<maxlen-1){
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT){ sleep_ms(1); continue; }
    if (ch == '\r') continue;
    if (ch == '\n'){ buf[n]='\0'; return true; }
    buf[n++] = (char)ch;
  }
  buf[n]='\0'; return n>0;
}

/** @brief Core0: parse simple commands and forward to Core1; print responses. */
int main(void){
  stdio_init_all();
  multicore_launch_core1(core1_entry);
  char line[128];
  while (true){
    if (!read_line(line, sizeof(line))) continue;
    if (line[0]=='w'){
      unsigned s=0; unsigned ts=0; float v=0;
      if (sscanf(line, "w %u %u %f", &s, &ts, &v)==3){ send_write((uint16_t)s, (uint32_t)ts, v); puts("OK"); }
      else { puts("ERR"); }
    } else if (line[0]=='f'){
      multicore_fifo_push_blocking(2); multicore_fifo_push_blocking(0); multicore_fifo_push_blocking(0); multicore_fifo_push_blocking(0); puts("OK");
    } else if (line[0]=='s'){
      multicore_fifo_push_blocking(3); multicore_fifo_push_blocking(0); multicore_fifo_push_blocking(0); multicore_fifo_push_blocking(0); puts("OK");
    } else if (line[0]=='l'){
      unsigned s=0; if (sscanf(line, "l %u", &s)==1){
        multicore_fifo_push_blocking(5); multicore_fifo_push_blocking(s); multicore_fifo_push_blocking(0); multicore_fifo_push_blocking(0);
        uint32_t tag=0; do { tag = multicore_fifo_pop_blocking(); } while (tag != 0xDEAD0005u);
        uint32_t ts = multicore_fifo_pop_blocking(); uint32_t vb = multicore_fifo_pop_blocking(); float v; memcpy(&v,&vb,sizeof(v));
        printf("OK %u %f\n", (unsigned)ts, (double)v);
      } else { puts("ERR"); }
    } else if (line[0]=='e'){
      unsigned s=0; unsigned t0=0; unsigned t1=0; if (sscanf(line, "e %u %u %u", &s, &t0, &t1)==3){
        multicore_fifo_push_blocking(6); multicore_fifo_push_blocking(s); multicore_fifo_push_blocking(t0); multicore_fifo_push_blocking(t1);
      } else { puts("ERR"); }
    } else {
      puts("ERR");
    }
  }
}
