/**
 * @file platform_sim.c
 * @brief Host platform glue: wall-clock and NOR flash shim bindings.
 *
 * Role in system:
 *  - Bridges core to `sim/flash.c` and provides millisecond clock.
 */
#include "stampdb_internal.h"
#include <time.h>
#include <string.h>

/** @brief Monotonic milliseconds used for quotas and hint cadence. */
uint64_t platform_millis(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec*1000ull + (uint64_t)(ts.tv_nsec/1000000ull);
}

extern int sim_flash_read(uint32_t addr, void *dst, size_t len);
extern int sim_flash_erase_4k(uint32_t addr);
extern int sim_flash_program_256(uint32_t addr, const void *src);
extern uint32_t sim_flash_size_bytes(void);

/** @brief NOR read/erase/program; 1→0 programming is enforced by the shim. */
int platform_flash_read(uint32_t addr, void *dst, size_t len){ return sim_flash_read(addr, dst, len); }
int platform_flash_erase_4k(uint32_t addr){ return sim_flash_erase_4k(addr); }
int platform_flash_program_256(uint32_t addr, const void *src){ return sim_flash_program_256(addr, src); }
uint32_t platform_flash_size_bytes(void){ return sim_flash_size_bytes(); }
