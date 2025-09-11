# SPEC.md — StampDB On‑Flash & API Specification (v1.0)

> **Normative language:** **MUST**, **SHOULD**, **MAY** follow RFC‑2119 tone. All sizes are in **bytes** unless noted. All multi‑byte fields are **little‑endian**.

---

## 1. Platform assumptions

- Target: Raspberry Pi **Pico 2 W (RP2350)** with \~**520 KiB SRAM** and **QSPI NOR flash** (2–4 MiB typical).
- Flash semantics: **erase = 4096 B (4 KiB)**, **program = 256 B** pages, **1→0 only** when programming; erased state = **0xFF**.
- XIP note: Program/erase **stalls XIP**; StampDB’s flash routines **MUST** run from **SRAM** on device builds.

---

## 2. Numerical constants (normative)

```
SEG_BYTES                 = 4096
PROG_BYTES                = 256
BLOCK_TARGET_PAYLOAD      = 224   // payload ≤224 so payload+header ≤256
BLOCK_HDR_MAX             = 32    // header fits within one 256‑B page tail
SERIES_BITMAP_BYTES       = 32    // 256 series quick‑skip per segment
READ_BATCH_TIGHT          = 256   // rows per batch (SoA)
READ_BATCH_COMFY          = 512
WATERMARK_WARN_PERCENT    = 10
WATERMARK_BUSY_PERCENT    = 5
GC_QUOTA_SEG_PER_SEC      = 2
SNAPSHOT_STRIDE_SEGMENTS  = 64
SNAPSHOT_STRIDE_SECONDS   = 10
RINGHEAD_STRIDE_BLOCKS    = 64
RINGHEAD_STRIDE_SECONDS   = 2
CRC32C_POLY               = 0x1EDC6F41  // Castagnoli
```

---

## 3. Types & identifiers

- **Series ID**: `uint16_t` (0..65535).
- **Timestamps**: API takes `uint32_t ts_ms` (monotonic milliseconds). Wrap (\~49.7 days) is disambiguated by `epoch_id` stored in snapshots.
- **Values**: API accepts `float` (binary32). On‑flash: **Fixed16** quantization per block.

---

## 4. On‑flash layout (data ring + raw meta region)

### 4.1 Data ring (raw log)

A fixed‑size circular region partitioned into **segments** of 4 KiB. Each segment contains **N pages** of 256 B. Within each page:

```
[  payload (≤224 B) ...  ][ BlockHdr (≤32 B) ]  => exactly 256 B programmed once
```

At the **end of each segment** there is a **SegmentFooter** placed in the last 256 B page.

### 4.2 Metadata (raw meta region)

A reserved flash region at the top of the device holds metadata as fixed 256‑byte records inside dedicated 4 KiB sectors:

```
Meta region (size = STAMPDB_META_RESERVED)
  Sector 0: Snapshot A (first 256 B page used)
  Sector 1: Snapshot B (first 256 B page used)
  Sector 2: Head hint   (first 256 B page used)
  Remaining: reserved for future use
```

Updates are sector‑erased and then a single 256‑byte page is programmed with a CRC. The newest valid snapshot is chosen by `seg_seq_head`.

---

## 5. Block payload format (encoded)

```
struct PayloadPreamble {
  float32 bias;        // 4  quantization bias
  float32 scale;       // 4  quantization scale (>0)
  uint32  t0_ms;       // 4  timestamp base for this block
  uint16  flags;       // 2  bit0: deltas=u8, bit1: deltas=u16 (mutually exclusive)
  uint16  count;       // 2  number of points
  // followed by: deltas[] then samples[]
}
// deltas: if flags.u8 → uint8[count], else if flags.u16 → uint16[count]
// samples: int16[count] where value ≈ bias + scale * q
```

**Rules (normative):**

1. **Close the block early** if adding one more point would exceed `BLOCK_TARGET_PAYLOAD`.
2. If any `dt = ts_ms - t0_ms` exceeds **255**, switch to `uint16` deltas for **this block**. If a `dt > 65535`, close the block early.
3. If any quantized `int16` would overflow, **close early**.
4. **CRC32C** is computed over the entire **payload** (preamble + deltas + samples).

**Quantization:**
`q = round((value - bias) / scale)`, `q ∈ [-32768, 32767]`.
**Error bound:** `|value - (bias + q*scale)| ≤ scale/2`.

---

## 6. Block header (written last, commits the page)

```
#define STAMPDB_BLOCK_MAGIC 0x4253   // 'SB'

struct BlockHdr {
  uint16 magic;        // 'SB'
  uint8  version;      // =1
  uint8  series_lo;    // low 8 bits of series
  uint32 t_min_ms;     // over block payload
  uint32 t_max_ms;     // over block payload
  uint16 count;        // duplicate of preamble.count
  uint16 payload_len;  // bytes of payload (≤224)
  uint16 series_hi;    // high 8 bits of series (<<8)
  uint16 reserved;     // 0
  uint32 crc32c;       // CRC32C(payload)
} // total ≤32 bytes
```

**Commit order (normative):**
(1) program the full 256‑B page with payload in the front; (2) write **BlockHdr** at the end of the page; (3) no further writes to this page.

---

## 7. Segment footer (once per 4 KiB segment)

```
#define STAMPDB_SEG_MAGIC   0x544D4353u   // 'SCMT'

struct SegmentFooter {
  uint32 magic;        // 'SCMT'
  uint8  version;      // =1
  uint8  reserved8[3];
  uint32 seg_seqno;    // monotonically increasing
  uint32 seg_t_min_ms;
  uint32 seg_t_max_ms;
  uint16 block_count;  // committed blocks in this segment
  uint16 reserved16;
  uint8  series_bitmap[32]; // 256 series presence bits
  uint32 crc32c;       // CRC32C over footer excluding this field
}
```

**Placement:** last 256‑B page of the segment. **Written once** when the segment rolls.

---

## 8. Snapshot & ring‑head (metadata)

```
#define STAMPDB_SNAP_MAGIC  0x50534E53u  // 'SNSP'

struct SnapshotV1 {
  uint32 magic;        // 'SNSP'
  uint8  version;      // =1
  uint8  reserved8[3];
  uint32 snap_seqno;   // monotonic
  uint32 epoch_id;     // disambiguates ts_ms wrap
  uint32 ring_head_seq;// next segment to write
  uint32 ring_tail_seq;// oldest retained segment
  uint32 crc32c;       // over all fields above
} // stored in meta sector 0 (A) or sector 1 (B)

struct RingHead {
  uint32 seg_seqno;    // recent head placement hint
  uint32 unix_secs;    // optional timestamp
  uint32 crc32c;
} // stored in meta sector 2 (head hint)
```

**A/B protocol:** write new snapshot to the older of A/B sectors (erase sector, then program the 256‑byte record). CRC guards torn writes.

---

## 9. Recovery (normative algorithm)

1. Read both snapshot sectors (A/B). Verify CRC; choose newest valid by `snap_seqno`.
2. Read head‑hint sector if present to fast‑forward head positioning.
3. Probe the **tail of the last segment** referenced by the snapshot: scan 256‑B pages forward; accept only pages with valid `BlockHdr` and CRC‑clean payload; stop at first invalid header; **truncate (virtually)** after the last valid block.
4. If snapshot missing or invalid: rebuild in‑RAM summaries by scanning only **SegmentFooter** pages across the ring (fast), then perform step 3 on the last segment.
  **Guarantee:** At most the **last partial block** is lost.

---

## 10. GC, retention, backpressure

- **Retention:** circular overwrite; reclaim oldest segments first.
- **Quota:** at most **GC_QUOTA_SEG_PER_SEC** reclaimed per second under pressure.
- **Watermarks:** `WARN` and `BUSY` percentages computed against total segments.
- **Backpressure:** default **block/delay**; non‑blocking mode **MAY** return `STAMPDB_EBUSY`.

**Retention estimate**
`days ≈ flash_bytes / ( avg_bytes_per_point × points_per_second × 86400 )`

---

## 11. Public C API (stable)

```c
// opaque
typedef struct stampdb stampdb_t;

typedef enum {
  STAMPDB_OK=0, STAMPDB_EINVAL, STAMPDB_EBUSY, STAMPDB_ENOSPACE, STAMPDB_ECRC, STAMPDB_EIO
} stampdb_rc;

typedef struct {
  void*    workspace;        // pre‑allocated
  uint32_t workspace_bytes;  // hard cap, checked at open
  uint32_t read_batch_rows;  // 256/512
  uint32_t commit_interval_ms; // 0=size‑only
} stampdb_cfg_t;

stampdb_rc stampdb_open(stampdb_t **db, const stampdb_cfg_t *cfg);
void       stampdb_close(stampdb_t *db);

stampdb_rc stampdb_write(stampdb_t *db, uint16_t series, uint32_t ts_ms, float value);
stampdb_rc stampdb_flush(stampdb_t *db);

// Range iterator (constant RAM)
typedef struct stampdb_it stampdb_it_t;
stampdb_rc stampdb_query_begin(stampdb_t *db, uint16_t series,
                               uint32_t t0_ms, uint32_t t1_ms, stampdb_it_t *it);
bool       stampdb_next(stampdb_it_t *it, uint32_t *ts_ms, float *val);
void       stampdb_query_end(stampdb_it_t *it);

stampdb_rc stampdb_query_latest(stampdb_t *db, uint16_t series,
                                uint32_t *out_ts_ms, float *out_value);

// Snapshots
stampdb_rc stampdb_snapshot_save(stampdb_t *db);

// Stats
typedef struct { uint32_t seg_seq_head, seg_seq_tail, blocks_written, crc_errors; } stampdb_stats_t;
void       stampdb_info(stampdb_t *db, stampdb_stats_t* out);
```

**Semantics:**

- `open()` MUST verify `workspace_bytes` ≥ minimum for chosen preset and pre‑layout internal pools. No heap after open.
- `write()` MAY block briefly (default) or return `EBUSY` in non‑blocking mode.
- `flush()` finalizes the current page (if any); MAY write a segment footer if the segment becomes full.
- Iteration returns rows **ordered by time** as written; each call to `next()` fills caller buffers from the internal SoA batch.

---

## 12. Invariants (must hold)

- One **256‑B program** per block page; **never** reprogram a page.
- Segment footer written **once**, after the last block in the segment.
- CRC32C covers **payload only**; header and footer have their own CRCs.
- All structures include **magic** and **version** fields for forward compatibility.

---

## 13. Compliance tests (summary)

- **Power‑cut matrix**: torn payload/header/footer → no false publishes.
- **CRC isolation**: payload bit‑flips only affect that block.
- **Snapshot‑bounded recovery**: time O(#segments since last snapshot).
- **Codec round‑trip**: error ≤ `scale/2`.
- **GC quota**: P99 ingest stable while reclaim ≤ 2 seg/s.
- **Exporter**: CSV/NDJSON by range returns monotonic (ts,value) rows.

---

## 14. Metadata location & versioning

- Snapshots: raw meta region sectors 0 (A) and 1 (B); record version = 1.
- Ring head hint: raw meta sector 2.
- Increment `version` on any incompatible format change; StampDB MAY support **read‑only** compatibility for older versions if RAM budget allows.
