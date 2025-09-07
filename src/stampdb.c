/**
 * @file stampdb.c
 * @brief Public API implementation: open/close, write/flush, snapshot/info.
 *
 * What it owns:
 *  - Workspace management and writer block builder (bias/scale, Fixed16)
 *  - Epoch wrap tracking and commit policy
 *
 * Role in system:
 *  - Bridges API calls to ring/codec/platform primitives
 */
#include "stampdb_internal.h"
#include <string.h>
#include <math.h>

/**
 * @brief Bump-pointer allocator inside the user-provided workspace.
 */
static void* ws_alloc(stampdb_state_t *s, size_t sz, size_t align){
  uintptr_t cur = (uintptr_t)s->ws_cur;
  uintptr_t aligned = (cur + (align-1)) & ~(uintptr_t)(align-1);
  if (aligned + sz > (uintptr_t)s->ws_begin + s->ws_size) return NULL;
  s->ws_cur = (uint8_t*)(aligned + sz);
  return (void*)aligned;
}

/** @brief Initialize a new block builder for a series starting at ts. */
static void begin_block(stampdb_state_t *s, uint16_t series, uint32_t ts, float val){
  s->cur_series = series; s->cur_t0 = ts; s->last_ts = ts; s->cur_count=0; s->cur_min = val; s->cur_max = val; s->cur_dt_bits = 8; // start optimistic
}

/**
 * @brief Close current block: quantize values, choose delta lane, encode and publish.
 *
 * Steps:
 *  1) Compute bias/scale and quantize to int16
 *  2) Pick dt_bits by max delta
 *  3) Encode payload + header and publish via ring_write_block()
 */
static void finalize_and_write_block(stampdb_state_t *s){
  if (s->cur_count==0) return;
  // compute bias/scale
  float minv = s->cur_min, maxv = s->cur_max;
  if (maxv < minv) maxv = minv;
  float scale = (maxv - minv) / 65535.0f; if (scale == 0) scale = 1e-9f;
  float bias = 0.5f*(maxv + minv);
  // quantize
  for (uint16_t i=0;i<s->cur_count;i++){
    float v = s->stg_vals[i];
    float qf = roundf((v - bias)/scale);
    if (qf < -32768.0f) qf = -32768.0f; if (qf > 32767.0f) qf = 32767.0f;
    s->stg_qvals[i] = (int16_t)qf;
  }
  // choose dt_bits by max delta
  uint32_t max_dt = 0; for (uint16_t i=0;i<s->cur_count;i++){ if (s->stg_deltas[i] > max_dt) max_dt = s->stg_deltas[i]; }
  uint8_t dt_bits = (max_dt <= 255)? 8 : 16; s->cur_dt_bits = dt_bits;
  // encode payload
  uint8_t payload[STAMPDB_PAYLOAD_BYTES];
  memset(payload, 0xFF, sizeof(payload));
  codec_encode_payload(payload, dt_bits, s->stg_deltas, s->stg_qvals, s->cur_count);
  // header
  block_header_t h; memset(&h,0,sizeof(h));
  h.series = s->cur_series; h.count = s->cur_count; h.t0_ms = s->cur_t0; h.dt_bits = dt_bits; h.bias = bias; h.scale = scale;
  h.payload_crc = crc32c(payload, STAMPDB_PAYLOAD_BYTES);
  ring_write_block(s, &h, payload);
  s->cur_count=0;
}

/**
 * @brief Add a sample to builder; auto-closes block if payload budget exceeded.
 */
static void push_sample(stampdb_state_t *s, uint16_t series, uint32_t ts, float val){
  // if new series or overflow, finalize current block
  if (s->cur_count>0 && (series != s->cur_series)) finalize_and_write_block(s);
  if (s->cur_count==0) begin_block(s, series, ts, val);

  // compute dt
  uint32_t dt = (s->cur_count==0)? 0 : (ts - s->last_ts);
  // estimate space usage if we add this row
  uint16_t cur = s->cur_count;
  uint8_t dt_bits_est = s->cur_dt_bits;
  if (dt > 255) dt_bits_est = 16;
  size_t payload_used = (size_t)((dt_bits_est==8)?(cur+1):((cur+1)*2)) + (size_t)((cur+1)*2);
  if (payload_used > STAMPDB_PAYLOAD_BYTES){
    finalize_and_write_block(s);
    begin_block(s, series, ts, val);
    cur = 0; dt = 0; dt_bits_est=8;
  }
  // append
  if (cur==0) s->stg_deltas[0]=0; else s->stg_deltas[cur]=dt;
  s->stg_vals[cur]=val;
  if (val < s->cur_min) s->cur_min = val; if (val > s->cur_max) s->cur_max = val;
  s->cur_count++;
  s->last_ts = ts;
}

// Platform abstraction for host sim is defined in sim/platform_sim.c

/**
 * @brief Open DB and recover ring state; allocate buffers in workspace.
 */
stampdb_rc stampdb_open(stampdb_t **db, const stampdb_cfg_t *cfg){
  if (!db || !cfg || !cfg->workspace || cfg->workspace_bytes < 4096) return STAMPDB_EINVAL;
  stampdb_t *inst = (stampdb_t*)cfg->workspace; // place control in workspace
  memset(inst, 0, sizeof(*inst));
  stampdb_state_t *s=&inst->s;
  s->ws_begin = (uint8_t*)cfg->workspace;
  s->ws_size = cfg->workspace_bytes;
  s->ws_cur = (uint8_t*)cfg->workspace + sizeof(*inst);
  s->read_batch_rows = cfg->read_batch_rows ? cfg->read_batch_rows : 256;
  s->commit_interval_ms = cfg->commit_interval_ms;

  // staging buffers sized for max rows
  s->stg_deltas = (uint32_t*)ws_alloc(s, sizeof(uint32_t)*74, _Alignof(uint32_t));
  s->stg_qvals  = (int16_t*) ws_alloc(s, sizeof(int16_t)*74, _Alignof(int16_t));
  s->stg_vals   = (float*)   ws_alloc(s, sizeof(float)*74, _Alignof(float));
  if (!s->stg_deltas || !s->stg_qvals || !s->stg_vals) return STAMPDB_EINVAL;

  // Recovery: try A/B snapshot, else scan
  stampdb_snapshot_t snap; stampdb_snapshot_t *snap_ptr = NULL;
  if (meta_load_snapshot(&snap)==0){ snap_ptr = &snap; }
  if (ring_scan_and_recover(s, snap_ptr) != 0) return STAMPDB_EINVAL;

  *db = inst;
  return STAMPDB_OK;
}

/** @brief Close DB; storage state remains intact. */
void stampdb_close(stampdb_t *db){ (void)db; }

/**
 * @brief Append a single sample; may trigger GC and/or finalize blocks.
 */
stampdb_rc stampdb_write(stampdb_t *db, uint16_t series, uint32_t ts_ms, float value){
  if (!db || series>=STAMPDB_MAX_SERIES) return STAMPDB_EINVAL;
  stampdb_state_t *s=&db->s;
  // retention/GC
  int rc = ring_gc_reclaim_if_needed(s, false);
  if (rc==STAMPDB_EBUSY) return STAMPDB_EBUSY;

  // epoch wrap detection: increment epoch if ts wraps by more than half range
  if (s->blocks_written>0){
    if (ts_ms < s->last_ts_observed && (s->last_ts_observed - ts_ms) > 0x80000000u){
      s->epoch_id++;
    }
  }
  s->last_ts_observed = ts_ms;

  push_sample(s, series, ts_ms, value);

  // commit by size only if commit_interval_ms==0 or on block close
  if (s->cur_count>=74) finalize_and_write_block(s);
  return STAMPDB_OK;
}

/** @brief Force publish of current block. */
stampdb_rc stampdb_flush(stampdb_t *db){
  if (!db) return STAMPDB_EINVAL;
  stampdb_state_t *s=&db->s;
  finalize_and_write_block(s);
  return STAMPDB_OK;
}

/** @brief Persist A/B snapshot (head/tail/epoch). */
stampdb_rc stampdb_snapshot_save(stampdb_t *db){
  if (!db) return STAMPDB_EINVAL;
  stampdb_state_t *s=&db->s;
  stampdb_snapshot_t snap={0};
  snap.version = 1;
  snap.epoch_id = s->epoch_id;
  snap.seg_seq_head = s->head.seg_seqno;
  // choose tail as oldest valid seg
  uint32_t oldest = 0xFFFFFFFFu; for (uint32_t i=0;i<s->seg_count;i++) if (s->segs[i].valid && s->segs[i].block_count>0 && s->segs[i].seg_seqno<oldest) oldest=s->segs[i].seg_seqno;
  if (oldest==0xFFFFFFFFu) oldest=s->head.seg_seqno;
  snap.seg_seq_tail = oldest;
  snap.head_addr = s->head.addr;
  snap.crc = 0; snap.crc = crc32c(&snap, sizeof(snap));
  if (meta_save_snapshot(&snap)!=0) return STAMPDB_EIO;
  return STAMPDB_OK;
}

/** @brief Populate lightweight stats; pointers may be NULL. */
void stampdb_info(stampdb_t *db, stampdb_stats_t* out){
  if (!db || !out) return; 
  stampdb_state_t *s=&db->s; 
  out->seg_seq_head=s->head.seg_seqno; 
  out->seg_seq_tail=s->tail_seqno; 
  out->blocks_written=s->blocks_written; 
  out->crc_errors=s->crc_errors; 
  out->gc_warn_events=s->gc_warn_events; 
  out->gc_busy_events=s->gc_busy_events; 
  out->recovery_truncations=s->recovery_truncations; 
}
