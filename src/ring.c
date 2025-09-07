/**
 * @file ring.c
 * @brief Segment ring management: write, finalize, GC, and recovery scanning.
 *
 * What it owns:
 *  - Segment footer I/O (CRC-protected), head/tail movement
 *  - Header-last publish; block write path; GC quota
 *
 * Role in system:
 *  - Central storage orchestrator used by writer, recovery, and iterator
 *
 * Constraints:
 *  - 4 KiB erase / 256 B program alignment; NOR 1→0 programming
 *  - Accepts only CRC-clean pages during recovery; truncates at first invalid
 */
#include "stampdb_internal.h"
#include <string.h>

static inline uint32_t align_down(uint32_t x, uint32_t a){return x - (x % a);} 

/**
 * @brief Read and validate a segment footer at the last page of a segment.
 *
 * Inputs:
 *  - seg_base: Base address of segment (aligned to 4 KiB)
 *  - out: Filled on success
 *
 * Returns: 0 on success; -1 on invalid/missing footer
 */
static int read_footer(uint32_t seg_base, seg_footer_t *out){
  uint8_t page[STAMPDB_PAGE_BYTES];
  if (platform_flash_read(seg_base + (STAMPDB_PAGES_PER_SEG-1)*STAMPDB_PAGE_BYTES, page, sizeof(page)) != 0) return -1;
  uint32_t magic = (uint32_t)page[0] | ((uint32_t)page[1]<<8) | ((uint32_t)page[2]<<16) | ((uint32_t)page[3]<<24);
  if (magic != STAMPDB_FOOTER_MAGIC) return -1;
  memcpy(out, page, sizeof(seg_footer_t));
  // verify crc
  uint32_t crc = out->crc;
  out->crc = 0;
  uint32_t calc = crc32c(out, sizeof(seg_footer_t));
  out->crc = crc;
  if (crc != calc) return -1;
  return 0;
}

/**
 * @brief Write a segment footer (payload is page-sized; CRC over struct).
 *
 * Steps:
 *  1) Build footer with magic and CRC
 *  2) Program last page of the segment
 */
static int write_footer(uint32_t seg_base, const seg_footer_t *footer){
  uint8_t page[STAMPDB_PAGE_BYTES];
  memset(page, 0xFF, sizeof(page));
  seg_footer_t tmp = *footer;
  tmp.magic = STAMPDB_FOOTER_MAGIC;
  tmp.crc = 0;
  uint32_t crc = crc32c(&tmp, sizeof(tmp));
  tmp.crc = crc;
  memcpy(page, &tmp, sizeof(tmp));
  return platform_flash_program_256(seg_base + (STAMPDB_PAGES_PER_SEG-1)*STAMPDB_PAGE_BYTES, page);
}

/**
 * @brief Read a full page (payload+header) and verify header and payload CRC.
 * @return 0 on OK; -1 on header error; -2 on payload CRC error.
 */
static int read_block(uint32_t page_addr, block_header_t *hout, uint8_t payload[STAMPDB_PAYLOAD_BYTES]){
  uint8_t page[STAMPDB_PAGE_BYTES];
  if (platform_flash_read(page_addr, page, sizeof(page))!=0) return -1;
  // split
  memcpy(payload, page, STAMPDB_PAYLOAD_BYTES);
  uint8_t hdr[STAMPDB_HEADER_BYTES];
  memcpy(hdr, page+STAMPDB_PAYLOAD_BYTES, STAMPDB_HEADER_BYTES);
  if (!codec_unpack_header(hout, hdr)) return -1;
  uint32_t calc = crc32c(payload, STAMPDB_PAYLOAD_BYTES);
  if (calc != hout->payload_crc) return -2;
  return 0;
}

/**
 * @brief Recovery entry: rebuild zone map and locate ring head, truncating torn tails.
 *
 * Inputs:
 *  - s: DB state (workspace cursors and config pre-initialized)
 *  - snap_opt: Optional trusted snapshot to seed head/tail/epoch
 *
 * Steps (high level):
 *  1) Scan segment footers to build zone map summary
 *  2) Use snapshot and/or head hint if present
 *  3) Probe tail segment to find first free page; truncate after first invalid
 */
int ring_scan_and_recover(stampdb_state_t *s, const stampdb_snapshot_t *snap_opt){
  // Build zone map by scanning footers; fallback to deep scan if missing
  uint32_t flash_bytes = platform_flash_size_bytes();
  uint32_t usable_bytes = (flash_bytes > STAMPDB_META_RESERVED) ? (flash_bytes - STAMPDB_META_RESERVED) : flash_bytes;
  s->seg_count = usable_bytes / STAMPDB_SEG_BYTES;
  size_t need = sizeof(seg_summary_t) * (size_t)s->seg_count;
  uintptr_t cur = (uintptr_t)s->ws_cur;
  uintptr_t end = (uintptr_t)s->ws_begin + s->ws_size;
  if (cur + need > end) return -1; // insufficient workspace
  s->segs = (seg_summary_t*)s->ws_cur;
  s->ws_cur += need;
  for (uint32_t i=0;i<s->seg_count;i++){
    s->segs[i].valid=false;
  }

  bool any=false;
  for (uint32_t i=0;i<s->seg_count;i++){
    uint32_t base = i*STAMPDB_SEG_BYTES;
    seg_footer_t f;
    if (read_footer(base, &f)==0){
      seg_summary_t *sm=&s->segs[i];
      sm->addr_first = base;
      sm->seg_seqno = f.seg_seqno;
      sm->t_min = f.t_min;
      sm->t_max = f.t_max;
      sm->block_count = f.block_count;
      memcpy(sm->series_bitmap, f.series_bitmap, STAMPDB_SERIES_BITMAP_BYTES);
      sm->valid=true; any=true;
    }
  }

  if (snap_opt){
    // trust snapshot head, but probe tail to ensure consistency
    s->head.addr = snap_opt->head_addr;
    s->head.page_index = ((s->head.addr % STAMPDB_SEG_BYTES)/STAMPDB_PAGE_BYTES);
    s->head.seg_seqno = snap_opt->seg_seq_head;
    s->tail_seqno = snap_opt->seg_seq_tail;
    s->epoch_id = snap_opt->epoch_id;
  } else {
    // Try head hint first
    uint32_t hint_addr=0, hint_seq=0;
    if (meta_load_head_hint(&hint_addr, &hint_seq)==0){
      uint32_t usable_bytes = s->seg_count * STAMPDB_SEG_BYTES;
      if (hint_addr < usable_bytes){
        s->head.addr = hint_addr;
        s->head.page_index = ((s->head.addr % STAMPDB_SEG_BYTES)/STAMPDB_PAGE_BYTES);
        s->head.seg_seqno = hint_seq;
      }
    }
    // pick head as next page after last valid block in newest seg (by footer); if none, probe seg 0 by scanning pages
    uint32_t best_i=0; uint32_t best_seq=0; any=false;
    for (uint32_t i=0;i<s->seg_count;i++) if (s->segs[i].valid && (!any || s->segs[i].seg_seqno > best_seq)){ any=true; best_seq=s->segs[i].seg_seqno; best_i=i; }
    if (any){
      s->head.seg_seqno = s->segs[best_i].seg_seqno + 1;
      s->head.addr = best_i*STAMPDB_SEG_BYTES; // will probe for first free page
      s->head.page_index = 0;
      s->tail_seqno = best_seq - (s->seg_count-1);
    } else {
      // No footers found: treat as possibly fresh or partially written; probe segment 0
      s->head.seg_seqno = 1;
      s->tail_seqno = 1;
      s->head.addr = 0;
      s->head.page_index = 0;
      // Build a summary for seg 0 by scanning pages
      uint32_t base = 0; seg_summary_t *sm=&s->segs[0]; memset(sm,0,sizeof(*sm)); sm->addr_first=0; sm->seg_seqno=1; sm->t_min=0xFFFFFFFFu; sm->t_max=0; sm->block_count=0; sm->valid=true;
      for (uint32_t p=0;p<STAMPDB_DATA_PAGES_PER_SEG;p++){
        block_header_t h; uint8_t payload[STAMPDB_PAYLOAD_BYTES];
        if (read_block(base + p*STAMPDB_PAGE_BYTES, &h, payload)!=0) break;
        if (h.t0_ms < sm->t_min) sm->t_min = h.t0_ms;
        uint32_t last_t = h.t0_ms;
        if (h.dt_bits==8){ const uint8_t *pr=payload; for (uint16_t i=0;i<h.count;i++) last_t += *pr++; }
        else { const uint8_t *pr=payload; for (uint16_t i=0;i<h.count;i++){ last_t += (uint16_t)(pr[0]|(pr[1]<<8)); pr+=2; } }
        if (last_t > sm->t_max) sm->t_max = last_t;
        sm->block_count++;
        sm->series_bitmap[h.series>>3] |= (1u<<(h.series&7));
      }
    }
  }

  // --- Recovery: probe tail of current head segment ------------------------
  // Scan pages forward; stop at first invalid header; keep last valid offset.
  uint32_t cur_seg_base = align_down(s->head.addr, STAMPDB_SEG_BYTES);
  uint32_t first_free_page = 0;
  bool had_valid=false; bool broke=false;
  for (uint32_t p=0;p<STAMPDB_DATA_PAGES_PER_SEG;p++){
    block_header_t h; uint8_t payload[STAMPDB_PAYLOAD_BYTES];
    int r = read_block(cur_seg_base + p*STAMPDB_PAGE_BYTES, &h, payload);
    if (r!=0){ first_free_page = p; broke=true; break; }
    had_valid=true;
    first_free_page = p+1;
  }
  if (broke && had_valid) s->recovery_truncations++;
  s->head.page_index = first_free_page;
  s->head.addr = cur_seg_base + first_free_page*STAMPDB_PAGE_BYTES;
  s->last_hint_ms = (uint32_t)platform_millis();
  return 0;
}

/**
 * @brief Seal current segment with an aggregated footer and rotate to next segment.
 *
 * Side effects:
 *  - Writes footer; erases next segment; updates zone map entry for new head
 */
int ring_finalize_segment_and_rotate(stampdb_state_t *s){
  // gather stats from the segment we are finalizing
  uint32_t base = align_down(s->head.addr, STAMPDB_SEG_BYTES);
  // compute footer by scanning this segment's data pages
  seg_footer_t f; memset(&f, 0, sizeof(f));
  f.magic = STAMPDB_FOOTER_MAGIC;
  f.seg_seqno = s->head.seg_seqno;
  f.t_min = 0xFFFFFFFFu; f.t_max=0;
  for (uint32_t p=0;p<STAMPDB_DATA_PAGES_PER_SEG;p++){
    block_header_t h; uint8_t payload[STAMPDB_PAYLOAD_BYTES];
    if (read_block(base + p*STAMPDB_PAGE_BYTES, &h, payload)!=0) break;
    if (h.t0_ms < f.t_min) f.t_min = h.t0_ms;
    uint32_t last_t = h.t0_ms;
    if (h.dt_bits==8){
      const uint8_t *pr = payload;
      for (uint16_t i=0;i<h.count;i++) last_t += *pr++;
    } else {
      const uint8_t *pr = payload;
      for (uint16_t i=0;i<h.count;i++){ last_t += (uint16_t)(pr[0]|(pr[1]<<8)); pr+=2; }
    }
    if (last_t > f.t_max) f.t_max = last_t;
    f.block_count++;
    // series bitmap
    uint32_t idx = h.series >> 3; uint8_t bit = 1u << (h.series & 7);
    f.series_bitmap[idx] |= bit;
  }
  f.crc = 0;
  f.crc = crc32c(&f, sizeof(f));
  // write footer last page
  write_footer(base, &f);

  // advance to next segment
  uint32_t next_base = (base + STAMPDB_SEG_BYTES) % (s->seg_count*STAMPDB_SEG_BYTES);
  platform_flash_erase_4k(next_base);
  s->head.seg_seqno++;
  s->head.addr = next_base;
  s->head.page_index = 0;
  // update zone map entry
  uint32_t idx = next_base / STAMPDB_SEG_BYTES;
  s->segs[idx].addr_first = next_base;
  s->segs[idx].seg_seqno = s->head.seg_seqno;
  s->segs[idx].t_min = 0xFFFFFFFFu; s->segs[idx].t_max = 0; s->segs[idx].block_count = 0; memset(s->segs[idx].series_bitmap,0,STAMPDB_SERIES_BITMAP_BYTES); s->segs[idx].valid=true;
  return 0;
}

/**
 * @brief Publish one block to flash with header-last, power-cut safe order.
 *
 * Steps:
 *  1) Program payload page body (header area left 0xFF)
 *  2) Program header (1→0 only) to atomically publish the block
 *  3) Update in-RAM zone-map summary
 */
int ring_write_block(stampdb_state_t *s, const block_header_t *h, const uint8_t payload[STAMPDB_PAYLOAD_BYTES]){
  uint32_t page_addr = s->head.addr;
  // --- Commit block (header-last, power-cut safe) --------------------------
  // 1) payload bytes (ignored if header missing)
  // 2) header at page tail (atomic publish)
  // 3) update in-RAM summaries
  uint8_t page[STAMPDB_PAGE_BYTES];
  memcpy(page, payload, STAMPDB_PAYLOAD_BYTES);
  memset(page + STAMPDB_PAYLOAD_BYTES, 0xFF, STAMPDB_HEADER_BYTES);
  int rc = platform_flash_program_256(page_addr, page);
  if (rc!=0) return -1;

  // Prepare header bytes
  uint8_t hdr[STAMPDB_HEADER_BYTES];
  codec_pack_header(hdr, h);
  // Page image #2: only header bytes (1->0), payload all 0xFF to avoid changes
  memset(page, 0xFF, sizeof(page));
  memcpy(page + STAMPDB_PAYLOAD_BYTES, hdr, STAMPDB_HEADER_BYTES);
  rc = platform_flash_program_256(page_addr, page);
  if (rc!=0) return -2;

  // advance head
  s->blocks_written++;
  s->head.page_index++;
  s->head.addr += STAMPDB_PAGE_BYTES;

  // update seg summary live (for current seg index)
  uint32_t seg_idx = (page_addr / STAMPDB_SEG_BYTES);
  seg_summary_t *sm = &s->segs[seg_idx];
  if (!sm->valid){ sm->valid=true; sm->seg_seqno=s->head.seg_seqno; sm->addr_first=seg_idx*STAMPDB_SEG_BYTES; }
  if (h->t0_ms < sm->t_min) sm->t_min = h->t0_ms;
  uint32_t last_t = h->t0_ms;
  // compute last ts
  if (h->dt_bits==8){
    const uint8_t *pr = payload;
    for (uint16_t i=0;i<h->count;i++) last_t += *pr++;
  } else {
    const uint8_t *pr = payload;
    for (uint16_t i=0;i<h->count;i++){ last_t += (uint16_t)(pr[0]|(pr[1]<<8)); pr+=2; }
  }
  if (last_t > sm->t_max) sm->t_max = last_t;
  sm->block_count++;
  sm->series_bitmap[h->series >> 3] |= (1u << (h->series & 7));

  if (s->head.page_index >= STAMPDB_DATA_PAGES_PER_SEG){
    ring_finalize_segment_and_rotate(s);
  }

  // hint update
  uint32_t now = (uint32_t)platform_millis();
  if ((s->blocks_written & 63u) == 0u || (now - s->last_hint_ms) >= 2000u){
    meta_save_head_hint(s->head.addr, s->head.seg_seqno);
    s->last_hint_ms = now;
  }

  return 0;
}

/**
 * @brief Reclaim oldest segment when free watermark <10% (busy at 5%).
 *
 * Quota: ≤2 seg/s; non_blocking returns EBUSY if quota exhausted.
 */
int ring_gc_reclaim_if_needed(stampdb_state_t *s, bool non_blocking){
  // Watermarks: warn at 10%, busy at 5%
  uint32_t used = 0;
  for (uint32_t i=0;i<s->seg_count;i++) if (s->segs[i].valid && s->segs[i].block_count>0) used++;
  uint32_t free = s->seg_count - used;
  if (free*100u < 10u*s->seg_count) s->gc_warn_events++;
  if (free*100u < 5u*s->seg_count) s->gc_busy_events++;
  if (free*100u >= 10u*s->seg_count) return 0; // plenty free

  // enforce quota ≤2 seg/s
  static uint64_t last_erase_ms = 0; static uint32_t erased_in_window = 0; static uint64_t window_start = 0;
  uint64_t now = platform_millis();
  if (now - window_start >= 1000){ window_start = now; erased_in_window = 0; }
  if (erased_in_window >= 2){
    if (non_blocking){ s->gc_busy_events++; return STAMPDB_EBUSY; }
    // block until next window
    while ((platform_millis() - window_start) < 1000) {}
    window_start = platform_millis(); erased_in_window = 0;
  }

  // reclaim the oldest (tail) segment
  uint32_t oldest_seq = 0xFFFFFFFFu; uint32_t oldest_idx = 0;
  for (uint32_t i=0;i<s->seg_count;i++) if (s->segs[i].valid && s->segs[i].block_count>0){
    if (s->segs[i].seg_seqno < oldest_seq){ oldest_seq = s->segs[i].seg_seqno; oldest_idx = i; }
  }
  uint32_t base = oldest_idx*STAMPDB_SEG_BYTES;
  platform_flash_erase_4k(base);
  s->segs[oldest_idx].t_min=0xFFFFFFFFu; s->segs[oldest_idx].t_max=0; s->segs[oldest_idx].block_count=0; memset(s->segs[oldest_idx].series_bitmap,0,STAMPDB_SERIES_BITMAP_BYTES);
  erased_in_window++;
  (void)last_erase_ms;
  return 0;
}
