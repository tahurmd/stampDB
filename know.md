StampDB Knowledgebase (Truth Mode)
TL;DR

StampDB is an embedded, append-only time-series store for NOR flash (4 KiB erase, 256 B program), with a host simulator and Pico 2 W firmware.
It writes 256 B pages as: up to 224 B payload then a 32 B header (header-last commit). CRC32C covers the 224 B payload; headers/footers have their own CRC.
Compression: per-block Fixed16 quantization (bias/scale f32), timestamp deltas u8 or u16; blocks are closed early to fit 224 B payload.
Recovery: loads newest valid A/B snapshot (host files or Pico LittleFS), uses a head hint, probes tail page-by-page and truncates after the first invalid page. Loses at most the last partial block.
GC: circular reclaim oldest; watermarks: warn <10% free, busy <5% free; ≤2 segments/sec quota; default blocking reclaim; non-blocking path returns EBUSY (not used by writer).
Pico: Core1 runs DB; Core0 handles USB-serial; flash ops in SRAM; metadata on LittleFS. Host sim uses flash.bin + metadata files in repo root.
Mini Glossary
Segment: 4 KiB chunk of the ring. Last 256 B page is the segment footer.
Page: 256 B programmable unit. One block/page max. Payload ≤224 B + header 32 B.
Header-last: Write payload first, then header bytes at page tail to atomically publish the block.
Zone map: In-RAM array of segment summaries: time range, block count, series bitmap.
Epoch wrap: 32‑bit timestamp wrap detection; epoch*id increments when ts jumps backward by >2^31.
SPSC FIFO: Single-producer/single-consumer FIFO over pico_multicore between Core0 and Core1.
Architecture at a Glance
PC (host)
app/tests/tools ──> libstampdb (host sim) ──> flash.bin + meta_snap*\*.bin + meta_head_hint.bin

Pico (RP2350)
Core0 (App/Serial) <---- SPSC FIFO ----> Core1 (StampDB)
| |
USB CDC flash read/erase/program (\_\_not_in_flash_func)
LittleFS (A/B snapshots + head_hint)

Data ring (raw flash) Metadata (host files / Pico LittleFS)
write/read/recovery/GC snapshots + head pointer
Where things run:

Write/read/recovery/GC: inside the core library (src/stampdb.c, src/ring.c, src/read_iter.c).
Metadata: host via atomic files; Pico via LittleFS (reserved 32 KiB at top of flash).
Dual-core (Pico): Core1 executes DB; Core0 parses serial commands and forwards via FIFO.
On‑Flash Schema (Truth)
Geometry and constants

Segment size: 4096 bytes.
Page size: 256 bytes.
Pages per segment: 16.
Data pages per segment: 15 (last page is footer).
Max payload per page: 224 bytes (so header ≤32 bytes fits within 256 B page).
Flash Map (overall)
[ Data Ring (0 .. flash_size - META_RESERVED - 1) ] [ LittleFS (META_RESERVED = 32 KiB) ]
One Segment (4 KiB)
data page[0] ... data page[13] data page[14] | footer page[15] (CRC'd SegmentFooter)
|<--------- 15 x 256B ---------->| 256B | 256B
One Data Page (256 B)
[ payload (≤224 B) ][ header (32 B) ]
(CRC32C over entire 224 B payload) (header has its own CRC field)
Data Payload (Truth)
Layout: deltas then qvals. There is no payload preamble; bias/scale/t0/count/dt_bits live in the 32‑B header.
Deltas: either uint8[count] or uint16[count] depending on dt_bits.
Samples: int16[count] quantized values.
Unused bytes: filled with 0xFF to reach 224 B.
Drift vs SPEC §5:

SPEC expects a payload preamble (bias, scale, t0_ms, flags, count). Implementation stores these in the header, not in payload (see “Block Header” below).
Block Header (32 B, header‑last)
Magic: STAMPDB_BLOCK_MAGIC = 'BLK1' = 0x424C4B31
CRC: header_crc = CRC32C(header bytes 0..27)

Field Size Units Meaning CRC covers? Write order
magic 4 u32 0x424C4B31 ('BLK1') Yes (2) header
series 2 u16 Series id (0..255 used) Yes
count 2 u16 Number of samples in this block Yes
t0_ms 4 u32 Base timestamp for deltas Yes
dt_bits 1 u8 8 or 16 (delta lane width) Yes
pad 3 bytes 0xFF Yes
bias 4 f32 Quantization bias Yes
scale 4 f32 Quantization scale (>0; clamp to 1e-9) Yes
payload_crc 4 u32 CRC32C over 224‑B payload Yes
header_crc 4 u32 CRC32C(header[0..27]) n/a (value)
Commit order:

Program entire 256‑B page with payload || 0xFF..0xFF header space.
Program page again with header bytes in tail (NOR 1→0 ensures safe header‑last atomic publish).
Drift vs SPEC §6:

SPEC’s header format (magic 'SB', version, t_min/max, etc.) is not implemented. The actual header above is truth.
Segment Footer (256 B)
Magic: STAMPDB_FOOTER_MAGIC = 'SFG1' = 0x53464731
CRC: crc = CRC32C(footer struct with crc=0)

Field Size Units Meaning CRC covers?
magic 4 u32 0x53464731 ('SFG1') Yes
seg_seqno 4 u32 Monotonic segment sequence number Yes
t_min 4 u32 Min timestamp over blocks in segment Yes
t_max 4 u32 Max timestamp Yes
block_count 4 u32 Number of committed blocks Yes
series_bitmap 32 bytes 256‑bit bitmap of series presence Yes
crc 4 u32 CRC32C of struct with crc=0 n/a
Placement: last page of segment; written exactly once at segment rollover by scanning the segment’s data pages.

Drift vs SPEC §7:

SPEC footer includes version and reserved fields with magic 'SCMT' and different field set. Implementation above is truth.
Snapshot (Metadata)
Host (files): meta_snap_a.bin, meta_snap_b.bin (repo root)
Pico (LittleFS): snap_a, snap_b (in reserved LFS volume)

Field Size Units Meaning CRC covers?
version 4 u32 =1 Yes
epoch_id 4 u32 Epoch disambiguation for ts wrap Yes
seg_seq_head 4 u32 Current head segment sequence Yes
seg_seq_tail 4 u32 Oldest retained segment sequence Yes
head_addr 4 u32 Absolute address of next free page Yes
crc 4 u32 CRC32C of struct with crc=0 n/a
Drift vs SPEC §8:

SPEC SnapshotV1 fields differ (magic, version, snap_seqno, ring_head_seq/tail_seq). Implementation above is truth.
Head Hint (Metadata)
Host: meta_head_hint.bin (repo root)
Pico: head_hint (LittleFS)

Field Size Units Meaning CRC covers?
addr 4 u32 Absolute head address hint Yes
seq 4 u32 Head segment sequence hint Yes
crc 4 u32 CRC32C over struct with crc=0 n/a
Data Flow (Step-by-Step)
Write path

Builder (in RAM) collects samples for a series into a block.
Quantization: computes bias=(min+max)/2, scale=(max-min)/65535 (min clamp 1e-9); quantizes q=round((v-bias)/scale) to int16.
Deltas: chooses u8 if all dt≤255 else u16; if adding the next sample would exceed 224 B payload budget, closes the block early.
Encode payload: write deltas (u8/u16) then qvals (int16), fill to 224 B with 0xFF; compute CRC32C(224 B).
Header-last commit:
Program payload into page with header space set to 0xFF.
Program header bytes at tail; 1→0 programming ensures atomic publish.
Live zone-map updated (t_min/t_max/block_count/series bitmap).
Head hint saved every 64 blocks or 2 seconds (whichever first).
If segment data pages are full, finalize segment:
Scan pages, compute t_min/t_max, block_count, series bitmap; write footer page.
Erase next segment, bump head seqno, reset page index.
Read path

Query iterator uses zone-map to skip segments where:
No blocks present, series bit not set, or time range doesn’t overlap (wrap-aware).
For each candidate page:
Reads payload (224 B) and header (32 B) separately.
Verifies header magic + header CRC; verifies payload CRC.
If series matches, decodes payload into SoA buffers; reconstructs times by t = t0 + delta.
Iterator yields rows in range [t0..t1], with constant RAM (per-block buffers).
Recovery

On open:
Build zone-map by scanning segment footers (valid CRC/magic). If none found:
Consider segment 0 as seqno=1; scan pages forward to create minimal summary.
If a snapshot loads successfully: set head addr/page index from snapshot; set tail seq and epoch.
Attempt to load a head hint; if valid and inside usable space, use it to position head.
Probe the current head segment’s pages, scanning forward:
Accept only CRC-clean pages with valid headers; stop at the first invalid page.
Truncate (virtually) after the last valid; if at least one valid existed before the first invalid, increment recovery_truncations.
Guarantees: At most the very last partial block can be lost.
GC & retention

Circular reclaim of oldest segments when free percentage falls below watermarks.
Watermarks: warn <10% free; busy <5% free (counters increment; warn/busy visible via stats).
Quota: ≤2 segments/sec. On quota exhaustion:
Default is blocking (writer calls GC in blocking mode). A non-blocking caller would get EBUSY.
Reclaimed segment is erased and its summary reset.
Backpressure

Default write path blocks under pressure (no EBUSY exposed to the API in current writer usage).
Compression Model (Truth)
Value quantization: Fixed16 per block (bias and scale in header).
Deltas: u8 or u16 chosen per block based on max delta observed so far in the block.
Block closure: Close before exceeding payload 224 B budget; close when deltas would overflow u16 (dt>65535) or if int16 quantization would overflow.
Error bound: ≤ scale/2 by rounding quantization.
Payload encoding:
deltas lane (u8×N or u16×N)
qvals lane (int16×N)
zero-fill (0xFF) to exactly 224 B
Drift vs SPEC §5:

SPEC places bias/scale/t0/count in payload preamble; implementation places these in the 32‑B header and computes payload CRC over only the 224 B payload.
Public API Cheat Sheet
Signatures (from include/stampdb.h)

Function Purpose Inputs Outputs Typical caller Blocking
stampdb_open(stampdb_t \**db, const stampdb_cfg_t *cfg) Open DB; recover ring; allocate buffers in workspace cfg (workspace ptr/size; read batch rows; commit interval) db handle Host tools, Pico app Yes (scans footers; may read flash)
stampdb_close(stampdb_t *db) Close; release in‑RAM state db — Host tools, Pico app No
stampdb_write(stampdb_t *db, uint16_t series, uint32_t ts_ms, float value) Append sample series, ts_ms, value STAMPDB_OK/… Host/Pico Yes; GC may block under quota
stampdb_flush(stampdb_t *db) Force commit of current block db rc Host/Pico Yes
stampdb_query_begin(stampdb_t *db, uint16_t series, uint32_t t0_ms, uint32_t t1_ms, stampdb_it_t *it) Start iterator series, t0, t1 it initialized Tools/app No
stampdb_next(stampdb_it_t *it, uint32_t *ts_ms, float *val) Get next row it row returned as (ts,val) Tools/app No
stampdb_query_end(stampdb_it_t *it) End iterator it — Tools/app No
stampdb_query_latest(stampdb_t *db, uint16_t series, uint32_t *out_ts_ms, float *out_value) Latest row series (ts,val) Tools/app No
stampdb_snapshot_save(stampdb_t *db) Save A/B snapshot db rc Tools/app Yes (metadata write)
stampdb_info(stampdb_t *db, stampdb_stats_t \*out) Get stats db seg_seq_head/tail, blocks_written, crc_errors, gc_warn_events, gc_busy_events, recovery_truncations Tools/app No
Examples (snippets)

// Open & write one sample
static unsigned char ws[128*1024];
stampdb_t \*db = NULL;
stampdb_cfg_t cfg = { ws, sizeof(ws), 256, 0 };
stampdb_open(&db, &cfg);
stampdb_write(db, 1, 0, 42.0f);
stampdb_flush(db);

// Iterate a range
stampdb*it_t it;
if (stampdb_query_begin(db, 1, 0, 1000, &it) == STAMPDB_OK) {
uint32_t ts; float val;
while (stampdb_next(&it, &ts, &val)) { /* use ts,val */ }
stampdb_query_end(&it);
}
stampdb_close(db);
File Map (What Each File Owns)
Path Responsibility Key functions Called by
include/stampdb.h Public API signatures and types all API tools, Python, Pico app
src/stampdb_internal.h Internal constants/types/glue crc32c decl; block/seg types; platform/meta prototypes all src files
src/codec.c Payload encoder/decoder; header pack/unpack codec_encode_payload, codec_decode_payload, codec_pack_header, codec_unpack_header writer, iterator
src/crc32c.c CRC32C (Castagnoli) crc32c codec, ring
src/stampdb.c API impl; workspace management; builder stampdb_open/close/write/flush/snapshot_save/info external callers
src/ring.c Ring write/recover/GC/footer ring_write_block, ring_finalize_segment_and_rotate, ring_scan_and_recover, ring_gc_reclaim_if_needed stampdb.c
src/read_iter.c Iterator + latest stampdb_query_begin/next/end, stampdb_query_latest tools/app
src/meta_lfs.c Metadata: snapshots + head hint meta_load/save_snapshot, meta_load/save_head_hint stampdb.c/ring.c
sim/flash.c Host NOR flash emulation sim_flash_read/erase/program/size sim platform
sim/platform_sim.c Host glue: millis + sim bindings platform_millis, platform_flash*_ core
platform/pico/platform*pico.c Pico glue: SRAM flash ops platform_flash*_, platform_millis core
platform/pico/main.c Pico app (Core0/1, serial) FIFO command handlers; serial parser firmware
platform/pico/CMakeLists.txt Pico build; fetch SDK/LittleFS targets CMake
tools/stampctl.c CLI exporter + retention tool export, retention user
tests/\*.c CTest suite per current impl powercut, recovery, codec, exporter, timing, GC CI/local
Build & Environment
Host build (presets)

Configure: cmake --preset host-debug
Build: cmake --build --preset host-debug -j
Test: (cd build/host-debug && ctest --output-on-failure)
Host build (Makefiles explicit)

cmake -S . -B build/mk -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/mk -j
(cd build/mk && ctest --output-on-failure)
Dependencies

CMake ≥ 3.20; a C11 compiler.
Pico SDK (fetched via CMake for Pico preset or provide PICO_SDK_PATH).
LittleFS (fetched and linked as static lib for Pico metadata).
Optional/Dev

Python 3 for ctypes and pyserial clients (host side).
pyserial if using py/stampdb_serial.py.
▶ Run on PC (with example data)
Clean DB (optional)

rm -f flash.bin meta_snap_a.bin meta_snap_b.bin meta_head_hint.bin
Configure & build

cmake --preset host-debug && cmake --build --preset host-debug -j
Generate data (Python one-liner)

python3 - <<'PY'
from py.stampdb import StampDB
db=StampDB()
for i in range(100): db.write(1, i*10, i*0.1)
db.flush()
print('latest:', db.latest(1))
PY
Export CSV

./build/host-debug/stampctl export --series 1 --t0 0 --t1 1000 --csv | head
Notes

Simulator files live in repo root: flash.bin, meta_snap_a.bin, meta_snap_b.bin, meta_head_hint.bin.
Flash size override: STAMPDB_SIM_FLASH_BYTES=8388608 (8 MiB) before running.
▶ Flash on Pico (with example data)
Configure & build UF2

cmake --preset pico2w-release
cmake --build --preset pico2w-release
Output UF2: build/pico2w-release/platform/pico/stampdb_pico_fw.uf2

Flash (BOOTSEL)

Hold BOOTSEL, plug Pico to USB.
Drag‑and‑drop the UF2 to the mass storage device.
Serial interaction (USB CDC)

Baud: default SDK; use a terminal (screen, miniterm, etc.).
Commands:
w <series> <ts_ms> <value> (write)
f (flush)
s (snapshot)
l <series> → prints OK <ts_ms> <value>
e <series> <t0> <t1> → streams ts,value lines, then END
Dual-core truth: Core1 runs DB; Core0 parses serial and forwards via FIFO.
Operations Runbook
Snapshots

Host: A/B files in repo root; atomic rename. Use stampdb_snapshot_save() or s on Pico serial.
Pico: LittleFS files snap_a/snap_b; rename-atomic; CRC validated.
Export ranges

Host: stampctl export --series S --t0 T0 --t1 T1 [--csv|--ndjson]
Pico: e S T0 T1 over serial; prints CSV lines then END.
Retention estimate

stampctl retention --days D prints rough capacity and target rows/day.
Wipe/reset

Host: delete flash.bin and meta files.
Pico: reflash firmware; (Unknown) — dedicated wipe command not implemented; could be added to serial.
Troubleshooting

Symptom Probable cause Fix
Export prints only header No data in sim files Run a writer first (Python snippet above), then export
Recovery takes long No snapshots + many segments Save snapshots periodically; recovery time grows with segments since last snapshot
Test hangs Old build dir or wrong working dir rm -rf build/_; reconfigure; ensure tests run from repo root
EBUSY returned Non-blocking GC call under quota or <5% free Writer uses blocking mode; if calling non-blocking GC, retry later
CRC spikes Corrupted pages or injected faults Simulator loads from disk on read; verify flash.bin integrity; run tests
Missing flash.bin Working dir mismatch Ensure working dir is repo root; sim files are in repo root
Performance & RAM (Truth)
Ingest: One 256 B program per block commit (payload), plus a second program writing the header tail; one 4 KiB erase per segment rollover.
Read: CRC verification per page; SoA decode per block; zone-map reduces scans.
RAM: No heap growth after stampdb_open(). Workspace must hold:
stampdb_t + staging arrays (deltas/qvals/values for up to ~74 rows) + zone-map (seg_summary_t _ seg_count).
Zone-map size scales with number of segments (flash_bytes - META_RESERVED) / 4096.
Knobs: read_batch_rows (iterator buffering), commit_interval_ms (advisory), and (host only) STAMPDB_SIM_FLASH_BYTES.
Reliability Model (Truth)
Power-cut scenarios (tested)

Torn header: Page payload written but header missing/invalid → block ignored; reads truncate after last valid.
Torn payload: CRC mismatch → page ignored; iteration skips to next segment/page.
Torn footer: Footer missing/invalid → segment summary rebuilt on next finalize/recovery; data pages still used.
Loss bound: At most the last partial block.
CRC isolation

Iterator verifies header magic+CRC and payload CRC; corrupt pages are skipped and do not poison earlier pages.
Snapshot-bounded recovery

With snapshots: recovery time grows O(segments since last snapshot). Test asserts a linear bound with slack.
Implementation Status & Drift (Truth)
Status (actual implementation)

Timestamp wrap/epoch: Implemented. epoch_id increments when ts_ms jumps backward by >2^31; persisted in snapshots.
Metadata: Host A/B snapshots and head hint via atomic files; Pico uses LittleFS with tmp+rename.
Dual-core (Pico): Implemented. Core1 DB; Core0 serial; SPSC FIFO over pico_multicore.
Watermarks: Implemented (warn<10%, busy<5%), quota ≤2 seg/s; default blocking; non-blocking returns EBUSY.
Deep recovery: Footers scanned across ring; tail segment probed page-by-page; at most last partial block lost.
Tests (§13 style): Implemented set covering basic, codec RT, power-cuts (header/payload/footer), CRC isolation, exporter correctness, recovery-time bound, GC P99 latency.
Drift (SPEC/ARCH references)

SPEC §5 Payload preamble vs header fields: Drift (SHOULD) — bias/scale/t0/dt_bits/count live in the 32‑B header, not in the payload preamble; payload is just deltas + qvals + padding.
SPEC §6 BlockHdr fields/magic: Drift (SHOULD) — implementation uses magic 'BLK1' and a different header layout; no t_min/t_max/payload_len/version in header; instead includes bias/scale and payload_crc.
SPEC §7 SegmentFooter format: Drift (NICE) — implementation uses magic 'SFG1' and a simplified struct (no version/reserved fields).
SPEC §8 Snapshot & ring-head fields: Drift (NICE) — implementation has version, epoch_id, seg_seq_head, seg_seq_tail, head_addr and a separate head hint record; SPEC defines different names/magic and paths.
ARCH LittleFS paths: Drift (NICE) — implementation uses top-level snap_a, snap_b, head_hint files in a small reserved LFS, not the directory paths shown in SPEC examples.
Backpressure: As implemented, writer calls blocking GC; non-blocking mode exists only if ring GC is called with non_blocking=true. (Unknown if SPEC mandates non-blocking mode exposure in API.)
Severity tags

BLOCKER: None (implementation is coherent and passes its tests).
SHOULD: Align header/payload/footers to SPEC layouts if strict compatibility is required.
NICE: Align metadata struct names/paths; expose non-blocking GC control if needed by callers.
Technical Decisions & Trade-offs
Area Chosen Why Explored/Notes Trade-offs
Header-last commit Payload then header Power-cut safety on NOR Classic two-phase commit for NOR Requires full-page write twice, but safe
Bias/scale location In header Smaller payload; simpler decode SPEC puts preamble in payload Header carries more metadata
Deltas u8/u16 Per-block Dense for small dt; upgrade when needed Close early on overflow Two formats to handle at decode
Zone-map Full in RAM Fast range pruning Scales with segment count RAM proportional to flash size
Snapshots A/B + hint Yes Bound recovery time; fast head positioning Host files; Pico LFS Metadata complexity; small FS integration
GC quota ≤2 seg/s Smooth tails; avoid stalls Busy/warn counters in stats Potential blocking under pressure
Dual-core on Pico Core1 DB; Core0 serial Keep flash ops on one core; FIFO simplicity Serial export streaming added More code, but clearer core ownership
Security, Safety & Wear
NOR semantics: 1→0 programming enforced; page aligned (256 B) programming; 4 KiB sector erase.
Pico: flash program/erase run from SRAM (\_\_not_in_flash_func); XIP stalls avoided during critical ops.
CRC32C used for payload, headers, and footers; recovery accepts only CRC-clean pages.
Power-cut resilience validated via tests for torn header/payload/footer.
Extensibility & Integration
Adding a series: Just use a new series id (0..255 supported in bitmap).
Export formats: Extend tools/stampctl to add formats (e.g., Parquet/NDJSON improvements).
Codec variants: Plug in alternate payload encoders in src/codec.c and wire in builder.
Telemetry/MQTT: Hook in at tool/app layer; stampdb_info() exposes useful counters (GC warn/busy, recovery truncations).
FAQ & Glossary
Do I need an RTC? No. Timestamps are caller-provided ms; wrap handled via snapshots’ epoch_id.
Can I delete data? Not directly; retention is circular; oldest segments reclaimed first.
Single-core or dual-core? Host is single-threaded; Pico splits: Core1 DB, Core0 serial.
What is UF2? A drag‑and‑drop firmware file format used by Raspberry Pi Pico.
What’s the max series? Bitmap is 256 bits; series 0..255.
Where does data go on PC? flash.bin and meta files in the repo root working directory.
Appendix
ASCII diagrams

Page (256 B):
+------------------------------+-----------------+
| Payload (≤224 B) | Header (32 B) |
+------------------------------+-----------------+

Segment (4096 B):
+----------------------+ ... +----------------------+----------------------+
| data page[0] 256 B | ... | data page[14] 256 B | footer page[15] 256 B|
+----------------------+ +----------------------+----------------------+
Constants table (as coded)

STAMPDB_SEG_BYTES = 4096
STAMPDB_PAGE_BYTES = 256
STAMPDB_PAYLOAD_BYTES = 224
STAMPDB_HEADER_BYTES = 32
STAMPDB_SERIES_BITMAP_BYTES = 32
STAMPDB_MAX_SERIES = 256
STAMPDB_BLOCK_MAGIC = 'BLK1'
STAMPDB_FOOTER_MAGIC = 'SFG1'
STAMPDB_META_RESERVED = 32768 (Pico; LittleFS region)
CLI reference

stampctl export --series S --t0 T0 --t1 T1 [--csv|--ndjson]
stampctl retention --days D
Test inventory

basic: writes/reads sanity; latest monotonic.
codec: delta/value round-trip; header pack/unpack.
recovery: torn header; ensure earlier rows survive.
powercut_matrix: torn header/payload/footer scenarios; ensure non-zero reads.
crc_isolation: corrupt middle page, early data still readable.
exporter: deterministic data; CLI export produces lines.
recovery_time: reopen time bounded by c1 + c2·segments since last snapshot.
gc_latency: P99 write latency under quota cap.
Unknowns (how to verify)

Board-specific flash size on Pico (currently assumes 2 MiB): check/configure PICO_FLASH_SIZE_BYTES in platform glue or CMake toolchain.
Serial export throughput limits: profile over large ranges (add tests or host capture).
