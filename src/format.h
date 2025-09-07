// Internal format invariants and compile-time guards (no public API changes)
#pragma once
#include "stampdb_internal.h"

// Lock header image size to 32 bytes (on-flash image)
typedef struct { unsigned char _img[32]; } stampdb_block_hdr_t;
_Static_assert(sizeof(stampdb_block_hdr_t) == 32, "BlockHdr must be 32 bytes");

// Lock footer page image size at 256 bytes (entire page reserved for footer)
typedef struct { unsigned char _page[256]; } stampdb_seg_footer_t;
_Static_assert(sizeof(stampdb_seg_footer_t) == 256, "Footer must be 256 bytes");
