Title: StampDB — Full-Spec Build Contract (No Stubs)

You are implementing StampDB full spec for RP2350 / Pico 2 W. Deliver a complete, buildable repository with host simulator + Pico firmware, tests, and tools, matching README.md, ARCHITECTURE.md, and SPEC.md in this workspace. No TODOs, no placeholders.

Must implement (all)

Write path: 4 KiB segments; each 256 B page = payload then header-last; CRC32C on payload; segment commit footer with seg_seqno, {t_min,t_max}, block_count, 256-bit series bitmap, footer CRC.

Compression: per-block Fixed16 quantization with bias, scale (f32), timestamp deltas (u8/u16); close block early on overflow; target payload 224 B, header ≤32 B.

Timestamps: u32 ts_ms + epoch_id (in snapshots) to handle wrap.

Metadata: A/B snapshots in LittleFS using rename-atomic; also persist a small ring head pointer updated every 64 blocks or 2 s (whichever first).

Recovery: load newest valid A/B snapshot; probe last segment tail; truncate partial block; if snapshot missing, rebuild summaries from commit footers. Lose at most the last partial block.

Read path: zone-map skipping by {t_min,t_max} + series bitmap; verify CRC; decode full payload into SoA batches (time[], value[]) of 256/512 rows; iterator yields all rows in range with constant RAM.

GC & retention: circular reclaim oldest; quota ≤2 seg/s; watermarks 10 % warn / 5 % busy; default block/delay, optional non-blocking (EBUSY).

RAM discipline: presets Tight ≈36 KiB / Comfy ≈64 KiB; no heap growth after open(); enforce via runtime checks.

Pico flash/XIP safety: all flash erase/program from SRAM (\_\_not_in_flash_func), obey 4 KiB erase / 256 B program.

Dual-core (Pico): Core0=app, Core1=DB via pico_multicore FIFO (SPSC). Host sim can be single-threaded.

Host simulator: flash.bin NOR emulation (1→0 program, 4 KiB erase); CSV/NDJSON exporter by range.

CLI tools: tools/stampctl export --series S --t0 --t1; retention calculator.

Tests: CTest covering power-cut matrix (torn payload/header/footer), CRC corruption isolation, bounded recovery by snapshots, codec round-trip tolerance, and GC quota not breaking P99 ingest. All tests must pass.

Deliverables

Language: C11 only.

Build: CMake presets for host and pico2_w; .vscode tasks/launch.

Layout:
include/ API · src/ core (codec, write, read, recovery, crc32c, util) · src/meta_lfs.c (LittleFS meta) · platform/pico/ port · sim/ host sim · tools/ CLI · tests/ CTest.

Docs: README.md (short), ARCHITECTURE.md (flows), SPEC.md (formats & constants).

Style: .clang-format, .editorconfig.

No stubs.

API (must match)
typedef struct stampdb stampdb_t;
typedef enum { STAMPDB_OK=0, STAMPDB_EINVAL, STAMPDB_EBUSY, STAMPDB_ENOSPACE, STAMPDB_ECRC, STAMPDB_EIO } stampdb_rc;

typedef struct {
void\* workspace; uint32_t workspace_bytes;
uint32_t read_batch_rows; // 256/512
uint32_t commit_interval_ms;// 0=size-only
} stampdb_cfg_t;

stampdb_rc stampdb_open(stampdb_t \**db, const stampdb_cfg_t *cfg);
void stampdb_close(stampdb_t \*db);

stampdb_rc stampdb_write(stampdb_t *db, uint16_t series, uint32_t ts_ms, float value);
stampdb_rc stampdb_flush(stampdb_t *db);

typedef struct stampdb_it stampdb_it_t;
stampdb_rc stampdb_query_begin(stampdb_t *db, uint16_t series, uint32_t t0_ms, uint32_t t1_ms, stampdb_it_t *it);
bool stampdb_next(stampdb_it_t *it, uint32_t *ts_ms, float *val);
void stampdb_query_end(stampdb_it_t *it);

stampdb_rc stampdb_query_latest(stampdb_t *db, uint16_t series, uint32_t *out_ts_ms, float *out_value);
stampdb_rc stampdb_snapshot_save(stampdb_t *db);

typedef struct { uint32_t seg_seq_head, seg_seq_tail, blocks_written, crc_errors; } stampdb_stats_t;
void stampdb_info(stampdb_t _db, stampdb_stats_t_ out);

Definition of Done

cmake --preset host-debug && cmake --build && ctest → PASS.

cmake --preset pico2w-release && cmake --build → UF2 produced.

tools/stampctl export returns rows matching inputs (within quantization error).

Recovery loses at most last partial block; time = O(#segments since last snapshot).

No TODOs; all features above implemented.

Now generate the repository. Do not ask questions. Choose the simplest correct option if unspecified.
