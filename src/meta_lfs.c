/**
 * @file meta_lfs.c
 * @brief Metadata persistence (snapshots + head hints) in a raw meta region.
 *
 * Implementation: Use three dedicated 4 KiB sectors at the top of flash.
 *  - Sector 0: Snapshot A (first 256 B page stores the record)
 *  - Sector 1: Snapshot B
 *  - Sector 2: Head hint (addr + seq + crc in first 256 B page)
 * Each write erases the owning sector, then writes one 256 B page.
 */
#include "stampdb_internal.h"
#include <string.h>

#define META_SECTOR_BYTES 4096u
#define META_PAGE_BYTES   256u

static inline uint32_t meta_base(void){ return platform_flash_size_bytes() - STAMPDB_META_RESERVED; }
static inline uint32_t meta_snap_a_base(void){ return meta_base() + 0u * META_SECTOR_BYTES; }
static inline uint32_t meta_snap_b_base(void){ return meta_base() + 1u * META_SECTOR_BYTES; }
static inline uint32_t meta_head_base(void){   return meta_base() + 2u * META_SECTOR_BYTES; }

static int page_all_ff(const uint8_t *p){ for (size_t i=0;i<META_PAGE_BYTES;i++){ if (p[i]!=0xFF) return 0; } return 1; }

static int read_record(uint32_t base_addr, void *dst, size_t len){
  uint8_t page[META_PAGE_BYTES];
  if (platform_flash_read(base_addr, page, sizeof(page))!=0) return -1;
  if (page_all_ff(page)) return -1; // treat erased as missing
  if (len > sizeof(page)) return -1;
  memcpy(dst, page, len);
  return 0;
}

static int write_record(uint32_t base_addr, const void *src, size_t len){
  if (len > META_PAGE_BYTES) return -1;
  uint8_t page[META_PAGE_BYTES]; memset(page, 0xFF, sizeof(page)); memcpy(page, src, len);
  if (platform_flash_erase_4k(base_addr)!=0) return -1;
  return platform_flash_program_256(base_addr, page);
}

/** @brief Load newest valid snapshot (A/B); 0 on success. */
int meta_load_snapshot(stampdb_snapshot_t *out){
  stampdb_snapshot_t a={0}, b={0}; int va=0, vb=0;
  if (read_record(meta_snap_a_base(), &a, sizeof(a))==0){ uint32_t c=a.crc; a.crc=0; if (crc32c(&a,sizeof(a))==c) { a.crc=c; va=1; } }
  if (read_record(meta_snap_b_base(), &b, sizeof(b))==0){ uint32_t c=b.crc; b.crc=0; if (crc32c(&b,sizeof(b))==c) { b.crc=c; vb=1; } }
  if (!va && !vb) return -1;
  *out = (!vb || (va && a.seg_seq_head >= b.seg_seq_head)) ? a : b;
  return 0;
}

/** @brief Save snapshot by toggling A/B target based on seg_seq_head parity. */
int meta_save_snapshot(const stampdb_snapshot_t *snap){
  stampdb_snapshot_t s=*snap; s.crc=0; s.crc=crc32c(&s,sizeof(s));
  uint32_t base = (snap->seg_seq_head & 1u) ? meta_snap_a_base() : meta_snap_b_base();
  return write_record(base, &s, sizeof(s));
}

/** @brief Load ring head hint record with CRC; 0 on success. */
int meta_load_head_hint(uint32_t *addr_out, uint32_t *seq_out){
  struct {uint32_t addr; uint32_t seq; uint32_t crc;} h;
  if (read_record(meta_head_base(), &h, sizeof(h))!=0) return -1;
  uint32_t c=h.crc; h.crc=0; if (crc32c(&h,sizeof(h))!=c) return -1; *addr_out=h.addr; *seq_out=h.seq; return 0;
}

/** @brief Save ring head hint in its dedicated sector. */
int meta_save_head_hint(uint32_t addr, uint32_t seq){
  struct {uint32_t addr; uint32_t seq; uint32_t crc;} h={addr,seq,0}; h.crc=crc32c(&h,sizeof(h));
  return write_record(meta_head_base(), &h, sizeof(h));
}
