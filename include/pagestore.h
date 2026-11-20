#ifndef PAGESTORE_H
#define PAGESTORE_H

/*
 * PageStore - a page-based persistent storage engine in C11.
 *
 * A database is a directory. Each table lives in three files:
 *   <table>.dat  page 0 = CRC32-protected header;
 *                pages 1..n = CRC32-protected data pages of fixed-size slots
 *   <table>.ndx  CRC32-protected sorted primary index (key -> slot)
 *   <table>.wal  CRC32-protected write-ahead log for dirty buffer pool durability
 *
 * Every open table keeps two in-memory indexes + a 64KB Bloom filter in sync:
 *   - a 4-probe Bloom filter for O(1) negative lookup short-circuiting
 *   - a chained hash table for O(1) average point lookups
 *   - an AVL tree for ordered iteration and range scans in O(log n + k)
 *
 * Concurrency:
 *   Each table is protected by a reader-writer latch (pthread_rwlock_t) paired
 *   with a mutex-protected LRU buffer pool, allowing concurrent readers and
 *   serialized writers.
 */

#include <stddef.h>
#include <stdint.h>

#define PS_PAGE_SIZE          4096u
#define PS_MAX_NAME           63
#define PS_MAX_RECORD_SIZE    (PS_PAGE_SIZE - 16u) /* 8B page hdr + 8B slot hdr */
#define PS_INIT_OPEN_TABLES   16
#define PS_DEFAULT_POOL_PAGES 256

typedef enum {
    PS_OK            =  0,
    PS_ERR_IO        = -1,
    PS_ERR_NOMEM     = -2,
    PS_ERR_EXISTS    = -3,
    PS_ERR_NOT_FOUND = -4,
    PS_ERR_DUPLICATE = -5,
    PS_ERR_INVALID   = -6,
    PS_ERR_CORRUPT   = -7,
    PS_ERR_TOO_MANY  = -8,
    PS_ERR_BUSY      = -9
} ps_status;

typedef struct ps_db ps_db;
typedef struct ps_table ps_table;

typedef struct {
    uint32_t record_size;
    uint32_t slot_size;
    uint32_t slots_per_page;
    uint32_t live_records;
    uint32_t dead_slots;      /* soft-deleted slots available for reuse or VACUUM */
    uint32_t total_slots;
    uint32_t data_pages;
    uint64_t slots_reused;    /* inserts satisfied from the freelist */
    size_t   hash_buckets;
    int      avl_height;
    int      index_rebuilt;   /* 1 if index/WAL recovery ran on open */
    size_t   pool_pages;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_evictions;
    uint64_t cache_writebacks;
    uint64_t bloom_rejections;/* negative lookups short-circuited by Bloom filter */
    uint64_t wal_records_logged;
    uint64_t wal_records_replayed;
} ps_table_stats;

typedef struct {
    uint32_t live_records;
    uint32_t slots_reclaimed;
    uint64_t bytes_before;
    uint64_t bytes_after;
} ps_vacuum_result;

/* Callback for scans. Return non-zero to stop early. Safe to call engine read
 * operations from inside the callback. */
typedef int (*ps_record_fn)(int32_t key, const void *record, void *ctx);

/* ---- database ---- */
int  ps_db_open(const char *dir, ps_db **out);
void ps_db_close(ps_db *db);   /* closes any tables still open */
int  ps_db_list_tables(ps_db *db, char names[][PS_MAX_NAME + 1], int max);

/* ---- tables ---- */
int ps_table_create(ps_db *db, const char *name, uint32_t record_size);
int ps_table_drop(ps_db *db, const char *name);
int ps_table_open(ps_db *db, const char *name, ps_table **out);
int ps_table_open_ex(ps_db *db, const char *name, size_t pool_pages, ps_table **out);
int ps_table_close(ps_table *t);
void ps_table_set_wal_sync(ps_table *t, int sync_on_write);

/* ---- records ---- */
int      ps_insert(ps_table *t, int32_t key, const void *record);
int      ps_get(ps_table *t, int32_t key, void *out);
int      ps_exists(const ps_table *t, int32_t key); /* 1 yes, 0 no, <0 err */
int      ps_update(ps_table *t, int32_t key, const void *record);
int      ps_delete(ps_table *t, int32_t key);
uint32_t ps_count(const ps_table *t);

/* Key-ordered scan over [lo, hi] using the AVL index. Returns the number of
 * records visited, or a negative ps_status on error. */
long ps_range(ps_table *t, int32_t lo, int32_t hi, ps_record_fn fn, void *ctx);

/* Full scan in physical (slot) order - used for non-key field searches. */
long ps_scan(ps_table *t, ps_record_fn fn, void *ctx);

/* ---- maintenance ---- */
int         ps_flush(ps_table *t);
int         ps_vacuum(ps_table *t, ps_vacuum_result *res);
void        ps_table_get_stats(const ps_table *t, ps_table_stats *st);
void        ps_reset_cache_stats(ps_table *t);
uint32_t    ps_record_size(const ps_table *t);
const char *ps_table_name(const ps_table *t);
const char *ps_strerror(int code);

#endif /* PAGESTORE_H */
