# PageStore

A page-based persistent storage engine written in C11 with zero external dependencies.

Each table is stored as two binary files: a 4 KB paged `.dat` file with per-page IEEE CRC-32 checksums and a sorted, CRC-32 checksummed `.ndx` index file. Open tables run through an LRU write-back buffer pool, recycle deleted slots in `O(1)` via an in-memory freelist, and keep two indexes in sync: a **chained hash table** for `O(1)` point lookups and an **AVL tree** for ordered iteration and range scans.

---

## Features

| Component | Details |
|---|---|
| **Page Layout & Variable Records** | 4 KB fixed pages (`page 0` CRC-32 header; `pages 1..n` 8-byte `(page_no, crc32)` header + 8-byte aligned record slots), plus variable-length slotted pages (`varrec.c`) with string/composite `PSKey` support and overflow page chaining |
| **Dual & Disk-Resident Indexing** | Resizable chained hash table (load factor `<= 0.75`) + iterative-traversal self-balancing AVL tree (`O(log n)` height, `O(n)` zero-rotation bulk-load) + disk-resident 4 KB B+ Tree (`btree.c`, order 240) + 1-to-many Secondary Indexes (`secindex.c`) |
| **Negative Lookup Bloom Filter** | In-memory 64 KB (524,288-bit) 4-probe Kirsch-Mitzenmacher Bloom filter (`bloom.c`) short-circuiting 99%+ of nonexistent key queries in `O(1)` |
| **Write-Ahead Logging (WAL)** | Append-only `.wal` log (`wal.c`) with monotonic LSNs, CRC-32 record verification, checkpoint truncation, and automatic redo replay for dirty buffer pool frames on crash recovery |
| **Concurrency Control** | Per-table reader-writer latches (`pthread_rwlock_t`) paired with a mutex-protected LRU buffer pool and a compile-time static `.rodata` CRC-32 table |
| **LRU Buffer Pool** | Fixed-frame write-back page cache with hash page table, per-page CRC-32 verification on read/writeback, and hit/miss/eviction counters |
| **Freelist & `VACUUM`** | Soft-deleted slots are recycled in `O(1)`; `VACUUM` compacts `.dat`, rebuilds the Bloom filter, and clusters live records in key order via atomic `fsync` + `rename()` |
| **Multi-Table Catalog & Server** | Dynamically expanding table descriptor array (no fixed open-table limit), interactive CLI (`build/pagestore`), and multi-threaded TCP server (`build/pagestored`) |
| **Tests & Fuzzing** | 17 test suites (240k+ assertions) + libFuzzer/PRNG page/WAL corruption fuzzer (`fuzz/fuzz_page.c`) — clean under `-Wall -Wextra -Werror -Wpedantic` and ASan/UBSan |

---

## Architecture

```
              +--------------------------------------+
              |       CLI Shell / Library API        |
              |  insert  get  exists  update  delete |
              |  range   scan vacuum  stats   drop   |
              +------------------+-------------------+
                                 |
              +------------------v-------------------+
              |            PageStore Engine          |
              +-------------+------------+-----------+
              | Hash Table  |  AVL Tree  | Freelist  |
              | key -> slot | key -> slot| dead slots|
              | O(1) lookup | O(log n+k) | O(1) reuse|
              +------+------+-----+------+-----+-----+
                     +------------+------------+
                                  v
              +--------------------------------------+
              |     LRU Buffer Pool (4 KB pages)     |
              |  write-back · per-page CRC-32 · stats|
              +------------------+-------------------+
                                 v
              +--------------------------------------+
              |  <table>.dat (pages)  <table>.ndx    |
              |  CRC-32 per page      CRC-32 sorted  |
              +--------------------------------------+
```

### On-Disk Format

- **`<table>.dat`**
  - **Page 0 (4096 B):** `dat_header` storing magic (`PGSTORE1`), format version, record size, 8-byte aligned slot size, `slots_per_page`, slot high-water mark, live record count, `clean_shutdown` flag, and header CRC-32.
  - **Pages 1..N (4096 B each):** 8-byte page header `[uint32 page_no | uint32 page_crc]` followed by packed fixed-size slots `[uint32 state (EMPTY/LIVE/DEAD) | int32 key | record bytes]`. Slot `s` maps directly to page `1 + s / slots_per_page` at byte offset `8 + (s % slots_per_page) * slot_size`.
- **`<table>.ndx`**
  - Written via `<table>.ndx.tmp` + `fsync()` + atomic `rename()` on clean close.
  - Stores sorted `(key, slot)` pairs verified by count and CRC-32. On reopen, the AVL tree is bulk-loaded in `O(n)` with zero rotations and the dead-slot freelist is reconstructed via an in-memory bitset.

---

## Benchmarks (100,000 Records)

Measured on Apple Silicon (`-O2`, 96-byte records, 39 slots per 4 KB page, 2,565 data pages, per-page CRC-32 enabled):

### In-Memory Index Lookup (100k sequential inserts, 100k random lookups)

| Index | Insert (100k) | Lookup Latency | Notes |
|---|---:|---:|---|
| **Chained Hash Table** | 8.7 ms | **8.1 ns/op** | 262,144 buckets, `O(1)` average |
| **AVL Tree** | 13.0 ms | 161.8 ns/op | Height **17** on sorted inserts (`O(log n)` guaranteed) |

Hash point lookups are **20.0x faster** than AVL lookups, while the AVL tree stays balanced on sorted inserts and supports `O(log n + k)` range scans.

### Engine Operations

| Workload | Result |
|---|---|
| **100k Inserts + `fsync`** | 58.5 ms (~1.71M records/sec) |
| **100k `ps_get` (Warm 4096-page pool)** | **34.8 ns/op** (~28.7M ops/sec, 100.0% hit ratio) |
| **100k `ps_get` (Tight 64-page pool + CRC-32)** | 7,876 ns/op (2.5% hit ratio, **226.1x** slower than warm pool) |
| **Online `VACUUM` (after 75% deletes)** | 192.4 ms — reclaims 75,000 slots, shrinks `.dat` **10.02 MB -> 2.51 MB (74.9%)** |
| **Clean Reopen (`.ndx` `O(n)` AVL bulk-load)** | **4.17 ms** for 100,000 keys |
| **Crash Recovery (`.dat` full CRC-32 rebuild)** | **6.77 ms** for 25,000 live keys |
| **Freelist Churn (10k deletes + 10k inserts)** | **3.1 ms** — 10,000 slots reused, **642 -> 642 pages (0% growth)** |

---

## Build & Run

Requires a C11 compiler (`cc`/`clang`/`gcc`) and GNU `make`.

```bash
make              # build interactive shell -> build/pagestore
make test         # run 12 test suites (236k+ checks, including fork() crash test)
make test-asan    # run full test suite under AddressSanitizer + UBSan
make bench        # run the 100k-record benchmark suite
make clean        # remove build artifacts
```

### Interactive Shell Example

```text
$ ./build/pagestore ./mydb
PageStore shell - type 'help' for commands
opened database './mydb'
pagestore> create contacts
table created
pagestore> use contacts
table opened
pagestore:contacts> insert 101 Aarav 9876543210 aarav@example.com
inserted
pagestore:contacts> insert 100 Kabir 9988776655 kabir@example.com
inserted
pagestore:contacts> list
  100      Kabir                9988776655     kabir@example.com
  101      Aarav                9876543210     aarav@example.com
  (2 records)
pagestore:contacts> exists 101
yes
pagestore:contacts> count
  2
pagestore:contacts> stats
  table            contacts
  live records     2
  dead slots       0 (reusable / reclaimable by vacuum)
  slots reused     0
  slot size        104 bytes, 39 slots/page
  data pages       1
  hash buckets     16
  avl height       2
  index source     .ndx file
  buffer pool      256 pages
  cache hits/miss  8 / 1 (88.9% hit ratio)
  evictions        0, writebacks 0
pagestore:contacts> quit
```

---

## Project Layout

```text
PageStore/
├── include/
│   ├── pagestore.h       # Public database, table, CRUD, scan, vacuum & stats API
│   ├── hashtable.h       # Resizable chained hash table interface
│   ├── avl.h             # Self-balancing AVL tree + O(n) sorted bulk-load
│   ├── bufferpool.h      # LRU write-back page cache with per-page CRC-32
│   └── crc32.h           # IEEE 802.3 CRC-32 checksums
├── src/
│   ├── pagestore.c       # Storage engine, freelist, index persistence, crash rebuild, VACUUM
│   ├── hashtable.c       # Hash table implementation (power-of-two resizing)
│   ├── avl.c             # AVL rotations, sorted bulk-load, range pruning, invariant validator
│   ├── bufferpool.c      # Doubly-linked LRU + page table + CRC-32 pread/pwrite I/O
│   └── crc32.c           # Lookup-table CRC-32
├── cli/
│   └── main.c            # Interactive contacts shell over PageStore
├── tests/
│   └── test_main.c       # 12 test suites (including bit-flip corruption & fork() crash tests)
├── bench/
│   └── bench_main.c      # 100k-record index, buffer-pool, vacuum, recovery & churn benchmarks
└── Makefile
```
