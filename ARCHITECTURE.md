# ARCHITECTURE.md — StampDB Design & Flows (v1.0)

> **Design goals:** crash‑safe, flash‑aware, deterministic RAM, tiny API, and simplicity. Reuse native SDK features. Optimize only for noticeable wins.

---

## 1) Big picture

```
                +-----------------------------------------------+
 Core 0         |                 User Application               |
 (App)          |  Sensors / Control / UI / Networking          |
                +---------------------^-------------------------+
                                      | stampdb_write(ts,val)
                                      |
                         RP2350 multicore FIFO (hardware SPSC)
                                      |
                +---------------------v-------------------------+
 Core 1         |                 StampDB Engine                 |
 (DB)           |  Ingest  •  Flush  •  Query  •  GC            |
                +---------------------+-------------------------+
                                      |
                 +--------------------+-----------------------+
                 |                 Storage                   |
                 |  Raw Circular Log (data)  +  LittleFS (meta)
                 +--------------------+-----------------------+
```

- **Core split:** App on Core0 (drives `stampdb_write`), DB on Core1.
- **Link:** hardware FIFO (SPSC) for deterministic backpressure and minimal locking.

---

## 2) Storage layout

```
QSPI Flash (2–4 MiB total)
┌───────────────────────────────────────────────────────────────┐
│                       Raw Circular Log (~data)                │
│  [ Segment 0 ][ Segment 1 ] ... [ Segment N ]                 │
└───────────────────────────────────────────────────────────────┘
┌───────────────────────────────────────────────────────────────┐
│                        LittleFS (~meta)                       │
│  /db/index_A.snap   /db/index_B.snap   /db/ring_head.bin      │
│  /logs/diag.log     /health/wear.bin                          │
└───────────────────────────────────────────────────────────────┘
```

**Segment (4 KiB)**

```
Offset →                                           last 256 B
┌────────────┬────────────┬───────┬────────────┬───────────────┐
│ Page 0     │ Page 1     │ ...   │ Page 14    │ SegmentFooter │
│ (256 B)    │ (256 B)    │       │ (256 B)    │ (256 B)       │
└────────────┴────────────┴───────┴────────────┴───────────────┘
Page: [ payload ≤224 ][ BlockHdr ≤32 ]  (program once, header‑last)
```

---

## 3) Write path

```
Application (Core 0)
   |  stampdb_write(series, ts_ms, value)
   v
[HW FIFO]  (SPSC, backpressure aware)
   v
[Core 1] Ingest & Block Builder (RAM)
   - accumulate points (time‑clustered, no sort)
   - encode: Fixed16 + timestamp deltas
   - CRC32C over payload
   - if builder full or interval elapsed:
        1) write 256‑B page: payload at front
        2) write BlockHdr at page end (commits)
        3) update in‑RAM seg stats & bitmap
   - if segment full:
        - write SegmentFooter (once)
        - advance to next segment (circular)
        - optional GC under quota (≤2 seg/s)
```

**XIP‑stall safety:** on Pico, erase/program routines are **SRAM‑resident** (`__not_in_flash_func`). Metadata updates are **quota’d** and scheduled between bursts.

---

## 4) Read path (zone‑map guided, SoA batching)

```
stampdb_query(series, [t0,t1])
  |
  v
[Cursor Init]
  - load ring head/tail from snapshot & ring_head
  - walk candidate segments by time & series bitmap
  |
  v
for each segment:
  for each page:
    - read BlockHdr (t_min/t_max, series)
    - SKIP pages outside [t0,t1] or wrong series
    - read payload; verify CRC
    - decode into SoA batch: time[], value[] (256/512 rows)
    - yield rows to caller
```

**Why SoA?** Vector‑friendly scans, minimal cache thrash, fits cleanly in bounded buffers.

---

## 5) Recovery (bounded time)

```
Boot → mount LittleFS → read A/B snapshots (pick newest valid)
     → read ring_head.bin (fast‑forward hint)
     → probe the tail segment:
         scan forward 256‑B pages
         accept only valid headers + CRC‑clean payloads
         stop at first invalid header → tail placed here
     → if snapshot missing: rebuild summaries from segment footers
     → ready
```

**Guarantee:** at most the **last partial block** is lost. Recovery time is **O(#segments since last snapshot)**.

---

## 6) Backpressure, retention, GC

- **Watermarks**: warn at **10%**, busy at **5%** free space.
- **Backpressure**: default block/delay; optional non‑blocking returns `EBUSY`.
- **Retention**: circular overwrite (oldest segments first).
- **GC quota**: max **2 segments/second** reclaimed under pressure to protect P99.

---

## 7) Compression & quantization

- **Per‑block** `bias, scale` (float32).
- Values stored as **int16**: `q = round((v - bias)/scale)`.
- Timestamps as deltas from `t0_ms` (u8 or u16).
- Close block early if int16 overflow or deltas overflow.

**Error bound:** ≤ `scale/2`. Choose `scale = span/32767` for span = `vmax - vmin`.

---

## 8) Concurrency & scheduling

- **Core0 (App):** sampling, UI, networking; pushes points to FIFO.
- **Core1 (DB):** periodic timers drive builder flushes, snapshot cadence, and ring‑head updates.
- **LittleFS ops:** short bursts; avoid long sequences during heavy ingest.
- **ISR‑safe:** `stampdb_write` SHOULD be callable from a soft‑IRQ context if the FIFO path is used (no dynamic allocation, no blocking in ISR).

---

## 9) Diagnostics & health

- `/health/wear.bin`: coarse erase‑count histogram and retired segments.
- `stampdb_info()`: `seg_seq_head/tail`, `blocks_written`, `crc_errors`.
- UART/USB shell: `stats`, `export t0 t1`, `snapshot`, `gc-now` (optional).

---

## 10) Failure model

- **Power loss during payload:** ignored (no header).
- **Power loss during header:** page discarded (CRC/header invalid).
- **Power loss during footer:** segment remains valid; footer rewritten on next rollover.
- **Metadata corruption:** A/B snapshot fallback; rebuild from footers.
- **CRC error in payload:** iterator skips the block; rest unaffected.

---

## 11) Build & targets

- **Host**: flash simulator (`flash.bin`), CLI tools, CTest.
- **Device**: Pico 2 W firmware; flash ops run from SRAM; dual‑core split; USB CDC shell optional.

---

## 12) Why this architecture (short)

- **Append‑only pages** → predictable latency, easy torn‑write handling.
- **Zone maps** (t_min/t_max) → tiny RAM, fast skipping.
- **LittleFS‑only for meta** → wear‑leveling & atomic rename without data‑path jitter.
- **SRAM‑resident flash ops** → avoid XIP stalls during program/erase.
- **SoA batching** → efficient scans in tiny buffers.

---

## 13) Trade‑offs (at a glance)

| Area          | Chosen                    | Why                   | Explored          | Trade‑off              |
| ------------- | ------------------------- | --------------------- | ----------------- | ---------------------- |
| Storage split | Raw log + LittleFS meta   | Predictable data path | FS for everything | FS GC jitter on ingest |
| Block/page    | 256 B page w/ header‑last | Match program unit    | 512–1 KiB blocks  | Larger loss per tear   |
| Segment size  | 4 KiB                     | Match erase unit      | 8–16 KiB          | Coarser GC             |
| Indexing      | Footer + block headers    | Tiny RAM              | Global tree       | Complexity, RAM        |
| Compression   | Fixed16 + deltas          | Low CPU, good ratio   | Gorilla‑lite      | More CPU/RAM           |
| Backpressure  | Block/delay default       | Correctness           | Drop‑newest       | Data loss risk         |

---

## 14) Road‑test checklist

- Power‑cut harness tests across payload/header/footer.
- Long‑soak ingest near watermarks; verify GC quota.
- Export then verify CSV/NDJSON round‑trip.
- Wi‑Fi bursts + ingest; ensure jitter within budget.
