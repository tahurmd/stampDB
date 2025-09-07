/**
 * @file stampdb_internal.h
 * @brief Internal types, constants, and prototypes for StampDB core.
 *
 * What it owns:
 *  - Storage geometry constants, CRC, codec, ring and iterator internals
 *  - Platform glue (flash I/O, time) and metadata (snapshots/head)
 *
 * Role in system:
 *  - Shared header for src/* files and platform shims (host/pico)
 *
 * Constraints:
 *  - No external allocations; all pointers derive from `stampdb_cfg_t.workspace`
 *  - Flash ops obey 4 KiB erase / 256 B program; header-last commit ordering
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "stampdb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Storage geometry
/* Segment/page geometry as required by the SPEC. */
#define STAMPDB_SEG_BYTES   4096u
#define STAMPDB_PAGE_BYTES   256u
#define STAMPDB_PAGES_PER_SEG (STAMPDB_SEG_BYTES / STAMPDB_PAGE_BYTES)
#define STAMPDB_DATA_PAGES_PER_SEG (STAMPDB_PAGES_PER_SEG - 1u) // last page is footer
#define STAMPDB_PAYLOAD_BYTES 224u
#define STAMPDB_HEADER_BYTES   32u

#define STAMPDB_BLOCK_MAGIC 0x424C4B31u /* 'BLK1' */
#define STAMPDB_FOOTER_MAGIC 0x53464731u /* 'SFG1' */

#define STAMPDB_SERIES_BITMAP_BYTES 32u // 256-bit
#define STAMPDB_MAX_SERIES 256u
#ifndef STAMPDB_META_RESERVED
#define STAMPDB_META_RESERVED (32768u) // reserved at top of flash for snapshots + head hint (LittleFS)
#endif
#define STAMPDB_LAYOUT_VERSION 1

static inline bool ts_le(uint32_t a, uint32_t b){ return (uint32_t)(b - a) < 0x80000000u; }
static inline bool ts_ge(uint32_t a, uint32_t b){ return ts_le(b,a); }
static inline bool ts_in_range(uint32_t t, uint32_t t0, uint32_t t1){
  if (ts_le(t0, t1)) return ts_le(t0, t) && ts_le(t, t1);
  // wrapped window
  return ts_le(t0, t) || ts_le(t, t1);
}

/** @brief Compute CRC32C (Castagnoli) over a buffer. */
uint32_t crc32c(const void *data, size_t len);

/* Platform glue for clock and flash I/O (host/pico). */
uint64_t platform_millis(void);
int platform_flash_read(uint32_t addr, void *dst, size_t len);
int platform_flash_erase_4k(uint32_t addr);
int platform_flash_program_256(uint32_t addr, const void *src);
uint32_t platform_flash_size_bytes(void);

// Meta store (LittleFS / host-files)
typedef struct {
  uint32_t version;
  uint32_t epoch_id;
  uint32_t seg_seq_head;
  uint32_t seg_seq_tail;
  uint32_t head_addr; // absolute address to next page
  uint32_t crc;
} stampdb_snapshot_t;

int meta_load_snapshot(stampdb_snapshot_t *out);
int meta_save_snapshot(const stampdb_snapshot_t *snap);
int meta_load_head_hint(uint32_t *addr_out, uint32_t *seq_out);
int meta_save_head_hint(uint32_t addr, uint32_t seq);

/* Block header used for publish (payload CRC, header CRC). */
typedef struct {
  // header fields
  uint16_t series;
  uint16_t count;
  uint32_t t0_ms;
  uint8_t  dt_bits; // 8 or 16
  float    bias;
  float    scale;
  uint32_t payload_crc;
  uint32_t header_crc;
} block_header_t;

/** @brief Encode deltas+qvals into 224B payload; zero-fills remainder. */
size_t codec_encode_payload(uint8_t *dst224, uint8_t dt_bits, const uint32_t *ts_deltas, const int16_t *qvals, uint16_t count);
/** @brief Decode payload into caller buffers (deltas then qvals). */
size_t codec_decode_payload(const uint8_t *src224, uint8_t dt_bits, uint32_t *ts_deltas, int16_t *qvals, uint16_t count);
/** @brief Serialize header (includes header CRC over bytes 0..27). */
void   codec_pack_header(uint8_t out32[STAMPDB_HEADER_BYTES], const block_header_t *h);
/** @brief Parse header and verify header CRC. */
bool   codec_unpack_header(block_header_t *h, const uint8_t in32[STAMPDB_HEADER_BYTES]);

// Ring manager
typedef struct {
  uint32_t magic;
  uint32_t seg_seqno;
  uint32_t t_min;
  uint32_t t_max;
  uint32_t block_count;
  uint8_t  series_bitmap[STAMPDB_SERIES_BITMAP_BYTES];
  uint32_t crc;
} seg_footer_t;

typedef struct {
  uint32_t addr; // absolute addr in flash to next free page start
  uint16_t page_index; // within current segment [0..15)
  uint32_t seg_seqno;  // current segment sequence
} ring_head_t;

typedef struct {
  uint32_t addr_first; // first page addr of seg
  uint32_t seg_seqno;
  uint32_t t_min;
  uint32_t t_max;
  uint32_t block_count;
  uint8_t  series_bitmap[STAMPDB_SERIES_BITMAP_BYTES];
  bool     valid;
} seg_summary_t;

typedef struct {
  // workspace-backed containers
  uint8_t *ws_begin;
  uint32_t ws_size;
  uint8_t *ws_cur;

  // zone map cache of all segments (constant RAM)
  seg_summary_t *segs;
  uint32_t seg_count;

  // ring head/tail
  ring_head_t head;
  uint32_t tail_seqno;

  // write buffer for building blocks
  uint16_t cur_series;
  uint32_t cur_t0;
  uint8_t  cur_dt_bits;
  float    cur_min;
  float    cur_max;
  uint16_t cur_count;
  uint32_t last_ts;
  uint32_t last_hint_ms;
  uint32_t last_ts_observed;

  uint32_t blocks_written;
  uint32_t crc_errors;
  uint32_t epoch_id;
  uint32_t gc_warn_events;
  uint32_t gc_busy_events;
  uint32_t recovery_truncations;

  // staging arrays (in workspace)
  uint32_t *stg_deltas;  // up to 74/56
  int16_t  *stg_qvals;
  float    *stg_vals;

  uint32_t read_batch_rows;
  uint32_t commit_interval_ms;
} stampdb_state_t;

struct stampdb { stampdb_state_t s; };

/* Recovery / scanning core. */
int ring_scan_and_recover(stampdb_state_t *s, const stampdb_snapshot_t *snap_opt);
int ring_write_block(stampdb_state_t *s, const block_header_t *h, const uint8_t payload[STAMPDB_PAYLOAD_BYTES]);
int ring_finalize_segment_and_rotate(stampdb_state_t *s);
int ring_gc_reclaim_if_needed(stampdb_state_t *s, bool non_blocking);

/* Query iterator internal definition (public form in stampdb.h). */
typedef struct stampdb_it stampdb_it_t; // defined in public header

#ifdef __cplusplus
}
#endif
