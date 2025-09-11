# RAM_BUDGET.md — Deterministic Memory Plan (v1.0)

> **Principle:** Use ≤ **10–15%** of 520 KiB SRAM. Presets: **Tight ≈36 KiB** (\~6.9%), **Comfy ≈64 KiB** (\~12.3%). No heap growth after `stampdb_open()`.

---

## 1) Memory layout (conceptual)

```
Workspace (caller‑provided)
┌─────────────────────────────────────────────────────────┐
│ Control & stats (struct stampdb, cursors)          ~3 KiB│
│ Block builder (payload staging + deltas + qvals)   ~6 KiB│
│ Codec workspace (Fixed16/Delta)                    ~4 KiB│
│ Read SoA batch (time[], value[], bitmap)           ~8 KiB│
│ Segment summaries / hot footers cache              ~8 KiB│
│ Recovery scratch                                   ~3 KiB│
│ Guard & alignment                                   ~4 KiB│
└─────────────────────────────────────────────────────────┘
```

> Sizes are upper‑bounds; exact numbers depend on preset & batch rows.

---

## 2) Presets (deterministic caps)

### Tight (\~36 KiB)

| Component                 |     Size | Notes                                            |
| ------------------------- | -------: | ------------------------------------------------ |
| Control + stats           |    3 KiB | DB handle, counters, cursors                     |
| Block builder             |  2–3 KiB | single buffer, u8/u16 deltas, qvals staging      |
| Codec workspace           |    4 KiB | quantization helpers                             |
| Read batch (SoA 256 rows) |  3–4 KiB | `uint32 time[256]` + `float value[256]` + bitmap |
| Segment summaries         | 8–10 KiB | ring head/tail, recent footers                   |
| Recovery scratch          |    3 KiB | probe buffers                                    |
| Stacks (2 cores)\*        |  \~8 KiB | outside workspace; reserved in plan              |
| Misc/guard                |  \~2 KiB | alignment, future headroom                       |

### Comfy (\~64 KiB)

| Component                 |      Size | Notes                  |
| ------------------------- | --------: | ---------------------- |
| Control + stats           |     3 KiB |                        |
| Block builder             |   4–6 KiB | optional double‑buffer |
| Codec workspace           |     4 KiB |                        |
| Read batch (SoA 512 rows) |   6–8 KiB | larger arrays          |
| Segment summaries         | 12–16 KiB | deeper cache           |
| Recovery scratch          |     3 KiB |                        |
| Stacks (2 cores)\*        |   \~8 KiB |                        |
| Misc/guard                |   \~4 KiB |                        |

\* Stacks are typically reserved outside the DB workspace but included here for discipline.

---

## 3) Runtime guard (example)

```c
// in stampdb_open()
const uint32_t MIN_TIGHT = 36 * 1024;
const uint32_t MIN_COMFY = 64 * 1024;
if (cfg->workspace_bytes < MIN_TIGHT) return STAMPDB_ENOSPACE; // or EINVAL
// layout internal pools here without mallocs
```

Or compile‑time:

```c
// sizes derived from cfg -> pack into a single static arena
_Static_assert(sizeof(struct stampdb) <= 4096, "DB control must stay tiny");
```

---

## 4) Tuning knobs that affect RAM

- **read_batch_rows** (256/512).
- **index cache depth** (recent footers/segment summaries).
- **double‑buffering** for builder (off in Tight).
- **optional codecs** (e.g., Gorilla‑lite adds +4–8 KiB; **off by default**).

---

## 5) Example workspace sizing

### Tight preset (typical)

```c
static uint8_t workspace[36*1024];
stampdb_cfg_t cfg = {
  .workspace = workspace,
  .workspace_bytes = sizeof(workspace),
  .read_batch_rows = 256,
  .commit_interval_ms = 0,
};
```

### Comfy preset (typical)

```c
static uint8_t workspace[64*1024];
stampdb_cfg_t cfg = {
  .workspace = workspace,
  .workspace_bytes = sizeof(workspace),
  .read_batch_rows = 512,
  .commit_interval_ms = 0,
};
```

---

## 6) What stays out of RAM

- **No global trees or large indexes.** Only per‑segment footers + per‑block headers are consulted.
- **No heap growth after `open()`**. All buffers pre‑allocated; sizes are deterministic.
- **No filesystem buffers.** Metadata uses a raw reserved flash region with fixed‑size records; the data path avoids any FS.

---

## 7) Monitoring

- Expose `stampdb_info()` counters to the app for telemetry.
- Optional metrics: max builder payload, blocks/segment, GC activity, snapshot cadence effectiveness.
