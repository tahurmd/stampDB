/**
 * @file stampdb.h
 * @brief Public C11 API for StampDB time-series storage.
 *
 * What it owns:
 *  - Opaque handle `stampdb_t` and public config/stat/iterator types
 *  - Append, flush, query, latest, snapshot, and info functions
 *
 * Role in system:
 *  - Stable ABI used by host CLI/tests/Python and Pico firmware
 *
 * Constraints:
 *  - No heap after open; all state in caller-provided workspace
 *  - Timestamps are u32 ms and may wrap; iterators are wrap-aware
 */
// StampDB public API (C11)
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct stampdb stampdb_t;

/** @brief Result codes returned by API calls. */
typedef enum {
  STAMPDB_OK=0,
  STAMPDB_EINVAL,
  STAMPDB_EBUSY,
  STAMPDB_ENOSPACE,
  STAMPDB_ECRC,
  STAMPDB_EIO
} stampdb_rc;

/**
 * @brief Open configuration. All memory comes from `workspace`.
 *
 * - workspace: backing memory (static or heap) owned by caller
 * - workspace_bytes: size of workspace; enforced cap
 * - read_batch_rows: 256/512 typical; affects iterator buffering only
 * - commit_interval_ms: advisory cadence (0 = size-only)
 */
typedef struct {
  void*    workspace;        // pre-allocated
  uint32_t workspace_bytes;  // hard cap, checked at open
  uint32_t read_batch_rows;  // 256/512
  uint32_t commit_interval_ms; // 0=size-only
} stampdb_cfg_t;

/**
 * @brief Open a database instance, scanning storage and rebuilding summaries.
 * @return STAMPDB_OK on success, *_E* on validation/recovery failure.
 */
stampdb_rc stampdb_open(stampdb_t **db, const stampdb_cfg_t *cfg);
/** @brief Close and release in-memory state. Storage remains intact. */
void       stampdb_close(stampdb_t *db);

/**
 * @brief Append a single (series, ts_ms, value) sample.
 * - series: 0..255
 * - ts_ms: u32 ms (wraps); ordering within block is maintained by caller
 */
stampdb_rc stampdb_write(stampdb_t *db, uint16_t series, uint32_t ts_ms, float value);
/** @brief Force current block publish (header-last). May roll segment. */
stampdb_rc stampdb_flush(stampdb_t *db);

/**
 * @brief Query iterator storage (opaque to callers). Stack-alloc and pass by pointer.
 */
typedef struct stampdb_it {
  // Public iterator state. Content is considered private; users just stack-allocate it.
  void *s;               // internal state pointer
  uint16_t series;       // query series
  uint32_t t0, t1;       // range
  uint32_t seg_idx;      // internal
  uint32_t page_in_seg;  // internal
  uint32_t row_idx_in_block;
  uint16_t count_in_block;
  uint8_t  dt_bits;
  uint32_t t0_block;
  float    bias;
  float    scale;
  uint32_t deltas[74];
  int16_t  qvals[74];
  uint32_t times[74];
  float    values[74];
} stampdb_it_t;
/** @brief Begin a query over [t0_ms..t1_ms] for a series. */
stampdb_rc stampdb_query_begin(stampdb_t *db, uint16_t series, uint32_t t0_ms, uint32_t t1_ms, stampdb_it_t *it);
/** @brief Advance iterator; returns true if a row is produced. */
bool       stampdb_next(stampdb_it_t *it, uint32_t *ts_ms, float *val);
/** @brief End a query and release any iterator-bound resources. */
void       stampdb_query_end(stampdb_it_t *it);

/** @brief Get latest row for a series. */
stampdb_rc stampdb_query_latest(stampdb_t *db, uint16_t series, uint32_t *out_ts_ms, float *out_value);
/** @brief Persist A/B snapshot with ring head/tail and epoch. */
stampdb_rc stampdb_snapshot_save(stampdb_t *db);

/**
 * @brief Lightweight stats for tests/telemetry.
 *
 * Fields:
 *  - seg_seq_head/tail: Current ring head/tail sequence numbers
 *  - blocks_written: Total blocks committed since open
 *  - crc_errors: CRC mismatches observed on read (iterator)
 *  - gc_warn_events: Entries into <10% free watermark (may count multiple times)
 *  - gc_busy_events: Entries into <5% free or GC quota busy condition
 *  - recovery_truncations: Times recovery truncated after first invalid page
 */
typedef struct {
  uint32_t seg_seq_head, seg_seq_tail, blocks_written, crc_errors;
  uint32_t gc_warn_events, gc_busy_events, recovery_truncations;
} stampdb_stats_t;
/** @brief Populate current stats into user struct. */
void       stampdb_info(stampdb_t *db, stampdb_stats_t* out);
