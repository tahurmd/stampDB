/**
 * @file codec.c
 * @brief Fixed16 value quantization and timestamp delta codec for 224 B payloads.
 *
 * What it owns:
 *  - Payload encoder/decoder and block header pack/unpack with header CRC
 *
 * Role in system:
 *  - Writer builds blocks that fit a single 256 B page (224 B payload + 32 B header)
 *
 * Constraints:
 *  - dt_bits chooses 8-bit or 16-bit delta lanes; values are 16-bit signed (Fixed16)
 */
#include "stampdb_internal.h"
#include <string.h>

static inline uint16_t rd16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);} 
static inline void wr16(uint8_t *p, uint16_t v){p[0]=(uint8_t)(v&0xFF);p[1]=(uint8_t)(v>>8);} 
static inline uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);} 
static inline void wr32(uint8_t *p, uint32_t v){p[0]=(uint8_t)(v&0xFF);p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);} 

/**
 * @brief Encode timestamp deltas and quantized values into the 224 B payload area.
 *
 * Inputs:
 *  - dst224: destination buffer (exactly 224 bytes)
 *  - dt_bits: 8 or 16; selects delta lane width
 *  - ts_deltas: array of `count` deltas (0..65535)
 *  - qvals: array of `count` quantized values (int16)
 *  - count: number of samples in the block
 *
 * Notes:
 *  - Payload is zero-filled with 0xFF for NOR cleanliness.
 */
size_t codec_encode_payload(uint8_t *dst224, uint8_t dt_bits, const uint32_t *ts_deltas, const int16_t *qvals, uint16_t count){
  // layout: deltas then qvals
  uint8_t *p = dst224;
  if (dt_bits==8){
    for (uint16_t i=0;i<count;i++) *p++=(uint8_t)ts_deltas[i];
  } else {
    for (uint16_t i=0;i<count;i++){ wr16(p,(uint16_t)ts_deltas[i]); p+=2; }
  }
  for (uint16_t i=0;i<count;i++){ wr16(p,(uint16_t)qvals[i]); p+=2; }
  size_t used = (size_t)(p - dst224);
  for (size_t i=used;i<STAMPDB_PAYLOAD_BYTES;i++) dst224[i]=0xFF;
  return used;
}

/**
 * @brief Decode payload back into delta and qval arrays.
 */
size_t codec_decode_payload(const uint8_t *src224, uint8_t dt_bits, uint32_t *ts_deltas, int16_t *qvals, uint16_t count){
  const uint8_t *p = src224;
  if (dt_bits==8){
    for (uint16_t i=0;i<count;i++) ts_deltas[i]=*p++;
  } else {
    for (uint16_t i=0;i<count;i++){ ts_deltas[i]=rd16(p); p+=2; }
  }
  for (uint16_t i=0;i<count;i++){ qvals[i]=(int16_t)rd16(p); p+=2; }
  return (size_t)(p - src224);
}

/**
 * @brief Pack block header and compute header CRC over first 28 bytes.
 */
void codec_pack_header(uint8_t out32[STAMPDB_HEADER_BYTES], const block_header_t *h){
  memset(out32, 0xFF, STAMPDB_HEADER_BYTES);
  wr32(out32+0, STAMPDB_BLOCK_MAGIC);
  wr16(out32+4, h->series);
  wr16(out32+6, h->count);
  wr32(out32+8, h->t0_ms);
  out32[12]=h->dt_bits;
  memcpy(out32+16, &h->bias, 4);
  memcpy(out32+20, &h->scale, 4);
  wr32(out32+24, h->payload_crc);
  uint32_t hc = crc32c(out32, 28);
  wr32(out32+28, hc);
}

/**
 * @brief Verify and unpack block header; returns false on CRC or magic mismatch.
 */
bool codec_unpack_header(block_header_t *h, const uint8_t in32[STAMPDB_HEADER_BYTES]){
  if (rd32(in32+0) != STAMPDB_BLOCK_MAGIC) return false;
  // Use aligned reads to avoid potential faults on some MCUs
  uint32_t hc = rd32(in32+28);
  uint32_t calc = crc32c(in32, 28);
  if (hc != calc) return false;
  h->series = rd16(in32+4);
  h->count = rd16(in32+6);
  h->t0_ms = rd32(in32+8);
  h->dt_bits = in32[12];
  memcpy(&h->bias, in32+16, 4);
  memcpy(&h->scale, in32+20, 4);
  h->payload_crc = rd32(in32+24);
  h->header_crc = hc;
  return true;
}
