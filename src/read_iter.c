/**
 * @file read_iter.c
 * @brief Range iterator over CRC-verified blocks with zone-map skipping.
 *
 * What it owns:
 *  - Iterator begin/next/end and latest lookup
 *
 * Role in system:
 *  - Streams results in constant RAM (SoA decode per block)
 */
#include "stampdb_internal.h"
#include <string.h>

static bool bitmap_has(const uint8_t *bm, uint16_t series){ return (bm[series>>3] & (1u<<(series&7))) != 0; }

/** @brief Initialize an iterator over [t0_ms..t1_ms] for a series. */
stampdb_rc stampdb_query_begin(stampdb_t *db, uint16_t series, uint32_t t0_ms, uint32_t t1_ms, stampdb_it_t *it){
  if (!db || !it) return STAMPDB_EINVAL;
  stampdb_state_t *s=&db->s;
  memset(it, 0, sizeof(*it));
  it->s = s; it->series = series; it->t0 = t0_ms; it->t1 = t1_ms;
  it->seg_idx = 0; it->page_in_seg = 0; it->row_idx_in_block = 0; it->count_in_block = 0;
  return STAMPDB_OK;
}

/**
 * @brief Internal: load next matching block into iterator buffers.
 *
 * Notes:
 *  - Uses zone-map (t_min,t_max)+series bitmap to skip irrelevant segments
 *  - Verifies header and payload CRC before decoding
 */
static bool load_next_block(stampdb_it_t *it){
  stampdb_state_t *s = it->s;
  uint64_t visited_pages = 0;
  const uint64_t max_pages = (uint64_t)s->seg_count * (uint64_t)STAMPDB_DATA_PAGES_PER_SEG;
  while (it->seg_idx < s->seg_count){
    seg_summary_t *sm = &s->segs[it->seg_idx];
    // --- Zone-map skip (wrap-aware) ----------------------------------------
    if (!sm->valid || sm->block_count==0 || !bitmap_has(sm->series_bitmap, it->series)) { it->seg_idx++; it->page_in_seg=0; continue; }
    // If entire seg time window outside query window, skip
    // We treat overlap if either sm->t_min..sm->t_max intersects it->t0..it->t1 under wrap semantics
    bool overlap = ts_in_range(sm->t_min, it->t0, it->t1) || ts_in_range(sm->t_max, it->t0, it->t1) || ts_in_range(it->t0, sm->t_min, sm->t_max);
    if (!overlap){
      it->seg_idx++; it->page_in_seg=0; continue;
    }
    // scan pages within seg
    while (it->page_in_seg < STAMPDB_DATA_PAGES_PER_SEG){
      if (++visited_pages > (max_pages + 1)) { return false; }
      uint32_t addr = sm->addr_first + it->page_in_seg*STAMPDB_PAGE_BYTES;
      block_header_t h; uint8_t page[STAMPDB_PAGE_BYTES];
      if (platform_flash_read(addr, page, STAMPDB_PAGE_BYTES)!=0) { it->seg_idx++; it->page_in_seg=0; break; }
      const uint8_t *payload = page;
      const uint8_t *hdr = page + STAMPDB_PAYLOAD_BYTES;
      if (!codec_unpack_header(&h, hdr)) { it->seg_idx++; it->page_in_seg=0; break; }
      it->page_in_seg++;
      if (h.series != it->series) continue; // skip CRC for non-target series
      if (crc32c(payload, STAMPDB_PAYLOAD_BYTES) != h.payload_crc){ s->crc_errors++; it->seg_idx++; it->page_in_seg=0; break; }
      it->count_in_block = h.count;
      it->dt_bits = h.dt_bits;
      it->t0_block = h.t0_ms;
      it->bias = h.bias; it->scale = h.scale;
      codec_decode_payload(payload, h.dt_bits, it->deltas, it->qvals, h.count);
      // reconstruct times and values
      uint32_t t = h.t0_ms; for (uint16_t i=0;i<h.count;i++){ t += it->deltas[i]; it->times[i]=t; it->values[i] = it->bias + it->scale * (float)it->qvals[i]; }
      it->row_idx_in_block = 0;
      return true;
    }
    // reached end of segment without finding a matching/valid block; advance to next segment
    it->seg_idx++;
    it->page_in_seg = 0;
  }
  return false;
}

/** @brief Step iterator and deliver next row in range; false when exhausted. */
bool stampdb_next(stampdb_it_t *it, uint32_t *ts_ms, float *val){
  while (1){
    if (it->row_idx_in_block < it->count_in_block){
      uint32_t t = it->times[it->row_idx_in_block];
      float v    = it->values[it->row_idx_in_block];
      it->row_idx_in_block++;
      if (t < it->t0 || t > it->t1) continue;
      if (ts_ms) *ts_ms = t; if (val) *val = v; return true;
    }
    if (!load_next_block(it)) return false;
  }
}

/** @brief End iterator; currently a no-op (reserved for future). */
void stampdb_query_end(stampdb_it_t *it){ (void)it; }

/**
 * @brief Find the latest row for a series by scanning newest segments backwards.
 */
stampdb_rc stampdb_query_latest(stampdb_t *db, uint16_t series, uint32_t *out_ts_ms, float *out_value){
  if (!db) return STAMPDB_EINVAL;
  stampdb_state_t *s = &db->s;
  // find the newest block for series
  int32_t best_seg = -1; int32_t best_page = -1; block_header_t best_h;
  for (int32_t seg=(int32_t)s->seg_count-1; seg>=0; --seg){
    seg_summary_t *sm=&s->segs[seg];
    if (!sm->valid || sm->block_count==0 || !bitmap_has(sm->series_bitmap, series)) continue;
    for (int32_t p=(int32_t)STAMPDB_DATA_PAGES_PER_SEG-1; p>=0; --p){
      uint32_t addr = sm->addr_first + (uint32_t)p*STAMPDB_PAGE_BYTES;
      uint8_t hdr[STAMPDB_HEADER_BYTES];
      platform_flash_read(addr+STAMPDB_PAYLOAD_BYTES, hdr, STAMPDB_HEADER_BYTES);
      block_header_t h; if (!codec_unpack_header(&h, hdr)) continue;
      if (h.series != series) continue;
      best_seg = seg; best_page = p; best_h = h; break;
    }
    if (best_seg>=0) break;
  }
  if (best_seg<0) return STAMPDB_EINVAL;
  // get last row in that block
  uint8_t payload[STAMPDB_PAYLOAD_BYTES];
  uint32_t addr = s->segs[best_seg].addr_first + (uint32_t)best_page*STAMPDB_PAGE_BYTES;
  platform_flash_read(addr, payload, STAMPDB_PAYLOAD_BYTES);
  uint32_t t = best_h.t0_ms;
  if (best_h.dt_bits==8){ const uint8_t *pr=payload; for (uint16_t i=0;i<best_h.count;i++) t += *pr++; }
  else { const uint8_t *pr=payload; for (uint16_t i=0;i<best_h.count;i++){ t += (uint16_t)(pr[0]|(pr[1]<<8)); pr+=2; } }
  int16_t q = 0;
  // load last qval
  const uint8_t *qptr = payload + ((best_h.dt_bits==8)?best_h.count:(best_h.count*2));
  q = (int16_t)(qptr[(best_h.count-1)*2] | (qptr[(best_h.count-1)*2+1]<<8));
  float v = best_h.bias + best_h.scale * (float)q;
  if (out_ts_ms) *out_ts_ms = t;
/**
 * Hard cap for page scans per iterator call to avoid unbounded loops under
 * unexpected corruption: seg_count * data_pages_per_segment + 1.
 */
  if (out_value) *out_value = v;
  return STAMPDB_OK;
}
