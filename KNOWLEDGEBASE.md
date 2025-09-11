# StampDB Knowledgebase (Truth Mode)

## Title & TL;DR

StampDB is an embedded, append‑only time‑series store optimized for QSPI NOR
(4 KiB erase, 256 B program). It ships with a host simulator and Pico 2 W firmware.

Key properties (as implemented now):
- Append‑only 256 B pages: payload ≤224 B, then 32 B header (header‑last commit).
- CRC32C on payload; header and segment footers have their own CRCs.
- Compression: per‑block Fixed16 (bias/scale f32), u8/u16 timestamp deltas; close early to fit.
- Recovery: A/B snapshots + head hint + tail probe; at most last partial block lost.
- GC watermarks: warn <10% free; busy <5% free; ≤2 segments/sec quota (blocking writer).
- Pico: dual‑core split (Core1 DB, Core0 serial); flash ops in SRAM; metadata in a raw reserved flash region.

---

## Mini glossary

- Segment: 4 KiB unit of the ring (16 pages). The last page stores a SegmentFooter.
- Page: 256 B program unit; 1→0 bit programming; 4 KiB sector erase granularity.
- Header‑last: Write payload first; publish by writing the header tail last in the same 256 B page.
- Zone map: In‑RAM per‑segment summary: t_min/t_max, block_count, series bitmap (256 bits).
- Epoch wrap: 32‑bit ms timestamp wrap detection; epoch_id increments on big backward jump.
- SPSC: Single‑producer/single‑consumer FIFO between Pico cores via pico_multicore.

---

## Architecture at a Glance (ASCII)

```
PC (host)
  tools/tests/python -> libstampdb (sim) -> flash.bin (contains data + meta)

Pico (RP2350)
  Core0 (USB-serial) <==== SPSC FIFO ====> Core1 (StampDB)
         |                                      |
      serial CLI                      flash read/erase/program (__not_in_flash_func)
                                         Raw meta region (snap_a, snap_b, head_hint)

Storage split:
  - Raw data ring (NOR flash region, circular segments)
  - Metadata (raw reserved flash region; no filesystem)

Write/read/recovery/GC run inside the core library (src/stampdb.c, src/ring.c, src/read_iter.c).
```

---

## On‑flash schema (truth)

Geometry:
- Segment: 4 KiB (4096 B).
- Page: 256 B; 16 pages per segment; last page is footer.
- Data pages: 15 per segment.
- Max payload per page: 224 B (payload) + 32 B (header) = 256 B.

Diagrams:

Overall flash
```
[ Data Ring: 0 .. (flash_size - META_RESERVED - 1) ] [ Raw Meta Region: META_RESERVED at top ]
```

Segment (4 KiB)
```
+----------------------+ ... +----------------------+----------------------+
| data page[0] 256 B   | ... | data page[14] 256 B  | footer page[15] 256 B|
+----------------------+     +----------------------+----------------------+
```

Data page (256 B)
```
+------------------------------+-----------------+
|       Payload (≤224 B)       |  Header (32 B)  |
+------------------------------+-----------------+
```

Payload format (as coded):
- deltas lane (u8×count or u16×count) followed by qvals lane (int16×count).
- Unused payload bytes are filled with 0xFF to reach 224 B.
- CRC32C computed over the entire 224 B payload.

Drift vs SPEC §5: SPEC’s “PayloadPreamble” (bias/scale/t0/flags/count) is NOT in payload.
Those fields live in the 32 B header in this implementation.

Struct size guarantees (compile‑time):
- Header is locked at 32 bytes (compile‑time assert in code).
- Footer occupies a full 256‑byte page (compile‑time assert on the image size).

Endianness & CRC:
- All multi‑byte integers and floats are little‑endian on flash.
- CRC32C (Castagnoli): polynomial 0x1EDC6F41, init 0xFFFFFFFF, final XOR 0xFFFFFFFF (per `src/crc32c.c`).

Block header (32 B, written last, commits the page):
- Magic: `STAMPDB_BLOCK_MAGIC = 'BLK1' = 0x424C4B31`.
- Header CRC: CRC32C over bytes 0..27; stored at bytes 28..31.

| Field       | Size | Units | Meaning                                   | In header CRC? | Write order |
|-------------|------|-------|-------------------------------------------|-----------------|-------------|
| magic       | 4    | u32   | 0x424C4B31 ('BLK1')                        | Yes             | (2) header  |
| series      | 2    | u16   | Series id (0..255 used)                   | Yes             |             |
| count       | 2    | u16   | Number of samples in block                | Yes             |             |
| t0_ms       | 4    | u32   | Base timestamp for deltas                 | Yes             |             |
| dt_bits     | 1    | u8    | 8 or 16 (delta width)                     | Yes             |             |
| pad         | 3    | bytes | 0xFF                                      | Yes             |             |
| bias        | 4    | f32   | Quantization bias                         | Yes             |             |
| scale       | 4    | f32   | Quantization scale (>0; clamp to 1e-9)    | Yes             |             |
| payload_crc | 4    | u32   | CRC32C(payload 224 B)                      | Yes             |             |
| header_crc  | 4    | u32   | CRC32C(header[0..27])                      | n/a             |             |

Segment footer (256 B) — last page:
- Magic: `STAMPDB_FOOTER_MAGIC = 'SFG1' = 0x53464731`.
- CRC: CRC32C of struct with `.crc=0`.

| Field         | Size | Units | Meaning                                   | CRC? |
|---------------|------|-------|-------------------------------------------|------|
| magic         | 4    | u32   | 0x53464731 ('SFG1')                        | Yes  |
| seg_seqno     | 4    | u32   | Monotonic segment sequence number         | Yes  |
| t_min         | 4    | u32   | Min ts in segment                         | Yes  |
| t_max         | 4    | u32   | Max ts in segment                         | Yes  |
| block_count   | 4    | u32   | Number of committed blocks                | Yes  |
| series_bitmap | 32   | bytes | 256‑bit series presence bitmap            | Yes  |
| crc           | 4    | u32   | CRC32C over struct with crc=0             | n/a  |

Snapshot (metadata; raw flash):
- Stored in two 4 KiB sectors at top of flash (A/B). First 256 B page contains the record.

| Field         | Size | Units | Meaning                             | CRC? |
|---------------|------|-------|-------------------------------------|------|
| version       | 4    | u32   | 1                                   | Yes  |
| epoch_id      | 4    | u32   | Epoch disambiguator for wrap        | Yes  |
| seg_seq_head  | 4    | u32   | Head segment seq                    | Yes  |
| seg_seq_tail  | 4    | u32   | Tail/oldest segment seq             | Yes  |
| head_addr     | 4    | u32   | Absolute head address               | Yes  |
| crc           | 4    | u32   | CRC32C over struct with crc=0       | n/a  |

Head hint (metadata; raw flash sector):
| Field | Size | Units | Meaning                | CRC? |
|-------|------|-------|------------------------|------|
| addr  | 4    | u32   | Head address hint      | Yes  |
| seq   | 4    | u32   | Head seq hint          | Yes  |
| crc   | 4    | u32   | CRC32C with crc=0      | n/a  |

---

## Data flow (step‑by‑step)

Write path
1) Builder accumulates samples for the current series.
2) Compute bias/scale from current values; clamp scale to 1e‑9 if zero range.
3) Quantize to int16; choose dt_bits=8 if max delta ≤255 else 16; compute payload size.
4) If adding a sample would exceed 224 B payload, close block early.
5) Encode payload: deltas then qvals; fill with 0xFF; compute payload CRC.
6) Header‑last commit:
   - Program 256 B page with payload then 0xFF header space.
   - Program the 32 B header bytes at page tail to publish block.
7) Update in‑RAM zone map; update head ptr. Move to next page in segment.
8) If data pages are full, finalize segment:
   - Build footer by scanning pages; write footer; erase next segment; rotate head.

Read path
1) Iterator uses zone map to skip non‑matching segments:
   - invalid/empty, series bit not set, or time window not overlapping (wrap‑aware).
2) For each candidate page: read payload and header, verify header magic+CRC and payload CRC.
3) Decode payload into per‑block buffers; reconstruct times (t0 + deltas).
4) Yield rows within [t0..t1]. Constant RAM per iterator (SoA per block).
Hard cap: Iteration enforces a maximum pages visited per call of `seg_count * 15 + 1` to avoid unbounded scans.

Recovery
- On open:
  - Build zone map: scan footers; if none, probe seg 0 to seed.
  - Load snapshot if available; seed head addr/page index, tail seq, epoch_id.
  - Load head hint (if valid and within usable range).
  - Probe current head segment tail: scan from page 0, accept only CRC‑clean blocks;
    stop at first invalid. If at least one valid page preceded the invalid, count a
    `recovery_truncations` event and position head at first invalid page.
- Guarantee: at most the last partial block is lost.
Hard cap: Tail probe enforces a maximum pages visited per call of `seg_count * 15 + 1` to avoid unbounded scans.

GC & retention
- Circular reclaim of oldest segments.
- Watermarks:
  - Warn when free% < 10% (`gc_warn_events` counter).
  - Busy when free% < 5% (`gc_busy_events` counter). Non‑blocking GC would return EBUSY;
    current write path uses blocking GC when needed.
- Quota: ≤2 segments/sec; blocks when quota hit (non‑blocking path increments busy counter).

Backpressure
- Writer path is blocking under GC pressure; no public non‑blocking write mode is exposed.

---

## Compression model (truth)

- Values: Fixed16 per block (bias/scale), quantization error ≤ scale/2 (by rounding).
- Deltas: per‑block dt_bits=8 or 16; chosen by max delta in the block.
- Payload limit: ≤224 B; close early if the next sample won’t fit.
- Overflow: close early if deltas would need >16 bits or quantized value would exceed int16.

Drift vs SPEC §5:
- SPEC: bias/scale/t0/count in a “payload preamble.” Implementation stores these in the header.
- SPEC’s flags/payload_len not used; header contains payload_crc and extra fields.

---

## Public API cheat sheet (actual signatures)

| Function | Purpose | Inputs | Outputs | Typical caller | Blocking |
|----------|---------|--------|---------|----------------|----------|
| `stampdb_open(stampdb_t **db, const stampdb_cfg_t *cfg)` | Open DB, recover ring | `cfg` (workspace*, size, batch rows, commit interval) | `db` handle | tools/Pico app | Yes |
| `stampdb_close(stampdb_t *db)` | Close DB | `db` | — | tools/Pico | No |
| `stampdb_write(stampdb_t *db, uint16_t series, uint32_t ts_ms, float value)` | Append one sample | series 0..255; ts_ms u32; value f32 | rc | tools/Pico | Yes (may GC) |
| `stampdb_flush(stampdb_t *db)` | Force publish current block | `db` | rc | tools/Pico | Yes |
| `stampdb_query_begin(stampdb_t *db, uint16_t series, uint32_t t0_ms, uint32_t t1_ms, stampdb_it_t *it)` | Start iterator | series, t0, t1, it | rc | tools/Pico | No |
| `stampdb_next(stampdb_it_t *it, uint32_t *ts_ms, float *val)` | Next row | it | row or false | tools/Pico | No |
| `stampdb_query_end(stampdb_it_t *it)` | End iterator | it | — | tools/Pico | No |
| `stampdb_query_latest(stampdb_t *db, uint16_t series, uint32_t *out_ts_ms, float *out_value)` | Latest row | series | (ts,val) | tools/Pico | No |
| `stampdb_snapshot_save(stampdb_t *db)` | Save A/B snapshot | `db` | rc | tools/Pico | Yes |
| `stampdb_info(stampdb_t *db, stampdb_stats_t *out)` | Stats | `db` | head seq, tail seq, blocks_written, crc_errors, gc_warn_events, gc_busy_events, recovery_truncations | tools/Pico | No |

Example snippets
```c
// Open -> write -> flush -> latest -> iterate
static unsigned char ws[128*1024];
stampdb_t *db=NULL; stampdb_cfg_t cfg={ws,(uint32_t)sizeof(ws),256,0};
stampdb_open(&db,&cfg);
stampdb_write(db, 1, 0, 42.0f);
stampdb_flush(db);
uint32_t ts; float val; stampdb_query_latest(db,1,&ts,&val);
stampdb_it_t it; stampdb_query_begin(db,1,0,1000,&it);
while (stampdb_next(&it,&ts,&val)) { /* use ts,val */ }
stampdb_query_end(&it);
stampdb_close(db);
```

---

## File map (what each file owns & why)

| Path | Responsibility | Key functions | Who calls |
|------|----------------|---------------|----------|
| `include/stampdb.h` | Public API/types | All API functions | tools/Pico/python |
| `src/stampdb_internal.h` | Internals/glue/geometry | CRC/codec/flash/meta decls | all src |
| `src/codec.c` | Payload encode/decode; header pack/unpack | `codec_*` | writer/iterator |
| `src/crc32c.c` | CRC32C (Castagnoli) | `crc32c` | codec/ring |
| `src/stampdb.c` | API impl; builder; epoch wrap | `stampdb_*` | external callers |
| `src/ring.c` | Write/recover/GC/footer | `ring_*` | stampdb.c |
| `src/read_iter.c` | Iterator + latest | `stampdb_query_*`, latest | tools/app |
| `src/meta_lfs.c` | Metadata (raw reserved flash region) | `meta_*` | stampdb/ring |
| `sim/flash.c` | Host NOR sim (1→0, 4 KiB erase) | `sim_flash_*` | platform_sim |
| `sim/platform_sim.c` | Host glue (millis + sim) | `platform_*` | core |
| `platform/pico/platform_pico.c` | Pico flash ops (SRAM) | `platform_*` | core |
| `platform/pico/main.c` | Pico app (Core0 serial, Core1 DB) | FIFO cmd handlers | firmware |
| `platform/pico/CMakeLists.txt` | Pico build; platform glue only (no filesystem) | targets | CMake |
| `tools/stampctl.c` | CLI exporter + retention | `export`, `retention` | user/CI |
| `tests/*.c` | CTest suite | basic/codec/recovery/GC | CI/local |

---

## Build & environment

Host (presets)
```
cmake --preset host-debug
cmake --build --preset host-debug -j
(cd build/host-debug && ctest --output-on-failure)
```

Host (Makefiles explicit)
```
cmake -S . -B build/mk -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/mk -j
(cd build/mk && ctest --output-on-failure)
```

Dependencies
- CMake ≥ 3.20; C11 compiler.
- Pico SDK (fetched via CMake or set `PICO_SDK_PATH`).

Optional
- Python 3 for ctypes and serial client (`pyserial`).

Versioning & Changelog
- Current KB version: 1.1 (CLI Enhancements)
  - Added one‑word C CLI commands: `reset`, `hello`, `peek`, `dump` (in `stampctl`).
  - Added Python helper CLI `tools/stampp.py` with the same one‑word commands.
  - Added `stampctl info` (prints stats) and `stampctl ingest` (write N rows).
  - Pico flash size auto‑detection via `PICO_FLASH_SIZE_BYTES` (firmware build).
  - Guards: hard caps on page scans; env‑var control for sim file paths.

---

## ▶ Run on PC (with example data)

Clean sim
```
rm -f flash.bin
```

Build & test
```
cmake --preset host-debug
cmake --build --preset host-debug -j
(cd build/host-debug && ctest --output-on-failure)
```

Generate data (Python)
```
python3 - <<'PY'
from py.stampdb import StampDB
db=StampDB()
for i in range(100): db.write(1, i*10, i*0.1)
db.flush()
print('latest:', db.latest(1))
PY
```

Export CSV
```
./build/host-debug/stampctl export --series 1 --t0 0 --t1 1000 --csv | head
```

Where sim files live
- Default: repo root (`flash.bin`, contains both data and meta regions).

Env‑var overrides (host sim):
```
export STAMPDB_FLASH_PATH=/abs/path/flash.bin
```

Env overrides
- Flash size: `STAMPDB_SIM_FLASH_BYTES=8388608` (8 MiB).
- File path override: **Unknown** (not implemented). To confirm, inspect `sim/flash.c`.

---

## ▶ Flash on Pico (with example data)

Build UF2
```
cmake --preset pico2w-release
cmake --build --preset pico2w-release
```
UF2 path: `build/pico2w-release/platform/pico/stampdb_pico_fw.uf2`

Flash (BOOTSEL)
1) Hold BOOTSEL and plug Pico.
2) Drag‑and‑drop the UF2.

Serial (USB CDC)
- Commands:
  - `w <series> <ts_ms> <value>`: write
  - `f`: flush
  - `s`: snapshot
  - `l <series>`: prints `OK <ts_ms> <value>`
  - `e <series> <t0> <t1>`: streams `ts,value` lines then `END`
- Dual‑core truth: Core1 runs DB; Core0 handles serial/FIFO.

---

## Operations runbook

Snapshots
- Host: call `stampdb_snapshot_save()` to write a CRC‑guarded snapshot record (A/B sectors).
- Pico: send `s` over serial to persist a snapshot.

Export ranges
- Host: `stampctl export --series S --t0 T0 --t1 T1 --csv`
- Pico: `e S T0 T1` over serial; outputs CSV lines and `END`.

Retention estimate
- `stampctl retention --days D` prints rough capacity and rows/day.

Reset/wipe
- Host: delete `flash.bin`.
- Pico: **Unknown** dedicated wipe command via serial. Could add one; otherwise reflash.

Troubleshooting

| Symptom | Probable cause | Fix |
|--------|-----------------|-----|
| Export shows only header | No data written | Write some samples, then export |
| Recovery slow | Missing snapshots and many segments | Save snapshots regularly |
| Tests hang | Old build dir/working dir mismatch | `rm -rf build/*`; reconfigure; ensure repo root wd |
| EBUSY (non-blocking) | GC quota/busy watermark | Retry later or use blocking writes |
| CRC errors spike | Corrupted pages | Simulator reloads flash on read; check `flash.bin` |
| Not seeing `flash.bin` | Different wd | Run from repo root (sim paths are relative) |

---

## Performance & RAM (truth)

- Ingest: Two 256 B programs per committed block (payload then header), one 4 KiB erase per segment rollover.
- Read: CRC verify + header parse per page; SoA decode per block; zone‑map prunes segments.
- RAM: No heap after `stampdb_open()`. Workspace must cover:
  - `stampdb_t`, staging arrays (~74 rows), and zone‑map `seg_summary_t * seg_count`.
  - Zone‑map scales with ring size: `(flash_bytes - META_RESERVED)/4096` entries.
- Knobs: `read_batch_rows` (iterator buffering), `commit_interval_ms` (advisory), `STAMPDB_SIM_FLASH_BYTES` (host).

---

## Reliability model (truth)

Power‑cut scenarios
- Torn header: page payload written but header invalid/missing → block ignored; tail probe truncates after last valid page.
- Torn payload: CRC mismatch → page ignored; iterator skips forward safely.
- Torn footer: footer invalid → rebuilt later by scanning; data remains usable.
- Loss bound: at most last partial block.

CRC isolation
- Iterator checks header magic+CRC and payload CRC; corrupted pages are skipped,
  earlier pages remain readable.

Snapshot‑bounded recovery
- With snapshots: recovery time grows linearly with number of segments since last snapshot (test asserts a time bound with slack).

---

## Implementation Status & Drift (required truth section)

Implemented
- Timestamp wrap/epoch: Yes (epoch++ on large backward jump); stored in snapshots.
- Metadata: Raw reserved flash region for snapshots (A/B sectors) and head hint.
- Dual‑core (Pico): Yes (Core1 DB; Core0 serial; FIFO SPSC).
- Watermarks: warn <10%, busy <5% with counters in stats; ≤2 seg/s quota; writer blocks.
- Deep recovery scan: Yes (footers across ring, tail page probe).
- Tests: basic, codec RT, power‑cut matrix, CRC isolation, exporter correctness, recovery‑time bound, GC P99 latency.

Drift vs SPEC/ARCH
- SPEC §5 Payload preamble: **SHOULD** — Implementation puts bias/scale/t0/count/dt_bits in header; payload is deltas+qvals only.
- SPEC §6 BlockHdr: **SHOULD** — Header magic/layout differ (`'BLK1'`; has bias/scale/payload_crc; no t_min/t_max/version fields).
- SPEC §7 SegmentFooter: **NICE** — Footer magic/layout differ (`'SFG1'`; no version/reserved).
- SPEC §8 Snapshot/ring-head: **NICE** — Fields differ (version/epoch/head_addr/seqs) and file paths are simplified.
- ARCH metadata: **NICE** — Uses raw meta sectors for `snap_a`, `snap_b`, and a head hint page.

Severity
- BLOCKER: None.
- SHOULD: Align header/payload/footer formats to SPEC if interoperability matters.
- NICE: Align metadata structs/paths; expose non‑blocking GC option publicly if desired.

---

## Technical decisions & trade‑offs (as coded)

| Area | Chosen | Why | Explored/notes | Trade‑offs |
|------|--------|-----|----------------|-----------|
| Commit order | Header‑last | Power‑cut safety on NOR | Two‑phase write | Extra page program per block |
| Bias/scale placement | In header | Simpler decode; payload fixed | SPEC preamble differs | Divergence from SPEC |
| Delta width | u8/u16 per block | Compact yet robust | Close early on overflow | Decode supports two lane types |
| Zone‑map | Full in RAM | Fast queries | Scales with ring size | Workspace must scale with flash |
| Snapshots + hint | Yes | Bound recovery | Host files; Pico LFS | Small FS integration complexity |
| GC quota | 2 seg/s | Smooth reclaim | Busy/warn counters | Blocking under heavy pressure |
| Pico split | Dual‑core | Clear ownership; safety | Serial export streaming | Extra firmware code |

---

## Security, safety & wear

- NOR 1→0 rule: enforced by host sim and Pico flash API.
- Program/erase alignment: 256 B program, 4 KiB erase; Pico ops run from SRAM.
- XIP stalls avoided during erase/program (`__not_in_flash_func`).
- CRC32C (Castagnoli) used on payloads, headers, footers.
- Power‑cut tests confirm recovery behavior.

---

## Extensibility & integration

- Add a series: Use a new 0..255 series id (bitmap covers 256 series).
- Add exporter format: Extend `tools/stampctl.c` (CSV/NDJSON currently).
- Add a codec: Replace/augment `src/codec.c`; wire into block builder.
- Telemetry/MQTT: Add at application layer; stats available via `stampdb_info()`.

---

## FAQ & Glossary

- Do I need an RTC? No. Caller supplies `ts_ms` in u32; epoch disambiguation via snapshots.
- Can I delete data? No explicit delete; circular retention reclaims the oldest segments.
- Single‑core or dual‑core? Host is single‑process; Pico is dual‑core (Core1 DB).
- What is UF2? A firmware file you drag‑and‑drop when Pico is in BOOTSEL mode.
- Where is data on PC? In repo root: `flash.bin` (data + meta inside).
- Can I change sim flash size? Yes: `STAMPDB_SIM_FLASH_BYTES` env var.

---

## Appendix

ASCII diagrams
```
Page (256 B):
+------------------------------+-----------------+
|       Payload (≤224 B)       |  Header (32 B)  |
+------------------------------+-----------------+

Segment (4096 B):
+----------------------+ ... +----------------------+----------------------+
| data page[0] 256 B   | ... | data page[14] 256 B  | footer page[15] 256 B|
+----------------------+     +----------------------+----------------------+
```

Constants (as coded)
- `STAMPDB_SEG_BYTES = 4096`
- `STAMPDB_PAGE_BYTES = 256`
- `STAMPDB_PAYLOAD_BYTES = 224`
- `STAMPDB_HEADER_BYTES = 32`
- `STAMPDB_SERIES_BITMAP_BYTES = 32`
- `STAMPDB_MAX_SERIES = 256`
- `STAMPDB_BLOCK_MAGIC = 0x424C4B31 ('BLK1')`
- `STAMPDB_FOOTER_MAGIC = 0x53464731 ('SFG1')`
- `STAMPDB_META_RESERVED = 32768` (raw meta region at top)

CLI reference
- Export: `stampctl export --series S --t0 T0 --t1 T1 [--csv|--ndjson]`
- Retention: `stampctl retention --days D`
- Info: `stampctl info`
- Ingest: `stampctl ingest --series S --rows N [--period-ms P] [--start T0]`
- One‑word helpers: `stampctl reset | hello | peek | dump`

Tests inventory
- `tests_basic.c`: write/read sanity; latest monotonic.
- `tests_codec.c`: delta/value round‑trip; header pack/unpack.
- `tests_recovery.c`: torn header; recovery correctness.
- `tests_powercut_matrix.c`: torn header/payload/footer matrix.
- `tests_crc_isolation.c`: CRC‑corrupt a page; earlier data still readable.
- `tests_exporter.c`: CLI exporter produces rows.
- `tests_recovery_time.c`: reopen time bound ~ O(#segments since last snapshot).
- `tests_gc_latency.c`: P99 write latency bound under GC quota.

Unknowns (how to verify)
- Pico flash size detection: currently hardcoded default; use board config (`PICO_FLASH_SIZE_BYTES`) to confirm true size.
- Path override for sim files: not implemented; inspect `sim/flash.c` for adding an env var (would wrap `flash_path`).
