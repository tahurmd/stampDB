/**
 * @file tests_codec.c
 * @brief Codec round-trip and header pack/unpack tests.
 */
#include "stampdb.h"
#include "src/stampdb_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/** @brief Verify delta/value round-trip and header integrity. */
int main(void){
  // test codec round-trip tolerance
  uint32_t deltas[60]; for (int i=0;i<60;i++) deltas[i]=(i%5)+1;
  int16_t q[60]; float vals[60]; float bias=1.2f, scale=0.005f;
  for (int i=0;i<60;i++){ q[i]=(int16_t)(i-30); vals[i]=bias+scale*(float)q[i]; }
  uint8_t buf[224];
  codec_encode_payload(buf, 8, deltas, q, 60);
  uint32_t del2[60]; int16_t q2[60]; codec_decode_payload(buf,8,del2,q2,60);
  for (int i=0;i<60;i++) if (del2[i]!=deltas[i]){ fprintf(stderr,"delta mismatch\n"); return 1; }
  for (int i=0;i<60;i++) if (q2[i]!=q[i]){ fprintf(stderr,"q mismatch\n"); return 2; }
  // pack/unpack header
  block_header_t h={.series=3,.count=60,.t0_ms=1234,.dt_bits=8,.bias=bias,.scale=scale,.payload_crc=0xdeadbeef};
  uint8_t hdr[32]; codec_pack_header(hdr,&h);
  block_header_t h2; if (!codec_unpack_header(&h2,hdr)){ fprintf(stderr,"header unpack failed\n"); return 3; }
  if (h2.series!=h.series || h2.count!=h.count || h2.t0_ms!=h.t0_ms || h2.dt_bits!=h.dt_bits){ fprintf(stderr,"header field mismatch\n"); return 4; }
  return 0;
}
