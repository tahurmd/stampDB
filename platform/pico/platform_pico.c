/**
 * @file platform_pico.c
 * @brief Pico platform glue: __not_in_flash_func flash ops and multicore deps.
 *
 * Role in system:
 *  - Provides XIP-safe erase/program and monotonic clock for quotas/hints.
 */
#include "stampdb_internal.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "pico/multicore.h"

// These functions must run from SRAM
#ifndef __not_in_flash_func
#define __not_in_flash_func(x) x
#endif

extern uint32_t __flash_binary_end; // symbol typically provided by linker

/** @brief Monotonic milliseconds from SDK timebase. */
uint64_t platform_millis(void){ return to_ms_since_boot(get_absolute_time()); }

static uint32_t flash_total_bytes(void){
#ifdef PICO_FLASH_SIZE_BYTES
  return PICO_FLASH_SIZE_BYTES;
#else
  // Fallback: 2 MiB if board config not provided
  return 2*1024*1024;
#endif
}

/** @brief XIP read from flash-mapped address space. */
int platform_flash_read(uint32_t addr, void *dst, size_t len){
  memcpy(dst, (const void*)(XIP_BASE + addr), len); return 0;
}

/** @brief 4 KiB sector erase (runs from SRAM). */
int __not_in_flash_func(platform_flash_erase_4k)(uint32_t addr){
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(addr, 4096);
  restore_interrupts(ints);
  return 0;
}

/** @brief 256 B page program (runs from SRAM, 1→0 only). */
int __not_in_flash_func(platform_flash_program_256)(uint32_t addr, const void *src){
  uint32_t ints = save_and_disable_interrupts();
  flash_range_program(addr, (const uint8_t*)src, 256);
  restore_interrupts(ints);
  return 0;
}

/** @brief Total flash size (bytes); adjust if board differs from 2 MiB. */
uint32_t platform_flash_size_bytes(void){ return flash_total_bytes(); }
