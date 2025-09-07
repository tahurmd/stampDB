/**
 * @file crc32c.c
 * @brief CRC-32C (Castagnoli) computation used for payloads/headers/footers.
 *
 * What it owns:
 *  - `crc32c()` implementation with a lazily-initialized 256-entry table
 *
 * Role in system:
 *  - Guards integrity of payloads (224 B), headers (28 B), and segment footers
 *
 * Constraints:
 *  - Table init is not thread-safe; DB is single-threaded on Pico/host usage
 */
#include <stdint.h>
#include <stddef.h>

static uint32_t table[256];
static int table_init = 0;

/**
 * @brief Initialize CRC table for polynomial 0x1EDC6F41 (Castagnoli).
 */
static void init_table(void) {
  uint32_t poly = 0x1EDC6F41u; // Castagnoli
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if (crc & 1) crc = (crc >> 1) ^ poly;
      else crc >>= 1;
    }
    table[i] = crc;
  }
  table_init = 1;
}

/**
 * @brief Compute CRC-32C over `len` bytes.
 * @return Bitwise-inverted CRC (standard CRC-32C output).
 */
uint32_t crc32c(const void *data, size_t len) {
  if (!table_init) init_table();
  const uint8_t *p = (const uint8_t*)data;
  uint32_t crc = ~0u;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}
