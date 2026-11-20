#define _POSIX_C_SOURCE 200809L
#include "pagestore.h"
#include "avl.h"
#include "bloom.h"
#include "bufferpool.h"
#include "crc32.h"
#include "hashtable.h"
#include "wal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ */
/* On-disk formats                                                     */
/* ------------------------------------------------------------------ */

#define DAT_MAGIC   "PGSTORE1"
#define NDX_MAGIC   "PGSNDX01"
#define FORMAT_VER  1u

#define SLOT_EMPTY  0u
#define SLOT_LIVE   1u
#define SLOT_DEAD   2u
#define SLOT_HDR    8u        /* uint32 state + int32 key */

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t slot_size;
    uint32_t slots_per_page;
    uint32_t total_slots;     /* slot high-water mark */
    uint32_t live_records;
    uint32_t clean_shutdown;  /* 1 = closed cleanly, 0 = open or crashed */
    uint32_t header_crc;      /* CRC32 of every field above */
} dat_header;

typedef struct {
    char     magic[8];
    uint32_t count;
    uint32_t total_slots;
    uint32_t entries_crc;
    uint32_t reserved;
} ndx_header;

typedef struct {
    int32_t  key;
    uint32_t slot;
} ndx_entry;

/* ------------------------------------------------------------------ */
/* In-memory handles                                                   */
/* ------------------------------------------------------------------ */

struct ps_table {
    ps_db           *db;
    char             name[PS_MAX_NAME + 1];
    char             dat_path[PATH_MAX];
    char             ndx_path[PATH_MAX];
    char             wal_path[PATH_MAX];
    int              fd;
    dat_header       hdr;
    bufferpool_t    *pool;
    ht_t            *hash;       /* key -> slot, O(1) point lookups   */
    avl_tree         tree;       /* key -> slot, ordered / range scans */
    ps_bloom_t       bloom;      /* 64KB 4-probe negative lookup filter */
    _Atomic uint64_t bloom_rejections;
    ps_wal_t         wal;        /* Write-Ahead Log for dirty page durability */
    uint32_t        *free_slots; /* LIFO stack of recyclable SLOT_DEAD indices */
    size_t           free_count;
    size_t           free_cap;
    uint64_t         slots_reused;
    int              index_rebuilt;
    pthread_rwlock_t rwlock;     /* Reader-writer latch for thread-safe concurrency */
};

struct ps_db {
    char            dir[PATH_MAX];
    ps_table      **open;        /* Dynamically expanding table descriptor array */
    size_t          open_cap;
    pthread_mutex_t lock;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

const char *ps_strerror(int code)
{
    switch (code) {
    case PS_OK:            return "ok";
    case PS_ERR_IO:        return "i/o error";
    case PS_ERR_NOMEM:     return "out of memory";
    case PS_ERR_EXISTS:    return "already exists";
    case PS_ERR_NOT_FOUND: return "not found";
    case PS_ERR_DUPLICATE: return "duplicate key";
    case PS_ERR_INVALID:   return "invalid argument";
    case PS_ERR_CORRUPT:   return "corrupt file";
    case PS_ERR_TOO_MANY:  return "too many open tables";
    case PS_ERR_BUSY:      return "table already open";
    default:               return "unknown error";
    }
}

static int valid_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > PS_MAX_NAME)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return 0;
    }
    return 1;
}

static int build_path(char *out, const char *dir, const char *name, const char *ext)
{
    int n = snprintf(out, PATH_MAX, "%s/%s%s", dir, name, ext);
    return (n > 0 && n < PATH_MAX) ? 0 : -1;
}

static uint32_t header_crc(const dat_header *h)
{
    return crc32_compute(h, offsetof(dat_header, header_crc));
}

static int bp_err_to_ps(const bufferpool_t *bp)
{
    switch (bp_last_error(bp)) {
    case BP_ERR_CORRUPT: return PS_ERR_CORRUPT;
    case BP_ERR_NOMEM:   return PS_ERR_NOMEM;
    default:             return PS_ERR_IO;
    }
}

static int freelist_push(ps_table *t, uint32_t slot)
{
    if (t->free_count == t->free_cap) {
        size_t ncap = t->free_cap ? t->free_cap * 2 : 64;
        uint32_t *nf = realloc(t->free_slots, ncap * sizeof *nf);
        if (!nf)
            return -1;
        t->free_slots = nf;
        t->free_cap = ncap;
    }
    t->free_slots[t->free_count++] = slot;
    return 0;
}

static int write_full(int fd, const void *buf, size_t len, off_t off)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;   /* short file */
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

static int write_header(ps_table *t)
{
    t->hdr.header_crc = header_crc(&t->hdr);
    return write_full(t->fd, &t->hdr, sizeof t->hdr, 0) == 0 ? PS_OK : PS_ERR_IO;
}

static void fsync_dir(const char *dir)
{
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

/* Slot s lives on data page 1 + s / slots_per_page (page 0 is the file header).
 * Each data page begins with an 8-byte (page_no, crc32) header. */
static uint8_t *slot_ptr(ps_table *t, uint32_t slot, int for_write)
{
    uint32_t page = 1u + slot / t->hdr.slots_per_page;
    uint8_t *base = bp_fetch(t->pool, page, for_write);
    if (!base)
        return NULL;
    return base + BP_PAGE_HDR + (size_t)(slot % t->hdr.slots_per_page) * t->hdr.slot_size;
}

static int lookup_slot(const ps_table *t, int32_t key, uint32_t *slot)
{
    if (!bloom_may_contain_i32(&t->bloom, key)) {
        ps_table *mut = (ps_table *)t;
        atomic_fetch_add_explicit(&mut->bloom_rejections, 1, memory_order_relaxed);
        return 0;
    }
    int64_t v;
    if (!ht_get(t->hash, key, &v))
        return 0;
    if (slot)
        *slot = (uint32_t)v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

int ps_db_open(const char *dir, ps_db **out)
{
    if (!dir || !out || strlen(dir) >= PATH_MAX - PS_MAX_NAME - 8)
        return PS_ERR_INVALID;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return PS_ERR_IO;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return PS_ERR_INVALID;

    ps_db *db = calloc(1, sizeof *db);
    if (!db)
        return PS_ERR_NOMEM;
    db->open_cap = PS_INIT_OPEN_TABLES;
    db->open = calloc(db->open_cap, sizeof *db->open);
    if (!db->open) {
        free(db);
        return PS_ERR_NOMEM;
    }
    pthread_mutex_init(&db->lock, NULL);
    snprintf(db->dir, sizeof db->dir, "%s", dir);
    *out = db;
    return PS_OK;
}

void ps_db_close(ps_db *db)
{
    if (!db)
        return;
    for (size_t i = 0; i < db->open_cap; i++)
        if (db->open[i])
            ps_table_close(db->open[i]);
    pthread_mutex_destroy(&db->lock);
    free(db->open);
    free(db);
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int ps_db_list_tables(ps_db *db, char names[][PS_MAX_NAME + 1], int max)
{
    if (!db || !names || max <= 0)
        return PS_ERR_INVALID;
    DIR *d = opendir(db->dir);
    if (!d)
        return PS_ERR_IO;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 4 && len - 4 <= PS_MAX_NAME && strcmp(e->d_name + len - 4, ".dat") == 0) {
            memcpy(names[n], e->d_name, len - 4);
            names[n][len - 4] = '\0';
            if (valid_name(names[n]))
                n++;
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    return n;
}

/* ------------------------------------------------------------------ */
/* Index persistence                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    ndx_entry *entries;
    size_t     n;
} collect_ctx;

static int collect_entry(int32_t key, uint32_t slot, void *ctx)
{
    collect_ctx *c = ctx;
    c->entries[c->n].key = key;
    c->entries[c->n].slot = slot;
    c->n++;
    return 0;
}

/* Writes the index to <table>.ndx.tmp, fsyncs, then renames over .ndx so a
 * crash mid-write never leaves a half-written index behind. */
static int save_index(ps_table *t)
{
    collect_ctx c = { NULL, 0 };
    if (t->tree.count > 0) {
        c.entries = malloc(t->tree.count * sizeof *c.entries);
        if (!c.entries)
            return PS_ERR_NOMEM;
        avl_range(&t->tree, INT32_MIN, INT32_MAX, collect_entry, &c);
    }

    ndx_header nh;
    memset(&nh, 0, sizeof nh);
    memcpy(nh.magic, NDX_MAGIC, 8);
    nh.count = (uint32_t)c.n;
    nh.total_slots = t->hdr.total_slots;
    nh.entries_crc = crc32_compute(c.entries, c.n * sizeof(ndx_entry));

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", t->ndx_path) >= (int)sizeof tmp) {
        free(c.entries);
        return PS_ERR_INVALID;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(c.entries);
        return PS_ERR_IO;
    }
    int rc = PS_OK;
    if (write_full(fd, &nh, sizeof nh, 0) != 0 ||
        (c.n && write_full(fd, c.entries, c.n * sizeof(ndx_entry), sizeof nh) != 0) ||
        fsync(fd) != 0)
        rc = PS_ERR_IO;
    close(fd);
    free(c.entries);
    if (rc == PS_OK && rename(tmp, t->ndx_path) != 0)
        rc = PS_ERR_IO;
    if (rc != PS_OK)
        unlink(tmp);
    return rc;
}

/* Loads <table>.ndx using O(n) sorted bulk-load for the AVL tree and rebuilds
 * the dead-slot freelist via an in-memory bitset (zero extra disk I/O).
 * Any inconsistency returns PS_ERR_CORRUPT so the caller falls back to .dat. */
static int load_index(ps_table *t)
{
    int fd = open(t->ndx_path, O_RDONLY);
    if (fd < 0)
        return PS_ERR_CORRUPT;

    int rc = PS_ERR_CORRUPT;
    ndx_entry *entries = NULL;
    avl_kv *kvs = NULL;
    uint8_t *used_bits = NULL;
    ndx_header nh;
    if (read_full(fd, &nh, sizeof nh, 0) != 0 || memcmp(nh.magic, NDX_MAGIC, 8) != 0)
        goto done;
    if (nh.count != t->hdr.live_records || nh.total_slots != t->hdr.total_slots)
        goto done;
    if (nh.count > 0) {
        entries = malloc((size_t)nh.count * sizeof *entries);
        kvs = malloc((size_t)nh.count * sizeof *kvs);
        if (!entries || !kvs) {
            rc = PS_ERR_NOMEM;
            goto done;
        }
        if (read_full(fd, entries, (size_t)nh.count * sizeof *entries, sizeof nh) != 0)
            goto done;
    }
    if (crc32_compute(entries, (size_t)nh.count * sizeof(ndx_entry)) != nh.entries_crc)
        goto done;

    size_t bit_bytes = ((size_t)t->hdr.total_slots + 7u) / 8u;
    if (t->hdr.total_slots > nh.count) {
        used_bits = calloc(bit_bytes, 1);
        if (!used_bits) {
            rc = PS_ERR_NOMEM;
            goto done;
        }
    }

    bloom_clear(&t->bloom);
    for (uint32_t i = 0; i < nh.count; i++) {
        uint32_t s = entries[i].slot;
        if (s >= t->hdr.total_slots)
            goto done;
        if (used_bits) {
            if (used_bits[s >> 3] & (1u << (s & 7)))
                goto done;   /* duplicate slot reference */
            used_bits[s >> 3] |= (uint8_t)(1u << (s & 7));
        }
        kvs[i].key = entries[i].key;
        kvs[i].val = s;
        if (ht_put(t->hash, entries[i].key, s) != 0) {
            rc = PS_ERR_NOMEM;
            goto done;
        }
        bloom_add_i32(&t->bloom, entries[i].key);
    }

    /* O(n) bulk-load directly from the sorted .ndx entries (zero rotations). */
    if (avl_build_sorted(&t->tree, kvs, nh.count) != 0)
        goto done;

    /* Reconstruct the recyclable freelist from dead slots in reverse order so
     * low-numbered slots are popped first. */
    if (used_bits) {
        for (uint32_t s = t->hdr.total_slots; s > 0; s--) {
            uint32_t idx = s - 1;
            if (!(used_bits[idx >> 3] & (1u << (idx & 7))))
                (void)freelist_push(t, idx);
        }
    }
    rc = PS_OK;
done:
    free(used_bits);
    free(kvs);
    free(entries);
    close(fd);
    if (rc != PS_OK) {
        ht_clear(t->hash);
        avl_free(&t->tree);
        bloom_clear(&t->bloom);
        t->free_count = 0;
    }
    return rc;
}

/* Internal unlocked mutation helpers (used by both public API and WAL replay). */
static int insert_unlocked(ps_table *t, int32_t key, const void *record, int log_wal)
{
    if (ht_get(t->hash, key, NULL))
        return PS_ERR_DUPLICATE;

    if (log_wal) {
        int wrc = ps_wal_append(&t->wal, PS_WAL_INSERT, key, record, t->hdr.record_size);
        if (wrc != PS_OK)
            return wrc;
    }

    int reused = 0;
    uint32_t slot;
    if (t->free_count > 0) {
        slot = t->free_slots[--t->free_count];
        reused = 1;
    } else {
        if (t->hdr.total_slots == UINT32_MAX)
            return PS_ERR_INVALID;
        slot = t->hdr.total_slots;
    }

    uint8_t *p = slot_ptr(t, slot, 1);
    if (!p) {
        if (reused)
            (void)freelist_push(t, slot);
        return bp_err_to_ps(t->pool);
    }

    uint32_t state = SLOT_LIVE;
    memcpy(p, &state, 4);
    memcpy(p + 4, &key, 4);
    memcpy(p + SLOT_HDR, record, t->hdr.record_size);

    if (ht_put(t->hash, key, slot) != 0) {
        uint32_t prev = reused ? SLOT_DEAD : SLOT_EMPTY;
        memcpy(p, &prev, 4);
        if (reused)
            (void)freelist_push(t, slot);
        return PS_ERR_NOMEM;
    }
    if (avl_insert(&t->tree, key, slot) != 0) {
        uint32_t prev = reused ? SLOT_DEAD : SLOT_EMPTY;
        memcpy(p, &prev, 4);
        ht_remove(t->hash, key);
        if (reused)
            (void)freelist_push(t, slot);
        return PS_ERR_NOMEM;
    }
    bloom_add_i32(&t->bloom, key);
    if (reused)
        t->slots_reused++;
    else
        t->hdr.total_slots++;
    t->hdr.live_records++;
    return PS_OK;
}

static int update_unlocked(ps_table *t, int32_t key, const void *record, int log_wal)
{
    uint32_t slot;
    if (!lookup_slot(t, key, &slot))
        return PS_ERR_NOT_FOUND;

    if (log_wal) {
        int wrc = ps_wal_append(&t->wal, PS_WAL_UPDATE, key, record, t->hdr.record_size);
        if (wrc != PS_OK)
            return wrc;
    }

    uint8_t *p = slot_ptr(t, slot, 1);
    if (!p)
        return bp_err_to_ps(t->pool);
    memcpy(p + SLOT_HDR, record, t->hdr.record_size);
    return PS_OK;
}

static int delete_unlocked(ps_table *t, int32_t key, int log_wal)
{
    uint32_t slot;
    if (!lookup_slot(t, key, &slot))
        return PS_ERR_NOT_FOUND;

    if (log_wal) {
        int wrc = ps_wal_append(&t->wal, PS_WAL_DELETE, key, NULL, 0);
        if (wrc != PS_OK)
            return wrc;
    }

    uint8_t *p = slot_ptr(t, slot, 1);
    if (!p)
        return bp_err_to_ps(t->pool);
    uint32_t state = SLOT_DEAD;
    memcpy(p, &state, 4);
    ht_remove(t->hash, key);
    avl_remove(&t->tree, key);
    t->hdr.live_records--;
    (void)freelist_push(t, slot);
    return PS_OK;
}

static int wal_replay_cb(uint64_t lsn, ps_wal_op op, int32_t key,
                         const void *payload, uint32_t payload_len, void *ctx)
{
    (void)lsn;
    (void)payload_len;
    ps_table *t = ctx;
    switch (op) {
    case PS_WAL_INSERT:
        if (ht_get(t->hash, key, NULL))
            return update_unlocked(t, key, payload, 0);
        return insert_unlocked(t, key, payload, 0);
    case PS_WAL_UPDATE:
        if (ht_get(t->hash, key, NULL))
            return update_unlocked(t, key, payload, 0);
        return insert_unlocked(t, key, payload, 0);
    case PS_WAL_DELETE:
        if (ht_get(t->hash, key, NULL))
            return delete_unlocked(t, key, 0);
        return PS_OK;
    default:
        return PS_ERR_CORRUPT;
    }
}

/* Crash recovery: scan every slot of the data file and rebuild both indexes
 * and the dead-slot freelist from the persisted pages, then replay the WAL. */
static int rebuild_index(ps_table *t)
{
    struct stat st;
    if (fstat(t->fd, &st) != 0)
        return PS_ERR_IO;
    uint64_t pages = (uint64_t)st.st_size / PS_PAGE_SIZE;
    uint64_t max_slots = pages > 1 ? (pages - 1) * t->hdr.slots_per_page : 0;
    if (max_slots > UINT32_MAX)
        return PS_ERR_CORRUPT;

    ht_clear(t->hash);
    avl_free(&t->tree);
    bloom_clear(&t->bloom);
    t->free_count = 0;

    uint32_t live = 0, high_water = 0;
    for (uint32_t s = 0; s < (uint32_t)max_slots; s++) {
        uint8_t *p = slot_ptr(t, s, 0);
        if (!p)
            return bp_err_to_ps(t->pool);
        uint32_t state;
        int32_t key;
        memcpy(&state, p, 4);
        memcpy(&key, p + 4, 4);
        if (state == SLOT_EMPTY)
            continue;
        if (state != SLOT_LIVE && state != SLOT_DEAD)
            return PS_ERR_CORRUPT;
        high_water = s + 1;
        if (state == SLOT_LIVE) {
            if (avl_insert(&t->tree, key, s) != 0)
                return PS_ERR_CORRUPT;
            if (ht_put(t->hash, key, s) != 0)
                return PS_ERR_NOMEM;
            bloom_add_i32(&t->bloom, key);
            live++;
        } else {
            (void)freelist_push(t, s);
        }
    }
    t->hdr.total_slots = high_water;
    t->hdr.live_records = live;

    if (ps_wal_has_records(&t->wal)) {
        int wrc = ps_wal_replay(&t->wal, t->hdr.record_size, wal_replay_cb, t);
        if (wrc != PS_OK)
            return wrc;
    }

    t->index_rebuilt = 1;
    return PS_OK;
}

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

int ps_table_create(ps_db *db, const char *name, uint32_t record_size)
{
    if (!db || !valid_name(name) || record_size == 0 || record_size > PS_MAX_RECORD_SIZE)
        return PS_ERR_INVALID;

    char dat[PATH_MAX], ndx[PATH_MAX], wal[PATH_MAX];
    if (build_path(dat, db->dir, name, ".dat") ||
        build_path(ndx, db->dir, name, ".ndx") ||
        build_path(wal, db->dir, name, ".wal"))
        return PS_ERR_INVALID;

    int fd = open(dat, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return errno == EEXIST ? PS_ERR_EXISTS : PS_ERR_IO;

    ps_table tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.fd = fd;
    memcpy(tmp.hdr.magic, DAT_MAGIC, 8);
    tmp.hdr.version = FORMAT_VER;
    tmp.hdr.record_size = record_size;
    tmp.hdr.slot_size = (SLOT_HDR + record_size + 7u) & ~7u;
    tmp.hdr.slots_per_page = (PS_PAGE_SIZE - BP_PAGE_HDR) / tmp.hdr.slot_size;
    tmp.hdr.clean_shutdown = 1;
    snprintf(tmp.ndx_path, sizeof tmp.ndx_path, "%s", ndx);

    uint8_t page0[PS_PAGE_SIZE];
    memset(page0, 0, sizeof page0);
    tmp.hdr.header_crc = header_crc(&tmp.hdr);
    memcpy(page0, &tmp.hdr, sizeof tmp.hdr);

    int rc = PS_OK;
    if (write_full(fd, page0, sizeof page0, 0) != 0 || fsync(fd) != 0)
        rc = PS_ERR_IO;
    if (rc == PS_OK) {
        avl_init(&tmp.tree);
        rc = save_index(&tmp);   /* empty, valid index */
    }
    close(fd);
    unlink(wal);
    if (rc != PS_OK) {
        unlink(dat);
        unlink(ndx);
        return rc;
    }
    fsync_dir(db->dir);
    return PS_OK;
}

int ps_table_drop(ps_db *db, const char *name)
{
    if (!db || !valid_name(name))
        return PS_ERR_INVALID;

    pthread_mutex_lock(&db->lock);
    for (size_t i = 0; i < db->open_cap; i++) {
        if (db->open[i] && strcmp(db->open[i]->name, name) == 0) {
            pthread_mutex_unlock(&db->lock);
            return PS_ERR_BUSY;
        }
    }
    pthread_mutex_unlock(&db->lock);

    char dat[PATH_MAX], ndx[PATH_MAX], wal[PATH_MAX];
    if (build_path(dat, db->dir, name, ".dat") ||
        build_path(ndx, db->dir, name, ".ndx") ||
        build_path(wal, db->dir, name, ".wal"))
        return PS_ERR_INVALID;
    if (unlink(dat) != 0)
        return errno == ENOENT ? PS_ERR_NOT_FOUND : PS_ERR_IO;
    if (unlink(ndx) != 0 && errno != ENOENT)
        return PS_ERR_IO;
    (void)unlink(wal);
    fsync_dir(db->dir);
    return PS_OK;
}

static void table_free(ps_table *t)
{
    if (!t)
        return;
    pthread_rwlock_destroy(&t->rwlock);
    ps_wal_close(&t->wal);
    bp_destroy(t->pool);
    ht_free(t->hash);
    avl_free(&t->tree);
    free(t->free_slots);
    if (t->fd >= 0)
        close(t->fd);
    free(t);
}

int ps_table_open_ex(ps_db *db, const char *name, size_t pool_pages, ps_table **out)
{
    if (!db || !out || !valid_name(name))
        return PS_ERR_INVALID;

    pthread_mutex_lock(&db->lock);
    ssize_t free_idx = -1;
    for (size_t i = 0; i < db->open_cap; i++) {
        if (db->open[i] && strcmp(db->open[i]->name, name) == 0) {
            pthread_mutex_unlock(&db->lock);
            return PS_ERR_BUSY;
        }
        if (!db->open[i] && free_idx < 0)
            free_idx = (ssize_t)i;
    }
    if (free_idx < 0) {
        /* Dynamically double the open table descriptor array */
        size_t new_cap = db->open_cap * 2;
        ps_table **grown = realloc(db->open, new_cap * sizeof *grown);
        if (!grown) {
            pthread_mutex_unlock(&db->lock);
            return PS_ERR_NOMEM;
        }
        memset(grown + db->open_cap, 0, (new_cap - db->open_cap) * sizeof *grown);
        free_idx = (ssize_t)db->open_cap;
        db->open = grown;
        db->open_cap = new_cap;
    }

    ps_table *t = calloc(1, sizeof *t);
    if (!t) {
        pthread_mutex_unlock(&db->lock);
        return PS_ERR_NOMEM;
    }
    t->fd = -1;
    t->wal.fd = -1;
    t->db = db;
    pthread_rwlock_init(&t->rwlock, NULL);
    bloom_clear(&t->bloom);
    atomic_store(&t->bloom_rejections, 0);
    snprintf(t->name, sizeof t->name, "%s", name);
    if (build_path(t->dat_path, db->dir, name, ".dat") ||
        build_path(t->ndx_path, db->dir, name, ".ndx") ||
        build_path(t->wal_path, db->dir, name, ".wal")) {
        pthread_mutex_unlock(&db->lock);
        table_free(t);
        return PS_ERR_INVALID;
    }

    t->fd = open(t->dat_path, O_RDWR);
    if (t->fd < 0) {
        int rc = errno == ENOENT ? PS_ERR_NOT_FOUND : PS_ERR_IO;
        pthread_mutex_unlock(&db->lock);
        table_free(t);
        return rc;
    }

    int rc;
    if (read_full(t->fd, &t->hdr, sizeof t->hdr, 0) != 0 ||
        memcmp(t->hdr.magic, DAT_MAGIC, 8) != 0 ||
        t->hdr.version != FORMAT_VER ||
        t->hdr.header_crc != header_crc(&t->hdr) ||
        t->hdr.record_size == 0 ||
        t->hdr.record_size > PS_MAX_RECORD_SIZE ||
        t->hdr.slot_size != ((SLOT_HDR + t->hdr.record_size + 7u) & ~7u) ||
        t->hdr.slots_per_page != (PS_PAGE_SIZE - BP_PAGE_HDR) / t->hdr.slot_size) {
        rc = PS_ERR_CORRUPT;
        goto fail;
    }

    if (ps_wal_open(&t->wal, t->wal_path) != PS_OK) {
        rc = PS_ERR_IO;
        goto fail;
    }

    t->pool = bp_create(t->fd, pool_pages ? pool_pages : PS_DEFAULT_POOL_PAGES, 1);
    t->hash = ht_create(t->hdr.live_records + 16u);
    avl_init(&t->tree);
    if (!t->pool || !t->hash) {
        rc = PS_ERR_NOMEM;
        goto fail;
    }

    /* Fast path: trust the persisted index only after a clean shutdown, with
     * an empty WAL, and only if .ndx passes its CRC and consistency checks. */
    if (!t->hdr.clean_shutdown || ps_wal_has_records(&t->wal) || load_index(t) != PS_OK) {
        rc = rebuild_index(t);
        if (rc != PS_OK)
            goto fail;
        /* Persist recovered pages and truncate the replayed WAL */
        if (bp_flush_all(t->pool) != 0 || ps_wal_checkpoint(&t->wal) != PS_OK) {
            rc = PS_ERR_IO;
            goto fail;
        }
    }

    /* Mark the table dirty until a clean close. */
    t->hdr.clean_shutdown = 0;
    if (write_header(t) != PS_OK || fsync(t->fd) != 0) {
        rc = PS_ERR_IO;
        goto fail;
    }

    bp_reset_stats(t->pool);
    db->open[free_idx] = t;
    pthread_mutex_unlock(&db->lock);
    *out = t;
    return PS_OK;

fail:
    pthread_mutex_unlock(&db->lock);
    table_free(t);
    return rc;
}

int ps_table_open(ps_db *db, const char *name, ps_table **out)
{
    return ps_table_open_ex(db, name, PS_DEFAULT_POOL_PAGES, out);
}

void ps_table_set_wal_sync(ps_table *t, int sync_on_write)
{
    if (!t)
        return;
    pthread_rwlock_wrlock(&t->rwlock);
    t->wal.sync_on_write = sync_on_write ? 1 : 0;
    pthread_rwlock_unlock(&t->rwlock);
}

static int flush_unlocked(ps_table *t)
{
    if (bp_flush_all(t->pool) != 0 || write_header(t) != PS_OK || fsync(t->fd) != 0)
        return PS_ERR_IO;
    if (ps_wal_checkpoint(&t->wal) != PS_OK)
        return PS_ERR_IO;
    return PS_OK;
}

int ps_flush(ps_table *t)
{
    if (!t)
        return PS_ERR_INVALID;
    pthread_rwlock_wrlock(&t->rwlock);
    int rc = flush_unlocked(t);
    pthread_rwlock_unlock(&t->rwlock);
    return rc;
}

int ps_table_close(ps_table *t)
{
    if (!t)
        return PS_ERR_INVALID;
    pthread_rwlock_wrlock(&t->rwlock);
    int rc = flush_unlocked(t);
    if (rc == PS_OK)
        rc = save_index(t);
    if (rc == PS_OK) {
        /* Only claim a clean shutdown once data and index are both durable. */
        t->hdr.clean_shutdown = 1;
        if (write_header(t) != PS_OK || fsync(t->fd) != 0)
            rc = PS_ERR_IO;
    }
    pthread_rwlock_unlock(&t->rwlock);

    pthread_mutex_lock(&t->db->lock);
    for (size_t i = 0; i < t->db->open_cap; i++)
        if (t->db->open[i] == t)
            t->db->open[i] = NULL;
    pthread_mutex_unlock(&t->db->lock);

    table_free(t);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Records                                                             */
/* ------------------------------------------------------------------ */

int ps_insert(ps_table *t, int32_t key, const void *record)
{
    if (!t || !record)
        return PS_ERR_INVALID;
    pthread_rwlock_wrlock(&t->rwlock);
    int rc = insert_unlocked(t, key, record, 1);
    pthread_rwlock_unlock(&t->rwlock);
    return rc;
}

int ps_get(ps_table *t, int32_t key, void *out)
{
    if (!t || !out)
        return PS_ERR_INVALID;
    pthread_rwlock_rdlock(&t->rwlock);
    uint32_t slot;
    if (!lookup_slot(t, key, &slot)) {
        pthread_rwlock_unlock(&t->rwlock);
        return PS_ERR_NOT_FOUND;
    }
    uint8_t *p = slot_ptr(t, slot, 0);
    if (!p) {
        int err = bp_err_to_ps(t->pool);
        pthread_rwlock_unlock(&t->rwlock);
        return err;
    }
    memcpy(out, p + SLOT_HDR, t->hdr.record_size);
    pthread_rwlock_unlock(&t->rwlock);
    return PS_OK;
}

int ps_exists(const ps_table *t, int32_t key)
{
    if (!t)
        return PS_ERR_INVALID;
    ps_table *mut = (ps_table *)t;
    pthread_rwlock_rdlock(&mut->rwlock);
    int res = lookup_slot(t, key, NULL);
    pthread_rwlock_unlock(&mut->rwlock);
    return res;
}

uint32_t ps_count(const ps_table *t)
{
    if (!t)
        return 0;
    ps_table *mut = (ps_table *)t;
    pthread_rwlock_rdlock(&mut->rwlock);
    uint32_t c = t->hdr.live_records;
    pthread_rwlock_unlock(&mut->rwlock);
    return c;
}

int ps_update(ps_table *t, int32_t key, const void *record)
{
    if (!t || !record)
        return PS_ERR_INVALID;
    pthread_rwlock_wrlock(&t->rwlock);
    int rc = update_unlocked(t, key, record, 1);
    pthread_rwlock_unlock(&t->rwlock);
    return rc;
}

int ps_delete(ps_table *t, int32_t key)
{
    if (!t)
        return PS_ERR_INVALID;
    pthread_rwlock_wrlock(&t->rwlock);
    int rc = delete_unlocked(t, key, 1);
    pthread_rwlock_unlock(&t->rwlock);
    return rc;
}

typedef struct {
    ps_table    *t;
    ps_record_fn fn;
    void        *ctx;
    int          err;
    uint8_t      buf[PS_MAX_RECORD_SIZE];
} range_ctx;

static int range_visit(int32_t key, uint32_t slot, void *ctx)
{
    range_ctx *rc = ctx;
    uint8_t *p = slot_ptr(rc->t, slot, 0);
    if (!p) {
        rc->err = bp_err_to_ps(rc->t->pool);
        return 1;
    }
    if (!rc->fn)
        return 0;
    /* Copy out of the pool frame so callbacks may safely call ps_get. */
    memcpy(rc->buf, p + SLOT_HDR, rc->t->hdr.record_size);
    return rc->fn(key, rc->buf, rc->ctx);
}

long ps_range(ps_table *t, int32_t lo, int32_t hi, ps_record_fn fn, void *ctx)
{
    if (!t)
        return PS_ERR_INVALID;
    pthread_rwlock_rdlock(&t->rwlock);
    range_ctx rc;
    rc.t = t;
    rc.fn = fn;
    rc.ctx = ctx;
    rc.err = PS_OK;
    size_t n = avl_range(&t->tree, lo, hi, range_visit, &rc);
    pthread_rwlock_unlock(&t->rwlock);
    return rc.err != PS_OK ? rc.err : (long)n;
}

long ps_scan(ps_table *t, ps_record_fn fn, void *ctx)
{
    if (!t)
        return PS_ERR_INVALID;
    pthread_rwlock_rdlock(&t->rwlock);
    uint8_t buf[PS_MAX_RECORD_SIZE];
    long visited = 0;
    for (uint32_t s = 0; s < t->hdr.total_slots; s++) {
        uint8_t *p = slot_ptr(t, s, 0);
        if (!p) {
            int err = bp_err_to_ps(t->pool);
            pthread_rwlock_unlock(&t->rwlock);
            return err;
        }
        uint32_t state;
        int32_t key;
        memcpy(&state, p, 4);
        if (state != SLOT_LIVE)
            continue;
        memcpy(&key, p + 4, 4);
        visited++;
        if (fn) {
            memcpy(buf, p + SLOT_HDR, t->hdr.record_size);
            if (fn(key, buf, ctx))
                break;
        }
    }
    pthread_rwlock_unlock(&t->rwlock);
    return visited;
}

/* ------------------------------------------------------------------ */
/* VACUUM: rewrite live records in key order into a fresh checksummed  */
/* file, then atomically rename it over the old one. Reclaims dead     */
/* slots and clusters records by key so range scans touch fewer pages. */
/* ------------------------------------------------------------------ */

int ps_vacuum(ps_table *t, ps_vacuum_result *res)
{
    if (!t)
        return PS_ERR_INVALID;
    pthread_rwlock_wrlock(&t->rwlock);
    int rc = flush_unlocked(t);
    if (rc != PS_OK) {
        pthread_rwlock_unlock(&t->rwlock);
        return rc;
    }

    struct stat st;
    if (fstat(t->fd, &st) != 0) {
        pthread_rwlock_unlock(&t->rwlock);
        return PS_ERR_IO;
    }
    uint64_t bytes_before = (uint64_t)st.st_size;
    uint32_t old_total = t->hdr.total_slots;

    collect_ctx c = { NULL, 0 };
    if (t->tree.count > 0) {
        c.entries = malloc(t->tree.count * sizeof *c.entries);
        if (!c.entries) {
            pthread_rwlock_unlock(&t->rwlock);
            return PS_ERR_NOMEM;
        }
        avl_range(&t->tree, INT32_MIN, INT32_MAX, collect_entry, &c);
    }

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.vacuum", t->dat_path) >= (int)sizeof tmp) {
        free(c.entries);
        pthread_rwlock_unlock(&t->rwlock);
        return PS_ERR_INVALID;
    }
    int nfd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (nfd < 0) {
        free(c.entries);
        pthread_rwlock_unlock(&t->rwlock);
        return PS_ERR_IO;
    }

    dat_header nh = t->hdr;
    nh.total_slots = (uint32_t)c.n;
    nh.live_records = (uint32_t)c.n;
    nh.clean_shutdown = 0;
    nh.header_crc = header_crc(&nh);

    uint8_t page[PS_PAGE_SIZE];
    memset(page, 0, sizeof page);
    memcpy(page, &nh, sizeof nh);
    rc = write_full(nfd, page, sizeof page, 0) == 0 ? PS_OK : PS_ERR_IO;

    uint32_t spp = t->hdr.slots_per_page, ss = t->hdr.slot_size;
    for (size_t i = 0; i < c.n && rc == PS_OK; i++) {
        if (i % spp == 0)
            memset(page, 0, sizeof page);
        uint8_t *src = slot_ptr(t, c.entries[i].slot, 0);
        if (!src) {
            rc = bp_err_to_ps(t->pool);
            break;
        }
        memcpy(page + BP_PAGE_HDR + (i % spp) * ss, src, ss);
        if (i % spp == spp - 1 || i == c.n - 1) {
            uint32_t pno = (uint32_t)(1 + i / spp);
            uint32_t crc = crc32_compute(page + BP_PAGE_HDR, PS_PAGE_SIZE - BP_PAGE_HDR);
            memcpy(page, &pno, 4);
            memcpy(page + 4, &crc, 4);
            if (write_full(nfd, page, sizeof page, (off_t)pno * PS_PAGE_SIZE) != 0)
                rc = PS_ERR_IO;
        }
    }
    if (rc == PS_OK && fsync(nfd) != 0)
        rc = PS_ERR_IO;
    if (rc == PS_OK && rename(tmp, t->dat_path) != 0)
        rc = PS_ERR_IO;
    if (rc != PS_OK) {
        close(nfd);
        unlink(tmp);
        free(c.entries);
        pthread_rwlock_unlock(&t->rwlock);
        return rc;
    }
    fsync_dir(t->db->dir);

    /* Swap in the new file; old pages in the pool and freelist are gone. */
    close(t->fd);
    t->fd = nfd;
    bp_discard_all(t->pool);
    bp_set_fd(t->pool, nfd);
    t->hdr = nh;
    t->free_count = 0;

    /* New slot numbers are simply 0..n-1 in key order; rebuild Bloom filter cleanly. */
    uint64_t prev_rejs = atomic_load(&t->bloom_rejections);
    bloom_clear(&t->bloom);
    atomic_store(&t->bloom_rejections, prev_rejs);
    for (size_t i = 0; i < c.n; i++) {
        avl_set(&t->tree, c.entries[i].key, (uint32_t)i);
        ht_put(t->hash, c.entries[i].key, (int64_t)i);
        bloom_add_i32(&t->bloom, c.entries[i].key);
    }
    free(c.entries);

    if (fstat(t->fd, &st) != 0) {
        pthread_rwlock_unlock(&t->rwlock);
        return PS_ERR_IO;
    }
    if (res) {
        res->live_records = (uint32_t)c.n;
        res->slots_reclaimed = old_total - (uint32_t)c.n;
        res->bytes_before = bytes_before;
        res->bytes_after = (uint64_t)st.st_size;
    }
    pthread_rwlock_unlock(&t->rwlock);
    return PS_OK;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

void ps_table_get_stats(const ps_table *t, ps_table_stats *st)
{
    memset(st, 0, sizeof *st);
    if (!t)
        return;
    ps_table *mut = (ps_table *)t;
    pthread_rwlock_rdlock(&mut->rwlock);
    bp_stats_t bs;
    bp_get_stats(t->pool, &bs);
    st->record_size = t->hdr.record_size;
    st->slot_size = t->hdr.slot_size;
    st->slots_per_page = t->hdr.slots_per_page;
    st->live_records = t->hdr.live_records;
    st->total_slots = t->hdr.total_slots;
    st->dead_slots = t->hdr.total_slots - t->hdr.live_records;
    st->data_pages = t->hdr.total_slots
                   ? 1u + (t->hdr.total_slots - 1u) / t->hdr.slots_per_page
                   : 0u;
    st->slots_reused = t->slots_reused;
    st->hash_buckets = ht_buckets(t->hash);
    st->avl_height = avl_height(&t->tree);
    st->index_rebuilt = t->index_rebuilt;
    st->pool_pages = bs.frames;
    st->cache_hits = bs.hits;
    st->cache_misses = bs.misses;
    st->cache_evictions = bs.evictions;
    st->cache_writebacks = bs.writebacks;
    st->bloom_rejections = atomic_load_explicit(&t->bloom_rejections, memory_order_relaxed);
    st->wal_records_logged = t->wal.records_logged;
    st->wal_records_replayed = t->wal.records_replayed;
    pthread_rwlock_unlock(&mut->rwlock);
}

void ps_reset_cache_stats(ps_table *t)
{
    if (!t)
        return;
    pthread_rwlock_wrlock(&t->rwlock);
    bp_reset_stats(t->pool);
    pthread_rwlock_unlock(&t->rwlock);
}

uint32_t ps_record_size(const ps_table *t) { return t ? t->hdr.record_size : 0; }
const char *ps_table_name(const ps_table *t) { return t ? t->name : ""; }
