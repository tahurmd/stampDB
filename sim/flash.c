/**
 * @file flash.c
 * @brief Host NOR flash simulator: 1→0 program, 4 KiB erase, persisted to disk.
 *
 * What it owns:
 *  - In-memory flash image with periodic persistence to `flash.bin`
 *  - Read/erase/program operations enforcing NOR semantics
 *
 * Notes:
 *  - `sim_flash_read` reloads from disk to reflect external mutations by tests.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *flash_path = "flash.bin";
static uint8_t *flash_mem = NULL;
static uint32_t flash_bytes = 4*1024*1024; // default 4 MiB

/** @brief Lazy allocation + initial load from disk (or 0xFF fill). */
static void ensure_loaded(void){
  if (flash_mem) return;
  const char *env = getenv("STAMPDB_SIM_FLASH_BYTES");
  if (env) { unsigned long v = strtoul(env, NULL, 10); if (v>=4096) flash_bytes = (uint32_t)v; }
  flash_mem = (uint8_t*)malloc(flash_bytes);
  FILE *f = fopen(flash_path, "rb");
  if (f){ fread(flash_mem,1,flash_bytes,f); fclose(f); }
  else { memset(flash_mem,0xFF,flash_bytes); }
}

/** @brief Persist entire image to disk. */
static void persist(void){ FILE *f=fopen(flash_path,"wb"); if(!f) return; fwrite(flash_mem,1,flash_bytes,f); fclose(f);} 

/** @brief Read from flash (refreshes in-memory view from disk). */
int sim_flash_read(uint32_t addr, void *dst, size_t len){
  ensure_loaded();
  // Refresh from disk to honor external modifications by tests
  FILE *f = fopen(flash_path, "rb");
  if (f){ fread(flash_mem,1,flash_bytes,f); fclose(f); }
  else { memset(flash_mem,0xFF,flash_bytes); }
  if (addr+len>flash_bytes) return -1; memcpy(dst, flash_mem+addr, len); return 0;
}

/** @brief Erase a 4 KiB sector (fills with 0xFF). */
int sim_flash_erase_4k(uint32_t addr){ ensure_loaded(); if (addr%4096) return -1; if (addr+4096>flash_bytes) return -1; memset(flash_mem+addr, 0xFF, 4096); persist(); return 0; }

/** @brief Program a 256 B page using NOR 1→0 (bitwise AND with existing). */
int sim_flash_program_256(uint32_t addr, const void *src){ ensure_loaded(); if (addr%256) return -1; if (addr+256>flash_bytes) return -1; const uint8_t *s=(const uint8_t*)src; for (size_t i=0;i<256;i++){ flash_mem[addr+i] = flash_mem[addr+i] & s[i]; } persist(); return 0; }

/** @brief Total simulated flash size (bytes). */
uint32_t sim_flash_size_bytes(void){ ensure_loaded(); return flash_bytes; }
