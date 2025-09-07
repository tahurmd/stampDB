/**
 * @file meta_lfs.c
 * @brief Metadata persistence for snapshots and head hints (Host: files, Pico: LittleFS).
 *
 * What it owns:
 *  - A/B snapshot load/save and ring-head hint load/save with CRC
 *
 * Role in system:
 *  - Bounds recovery time and provides progress hints to reduce scanning
 *
 * Constraints:
 *  - Host: atomic rename via tmp file; Pico: LittleFS tmp+rename
 */
#include "stampdb_internal.h"
#include <string.h>
#include <stdio.h>

#ifdef STAMPDB_PLATFORM_SIM
// Host: implement A/B snapshots and head hint via regular files

static const char *snap_a_path = "meta_snap_a.bin";
static const char *snap_b_path = "meta_snap_b.bin";
static const char *head_hint_path = "meta_head_hint.bin";

static int load_file(const char *p, void *buf, size_t len){
  FILE *f = fopen(p, "rb"); if (!f) return -1; size_t r=fread(buf,1,len,f); fclose(f); return r==len?0:-1; }
static int save_file_atomic(const char *p, const void *buf, size_t len){
  char tmp[256]; snprintf(tmp,sizeof(tmp),"%s.tmp",p);
  FILE *f=fopen(tmp,"wb"); if(!f) return -1; size_t w=fwrite(buf,1,len,f); fclose(f); if(w!=len) return -1; if (rename(tmp,p)!=0) return -1; return 0; }

/** @brief Load newest valid snapshot (A or B); returns 0 on success. */
int meta_load_snapshot(stampdb_snapshot_t *out){
  stampdb_snapshot_t a,b; int have_a=0,have_b=0;
  if (load_file(snap_a_path, &a, sizeof(a))==0){ uint32_t crc=a.crc; a.crc=0; if (crc32c(&a,sizeof(a))==crc) have_a=1; }
  if (load_file(snap_b_path, &b, sizeof(b))==0){ uint32_t crc=b.crc; b.crc=0; if (crc32c(&b,sizeof(b))==crc) have_b=1; }
  if (!have_a && !have_b) return -1;
  *out = (!have_b || (have_a && a.seg_seq_head >= b.seg_seq_head)) ? a : b;
  return 0;
}

/** @brief Save snapshot by toggling A/B target; file is rename-atomic. */
int meta_save_snapshot(const stampdb_snapshot_t *snap){
  // toggle based on seg_seq_head parity
  stampdb_snapshot_t s=*snap; s.crc=0; s.crc=crc32c(&s,sizeof(s));
  const char *primary = (snap->seg_seq_head & 1)? snap_a_path : snap_b_path;
  return save_file_atomic(primary, &s, sizeof(s));
}

/** @brief Load ring head hint (address + seq) with CRC. */
int meta_load_head_hint(uint32_t *addr_out, uint32_t *seq_out){
  struct {uint32_t addr; uint32_t seq; uint32_t crc;} h;
  if (load_file(head_hint_path, &h, sizeof(h))!=0) return -1;
  uint32_t c=h.crc; h.crc=0; if (crc32c(&h,sizeof(h))!=c) return -1; *addr_out=h.addr; *seq_out=h.seq; return 0;
}

/** @brief Save ring head hint as a small record; rename-atomic. */
int meta_save_head_hint(uint32_t addr, uint32_t seq){
  struct {uint32_t addr; uint32_t seq; uint32_t crc;} h={addr,seq,0};
  h.crc=crc32c(&h,sizeof(h));
  return save_file_atomic(head_hint_path, &h, sizeof(h));
}

#else

// Pico: official LittleFS-backed A/B snapshots and head pointer files with rename-atomic

#include "lfs.h"

static uint32_t fs_base(void){ return platform_flash_size_bytes() - STAMPDB_META_RESERVED; }

/** @brief LittleFS block-device read thunk to physical flash. */
static int lfs_bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size){
  uint32_t addr = fs_base() + block * c->block_size + off;
  return platform_flash_read(addr, buffer, size);
}
/** @brief LittleFS block-device program thunk (256 B aligned, 1→0). */
static int lfs_bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size){
  // prog_size is 256; enforce page-aligned writes
  uint32_t addr = fs_base() + block * c->block_size + off;
  const uint8_t *p=(const uint8_t*)buffer; size_t rem=size;
  while (rem){
    if ((addr % 256)!=0 || rem < 256) {
      // align to 256 by staging
      uint8_t tmp[256]; memset(tmp,0xFF,sizeof(tmp));
      size_t chunk = (size_t)((addr%256)? (256 - (addr%256)) : (rem<256?rem:256));
      // read existing page to preserve
      uint32_t page_base = addr - (addr%256);
      platform_flash_read(page_base, tmp, 256);
      size_t offpg = addr - page_base;
      for (size_t i=0;i<chunk;i++) tmp[offpg+i] = tmp[offpg+i] & p[i];
      platform_flash_program_256(page_base, tmp);
      addr += chunk; p += chunk; rem -= chunk;
    } else {
      platform_flash_program_256(addr, p);
      addr += 256; p += 256; rem -= 256;
    }
  }
  return 0;
}
/** @brief LittleFS block-device erase thunk (4 KiB). */
static int lfs_bd_erase(const struct lfs_config *c, lfs_block_t block){
  uint32_t addr = fs_base() + block * c->block_size;
  return platform_flash_erase_4k(addr);
}
static int lfs_bd_sync(const struct lfs_config *c){ (void)c; return 0; }

/** @brief Fill default LFS configuration for reserved metadata region. */
static void cfg_defaults(struct lfs_config *cfg){
  memset(cfg,0,sizeof(*cfg));
  cfg->context=NULL;
  cfg->read = lfs_bd_read;
  cfg->prog = lfs_bd_prog;
  cfg->erase= lfs_bd_erase;
  cfg->sync = lfs_bd_sync;
  cfg->read_size = 16;
  cfg->prog_size = 256;
  cfg->block_size = 4096;
  cfg->cache_size = 256;
  cfg->lookahead_size = 32;
  cfg->block_count = STAMPDB_META_RESERVED / cfg->block_size;
  cfg->block_cycles = 100;
}

/** @brief Mount if possible, otherwise format and mount. */
static int lfs_mount_or_format(lfs_t *lfs, struct lfs_config *cfg){
  int r = lfs_mount(lfs, cfg);
  if (r != 0){
    lfs_format(lfs, cfg);
    r = lfs_mount(lfs, cfg);
  }
  return r;
}

static const char *F_SNAP_A = "snap_a";
static const char *F_SNAP_B = "snap_b";
static const char *F_HEAD   = "head_hint";

/** @brief Load exact-size file into buffer; 0 on success. */
static int lfs_load_file(const char *name, void *buf, lfs_size_t len){
  lfs_t lfs; struct lfs_config cfg; cfg_defaults(&cfg); if (lfs_mount_or_format(&lfs,&cfg)!=0) return -1;
  lfs_file_t f; int r=-1;
  if (lfs_file_open(&lfs,&f,name,LFS_O_RDONLY)==0){
    lfs_ssize_t n = lfs_file_read(&lfs,&f,buf,len);
    if (n == (lfs_ssize_t)len) r=0;
    lfs_file_close(&lfs,&f);
  }
  lfs_unmount(&lfs);
  return r;
}

/** @brief Save to temp and rename to ensure atomic update. */
static int lfs_save_file_atomic(const char *name, const void *buf, lfs_size_t len){
  lfs_t lfs; struct lfs_config cfg; cfg_defaults(&cfg); if (lfs_mount_or_format(&lfs,&cfg)!=0) return -1;
  char tmpn[32]; snprintf(tmpn,sizeof(tmpn),"%s.tmp",name);
  lfs_file_t f; int r=-1;
  if (lfs_file_open(&lfs,&f,tmpn,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)==0){
    lfs_ssize_t n = lfs_file_write(&lfs,&f,buf,len);
    lfs_file_close(&lfs,&f);
    if (n == (lfs_ssize_t)len){
      lfs_remove(&lfs,name); // ignore error
      if (lfs_rename(&lfs,tmpn,name)==0) r=0;
    }
  }
  lfs_unmount(&lfs);
  return r;
}

/** @brief Load newest valid snapshot (A or B) from LFS; returns 0 on success. */
int meta_load_snapshot(stampdb_snapshot_t *out){
  stampdb_snapshot_t a,b; int va=0,vb=0;
  if (lfs_load_file(F_SNAP_A,&a,sizeof(a))==0){ uint32_t c=a.crc; a.crc=0; if (crc32c(&a,sizeof(a))==c) va=1; a.crc=c; }
  if (lfs_load_file(F_SNAP_B,&b,sizeof(b))==0){ uint32_t c=b.crc; b.crc=0; if (crc32c(&b,sizeof(b))==c) vb=1; b.crc=c; }
  if (!va && !vb) return -1;
  *out = (!vb || (va && a.seg_seq_head >= b.seg_seq_head)) ? a : b; return 0;
}

/** @brief Save snapshot to A/B file (parity toggled) using atomic rename. */
int meta_save_snapshot(const stampdb_snapshot_t *snap){
  stampdb_snapshot_t s=*snap; s.crc=0; s.crc=crc32c(&s,sizeof(s));
  const char *nm = (snap->seg_seq_head & 1) ? F_SNAP_A : F_SNAP_B;
  return lfs_save_file_atomic(nm, &s, sizeof(s));
}

/** @brief Load ring head hint record with CRC; 0 on success. */
int meta_load_head_hint(uint32_t *addr_out, uint32_t *seq_out){
  struct {uint32_t addr; uint32_t seq; uint32_t crc;} h;
  if (lfs_load_file(F_HEAD,&h,sizeof(h))!=0) return -1; uint32_t c=h.crc; h.crc=0; if (crc32c(&h,sizeof(h))!=c) return -1; *addr_out=h.addr; *seq_out=h.seq; return 0;
}

/** @brief Save ring head hint using atomic rename. */
int meta_save_head_hint(uint32_t addr, uint32_t seq){
  struct {uint32_t addr; uint32_t seq; uint32_t crc;} h={addr,seq,0}; h.crc=crc32c(&h,sizeof(h));
  return lfs_save_file_atomic(F_HEAD,&h,sizeof(h));
}

#endif
