# StampDB Architectural Blueprint

## 1. Overview

**StampDB** is a high-performance, crash-safe, embedded time-series database meticulously engineered for resource-constrained microcontrollers like the Raspberry Pi Pico 2 W (RP2350).

Its design prioritizes:

- **Reliability:** Guarantees against data loss, even during sudden power failure.
- **Predictability:** Operates within a fixed, deterministic RAM budget and ensures a fast, bounded boot time.
- **Performance:** Optimized for high-volume, sequential writes, ensuring maximum flash memory lifespan.
- **Simplicity:** Provides a clean, minimal C API, prefixed with `stampdb_`, that is easy to integrate and understand.

## 2. Core Architecture: A Dual-Core, Log-Structured Design

StampDB's architecture is built on two foundational pillars: a sophisticated storage engine inspired by professional databases and a hardware-aware process model that leverages the dual-core nature of the RP2350.

### 2.1. Process & CPU Layer

To ensure the database never interferes with the main application's real-time tasks, StampDB employs a strict dual-core pipeline.

text
Core 0 (Application) Core 1 (StampDB)
┌─────────────────────────┐ ┌────────────────────────────┐
│ User Application │ │ StampDB Engine │
│ (Sensors, Logic, UI) │ │ (Write, Compact, Query) │
└─────────────────────────┘ └────────────────────────────┘
│ ▲
│ stampdb_write(ts, val) │
│ │
▼ │
┌─────────────────────────┐ │
│ Hardware SPSC FIFO │ multicore_fifo_pop_blocking()
│ (Pico SDK Multicore) │<────────────────────
└─────────────────────────┘

text

| Decision & Rationale                                                                                                                                                                                                              | Alternatives Explored                                                                                                                                                                                                                                          | Trade-offs                                                                                                                                                                                                                                                                                |
| :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Dedicated Database Core (Core 1):** We pin the entire StampDB engine to Core 1. This isolates all slow I/O operations (flash writes) from the main application, guaranteeing that Core 0 remains free for time-sensitive tasks. | - **Single-Core with Interrupts:** Running everything on one core would add complexity (managing IRQ priorities) and could lead to missed real-time deadlines. <br>- **Cooperative Multitasking:** Less predictable and can still result in blocking behavior. | - **Increased Code Complexity:** Requires careful use of inter-core communication primitives. <br>- **Benefit:** This complexity is justified by the massive gain in real-time performance and system stability.                                                                          |
| **Pico SDK Hardware FIFO:** We use the native `pico/multicore.h` library for inter-core communication.                                                                                                                            | - **Custom Ring Buffer:** Building our own lock-free ring buffer is possible but adds risk and complexity.                                                                                                                                                     | - **Slight Overhead:** The SDK's implementation may have a minor overhead compared to a perfectly optimized custom solution. <br>- **Benefit:** We gain a battle-tested, hardware-specific, and officially supported communication channel for free, saving significant development time. |

## 3. The Write Path: Optimized for Speed and Longevity

The write path is designed to be as fast and efficient as possible, transforming many small application writes into a few large, sequential flash operations.

text
┌─────────────────────────────────────────────────────────┐
│ Core 1 │
┌──────────┐ stampdb_write() ┌──────────┐ multicore_fifo_pop() ┌──────────────────────┐ │
│ User App │───────────────────> │ HW FIFO │ ───────────────────────> │ Memtable (4KB RAM) │ │
└──────────┘ └──────────┘ └─┬────────────────────┘ │
│ (Buffer is full) │
│ │
▼ │
┌──────────────────────────┐ │
│ Compress (Gorilla/Delta) │ │
└───────────┬──────────────┘ │
│ │
▼ │
┌──────────────────────────┐ │
│ Calculate CRC32 │ │
└───────────┬──────────────┘ │
│ │
▼ │
┌──────────────────────────┐ │
│ Write SSTable to LittleFS│ │
└──────────────────────────┘ │
└─────────────────────────────────────────────────────────┘

text

## 4. The Read Path: Efficient and Memory-Safe

The read path is designed to satisfy queries quickly while using a minimal and constant amount of RAM, regardless of the query range.

text
┌───────────────────────────────────────────────────────────────┐
│ Core 1 │
┌──────────┐ stampdb_query_range() ┌───────────────────┐ ┌──────────────────────────────┐ │
│ User App │────────────────────────>│ StampDB Engine ├─────>│ Sparse Index (RAM) │ │
└──────────┘ └───────────────────┘ └───────┬──────────────────────┘ │
│ (Finds relevant files) │
│ │
▼ │
┌───────────────────────────────────────────────────────────────┐
│ Query Iterator │
│ ┌───────────────────────────────────────────────────────┐ │
│ │ while (user calls stampdb_iterator_next()) : │ │
│ │ 1. Read next compressed block from SSTable (Flash) │ │
│ │ 2. Verify CRC32 of block │ │
│ │ 3. Decompress block into small buffer (RAM) │ │
│ │ 4. Return next point to user │ │
│ └───────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────┘

text

## 5. Detailed Technical Decisions

### 5.1. Filesystem and On-Flash Layout

The foundation of StampDB's reliability is its underlying filesystem.

| Decision & Rationale                                                  | Alternatives Explored                                                                                                                                                               | Trade-offs                                                                                                                                                                                                                                                                                                               |
| :-------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Use LittleFS:** We build StampDB on top of the LittleFS filesystem. | - **FAT Filesystem:** Not power-fail safe and lacks wear leveling. Unsuitable for reliable embedded systems.<br>- **Custom Flash Abstraction Layer:** Hugely complex and high-risk. | - **Performance Overhead:** LittleFS has a small performance and memory overhead compared to writing to raw flash.<br>- **Benefit:** We instantly inherit professional-grade **wear leveling**, **bad block management**, and **power-loss safety**, saving months of development and drastically reducing project risk. |

#### Flash Layout (within LittleFS)

/ (LittleFS Root)
├── db/
│ ├── data/
│ │ ├── 1662547200.sst
│ │ ├── 1662547560.sst
│ │ └── ...
│ └── index/
│ └── snapshot.idx

text

- **`data/`**: Contains the compressed and checksummed time-series data blocks (SSTables).
- **`index/`**: Contains the periodic snapshot of the in-memory sparse index, enabling a fast boot time.

### 5.2. Data Compression

| Decision & Rationale                                                                                                                                                                                                                  | Alternatives Explored                                                                                                                                                                           | Trade-offs                                                                                                                                                                                                                                                         |
| :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Use Gorilla/Delta-of-Delta Compression:** We will borrow a lightweight C implementation of this algorithm. It works by storing the "delta-of-delta" for timestamps (which are often very regular) and XORing floating-point values. | - **No Compression:** Simple, but would drastically reduce storage capacity. <br>- **General-Purpose (e.g., zlib):** Not optimized for time-series data and has a much higher RAM/CPU overhead. | - **CPU Cost:** Adds a small computational cost to each write (~500 cycles/point). <br>- **Benefit:** Achieves compression ratios of 3–12x, massively increasing the amount of data we can store. This is a crucial feature for long-running logging applications. |

### 5.3. RAM Calculation (Deterministic Budget)

The database's memory usage is fixed and will not grow unexpectedly. Our total budget is **~78 KB** (<15% of the Pico 2 W's 520KB SRAM).

| Component                          | Size (KB) | Purpose & Rationale                                                                                                                                                      |
| :--------------------------------- | :-------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **In-RAM Write Buffer (Memtable)** | 4 KB      | Primary buffer for incoming data points before they are flushed to flash.                                                                                                |
| **Query & Compaction Buffer**      | 8 KB      | A dedicated buffer used for reading and decompressing data from flash during queries and compaction. Its fixed size prevents large queries from consuming excessive RAM. |
| **In-Memory Sparse Index**         | 48 KB     | Stores `(start_timestamp, file_name)` pairs. This allows for ~2,000 index entries, which can track over 2 million data points.                                           |
| **LittleFS Runtime & Cache**       | 16 KB     | Allocates memory for the LittleFS filesystem itself, including its internal state, file handles, and a small cache.                                                      |
| **Runtime & Stack Overhead**       | 2 KB      | A small allocation for the database's own function call stack and other runtime variables on Core 1.                                                                     |
| **Total Estimated Maximum RAM**    | **78 KB** | This provides significant breathing space, leaving over 440KB of RAM for the main application and networking stack.                                                      |

### 5.4. Clock Management: A Resilient Time Source

The database itself acts as the system's persistent source of time.

| Decision & Rationale                                                                                                                                                                                                                            | Alternatives Explored                                                                                                                                                                                               | Trade-offs                                                                                                                                                                                                                                                                                                |
| :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Software-Managed Monotonic Clock:** The database persists the latest known timestamp to its index snapshot on flash. On boot, it reads this value and initializes the Pico's hardware timer, ensuring time is continuous across power cycles. | - **Rely on On-Chip RTC:** The on-chip clock resets on power loss, making it unsuitable for a time-series database.<br>- **Require External RTC Hardware:** Adds cost and complexity to the user's hardware design. | - **Clock Drift:** Without external sync, the clock will drift over time. <br>- **Benefit:** This software-only approach is robust, requires no extra hardware, and an API (`stampdb_set_time_unix()`) allows the main application to correct the time from an external source (like NTP) when available. |

### 5.5. Data Integrity and Reliability

StampDB employs a multi-layered approach to ensure data is never lost or corrupted.

| Feature             | How it Works                                                                                                                                                                                                       | Purpose                                                                            |
| :------------------ | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------- |
| **CRC32 Checksums** | A 32-bit checksum is calculated for every data block written to flash. This checksum is verified every time the block is read. We will use the Pico SDK's hardware-accelerated CRC32 capabilities for performance. | Guarantees protection against "bit rot" or other forms of on-disk data corruption. |
| **Atomic Writes**   | By using LittleFS, all file writes are atomic. An index snapshot or a data block is either written completely and correctly, or not at all.                                                                        | Prevents corrupted files in the event of power loss during a write operation.      |
| **Compaction**      | A low-priority background task on Core 1 merges smaller SSTable files into larger ones. It writes the new merged file first, then updates the index, and only then deletes the old files.                          | Maintains fast read performance over time and is itself a power-safe operation.    |

## 6. Key Jargon Explained

- **CRC32 (Cyclic Redundancy Check):** A powerful and efficient error-detecting code used to detect accidental changes to raw data. It's like a highly reliable "fingerprint" for a block of data.
- **Memtable:** A temporary, in-memory table that buffers new writes before they are flushed to permanent storage. It's the key to absorbing writes at high speed.
- **SSTable (Sorted String Table):** An immutable (never modified) file on flash that contains a block of data, sorted by its key (in our case, by timestamp).
- **LSM-Tree (Log-Structured Merge-Tree):** The architectural concept of using a Memtable, SSTables, and a Compaction process to create a write-optimized database.
- **Wear Leveling:** A technique used by flash-aware filesystems like LittleFS to distribute writes evenly across all physical blocks of the flash memory, preventing any single block from wearing out prematurely.

---

## Run on PC (host)

Build & test:

```
cmake --preset host-debug
cmake --build --preset host-debug -j
(cd build/host-debug && ctest --output-on-failure)
```

Export a range (CSV):

```
./build/host-debug/stampctl export --series 1 --t0 0 --t1 5000 --csv | head
```

Environment overrides (host sim paths):

```
export STAMPDB_FLASH_PATH=/abs/path/flash.bin
export STAMPDB_META_DIR=/abs/path
```

See `KNOWLEDGEBASE.md` for the full operational guide.
