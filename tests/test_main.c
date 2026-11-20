#define _POSIX_C_SOURCE 200809L
#include "avl.h"
#include "bloom.h"
#include "btree.h"
#include "bufferpool.h"
#include "crc32.h"
#include "hashtable.h"
#include "pagestore.h"
#include "secindex.h"
#include "varrec.h"
#include "wal.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks, g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                    \
    } while (0)

#define TEST_DIR "build/testdb"

typedef struct {
    int32_t id;
    char    payload[60];
} rec_t;

static void make_rec(rec_t *r, int32_t id)
{
    memset(r, 0, sizeof *r);
    r->id = id;
    snprintf(r->payload, sizeof r->payload, "record-%d", id);
}

static void wipe_dir(void)
{
    if (system("rm -rf " TEST_DIR) != 0)
        fprintf(stderr, "warning: could not clean " TEST_DIR "\n");
}

static uint32_t lcg(uint32_t *s) { return *s = *s * 1664525u + 1013904223u; }

/* ------------------------------------------------------------------ */

static void test_crc32(void)
{
    /* standard check value for "123456789" */
    CHECK(crc32_compute("123456789", 9) == 0xCBF43926u);
    CHECK(crc32_compute("", 0) == 0);
}

static void test_hashtable(void)
{
    ht_t *h = ht_create(4);
    CHECK(h != NULL);
    for (int32_t i = 0; i < 50000; i++)
        CHECK(ht_put(h, i * 7, i) == 0);
    CHECK(ht_size(h) == 50000);
    CHECK(ht_buckets(h) * 3 >= ht_size(h) * 4);   /* load factor <= 0.75 */

    int64_t v = 0;
    CHECK(ht_get(h, 7 * 1234, &v) == 1 && v == 1234);
    CHECK(ht_get(h, 5, &v) == 0);
    CHECK(ht_put(h, 7, 999) == 0 && ht_get(h, 7, &v) && v == 999);  /* overwrite */
    CHECK(ht_size(h) == 50000);
    CHECK(ht_remove(h, 7) == 1 && ht_remove(h, 7) == 0);
    CHECK(ht_get(h, 7, NULL) == 0);
    CHECK(ht_put(h, -42, 1) == 0 && ht_get(h, -42, &v) && v == 1);
    ht_free(h);
}

static int count_visit(int32_t k, uint32_t v, void *ctx)
{
    (void)k; (void)v;
    (*(int *)ctx)++;
    return 0;
}

typedef struct { int32_t last; int ordered; } order_ctx;

static int order_visit(int32_t k, uint32_t v, void *ctx)
{
    (void)v;
    order_ctx *o = ctx;
    if (k <= o->last) o->ordered = 0;
    o->last = k;
    return 0;
}

static void test_avl(void)
{
    avl_tree t;
    avl_init(&t);

    /* Sorted inserts are the worst case for a plain BST (height n).
     * AVL must stay within 1.44 * log2(n). */
    const int n = 100000;
    for (int32_t i = 0; i < n; i++)
        CHECK(avl_insert(&t, i, (uint32_t)i) == 0);
    CHECK(t.count == (size_t)n);
    CHECK(avl_height(&t) <= 25);      /* 1.44 * log2(100000) ~ 23.9 */
    CHECK(avl_validate(&t));
    CHECK(avl_insert(&t, 500, 1) == 1);  /* duplicate */

    uint32_t v;
    CHECK(avl_find(&t, 99999, &v) == 1 && v == 99999);
    CHECK(avl_find(&t, n, &v) == 0);
    CHECK(avl_set(&t, 10, 777) == 1 && avl_find(&t, 10, &v) && v == 777);

    int cnt = 0;
    CHECK(avl_range(&t, 100, 199, count_visit, &cnt) == 100 && cnt == 100);
    CHECK(avl_range(&t, 50, 10, count_visit, &cnt) == 0);

    /* delete every even key, tree must remain balanced and ordered */
    for (int32_t i = 0; i < n; i += 2)
        CHECK(avl_remove(&t, i) == 1);
    CHECK(avl_remove(&t, 0) == 0);
    CHECK(t.count == (size_t)n / 2);
    CHECK(avl_validate(&t));
    order_ctx o = { INT32_MIN, 1 };
    avl_range(&t, INT32_MIN, INT32_MAX, order_visit, &o);
    CHECK(o.ordered);
    CHECK(avl_find(&t, 2, NULL) == 0 && avl_find(&t, 3, NULL) == 1);

    avl_free(&t);
    CHECK(t.root == NULL && t.count == 0);
}

static void test_bufferpool(void)
{
    char path[] = "build/bp_test.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    bufferpool_t *bp = bp_create(fd, 4, 0);
    CHECK(bp != NULL);

    /* write 10 pages through a 4-frame pool: forces evictions + writebacks */
    for (uint32_t p = 0; p < 10; p++) {
        uint8_t *pg = bp_fetch(bp, p, 1);
        CHECK(pg != NULL);
        memset(pg, (int)('A' + p), BP_PAGE_SIZE);
    }
    bp_stats_t st;
    bp_get_stats(bp, &st);
    CHECK(st.misses == 10 && st.evictions == 6 && st.writebacks == 6);

    /* LRU: page 9 is resident (hit); page 0 was evicted (miss) */
    bp_reset_stats(bp);
    uint8_t *pg = bp_fetch(bp, 9, 0);
    CHECK(pg && pg[0] == 'J');
    pg = bp_fetch(bp, 0, 0);
    CHECK(pg && pg[100] == 'A');      /* written back on eviction, read back */
    bp_get_stats(bp, &st);
    CHECK(st.hits == 1 && st.misses == 1);

    CHECK(bp_flush_all(bp) == 0);
    bp_destroy(bp);

    struct stat sb;
    CHECK(fstat(fd, &sb) == 0 && sb.st_size == 10 * (off_t)BP_PAGE_SIZE);
    close(fd);
    unlink(path);
}

/* ------------------------------------------------------------------ */

static void test_crud_and_persistence(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r, out;

    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "users", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_create(db, "users", sizeof(rec_t)) == PS_ERR_EXISTS);
    CHECK(ps_table_create(db, "bad/name", sizeof(rec_t)) == PS_ERR_INVALID);
    CHECK(ps_table_create(db, "huge", PS_MAX_RECORD_SIZE + 1) == PS_ERR_INVALID);
    CHECK(ps_table_open(db, "missing", &t) == PS_ERR_NOT_FOUND);
    CHECK(ps_table_open(db, "users", &t) == PS_OK);
    ps_table *t2;
    CHECK(ps_table_open(db, "users", &t2) == PS_ERR_BUSY);

    for (int32_t i = 1; i <= 5000; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }
    make_rec(&r, 1);
    CHECK(ps_insert(t, 1, &r) == PS_ERR_DUPLICATE);

    CHECK(ps_get(t, 4321, &out) == PS_OK && out.id == 4321);
    CHECK(strcmp(out.payload, "record-4321") == 0);
    CHECK(ps_get(t, 9999, &out) == PS_ERR_NOT_FOUND);

    make_rec(&r, 42);
    snprintf(r.payload, sizeof r.payload, "updated");
    CHECK(ps_update(t, 42, &r) == PS_OK);
    CHECK(ps_get(t, 42, &out) == PS_OK && strcmp(out.payload, "updated") == 0);
    CHECK(ps_update(t, 99999, &r) == PS_ERR_NOT_FOUND);

    for (int32_t i = 1; i <= 5000; i += 2)
        CHECK(ps_delete(t, i) == PS_OK);
    CHECK(ps_delete(t, 1) == PS_ERR_NOT_FOUND);
    CHECK(ps_get(t, 1, &out) == PS_ERR_NOT_FOUND);

    CHECK(ps_range(t, 1, 100, NULL, NULL) == 50);   /* evens only */
    CHECK(ps_scan(t, NULL, NULL) == 2500);

    ps_table_stats st;
    ps_table_get_stats(t, &st);
    CHECK(st.live_records == 2500 && st.dead_slots == 2500 && st.index_rebuilt == 0);
    CHECK(ps_table_close(t) == PS_OK);

    /* reopen: index must come from .ndx, data intact */
    CHECK(ps_table_open(db, "users", &t) == PS_OK);
    ps_table_get_stats(t, &st);
    CHECK(st.index_rebuilt == 0 && st.live_records == 2500);
    CHECK(ps_get(t, 42, &out) == PS_OK && strcmp(out.payload, "updated") == 0);
    CHECK(ps_get(t, 4000, &out) == PS_OK && out.id == 4000);
    CHECK(ps_get(t, 4001, &out) == PS_ERR_NOT_FOUND);
    CHECK(ps_table_close(t) == PS_OK);

    char names[8][PS_MAX_NAME + 1];
    CHECK(ps_table_create(db, "accounts", 16) == PS_OK);
    CHECK(ps_db_list_tables(db, names, 8) == 2);
    CHECK(strcmp(names[0], "accounts") == 0 && strcmp(names[1], "users") == 0);
    ps_db_close(db);
}

typedef struct { int32_t last; int ordered; long n; } range_order;

static int range_order_cb(int32_t key, const void *rec, void *ctx)
{
    range_order *o = ctx;
    const rec_t *r = rec;
    if (key <= o->last || r->id != key) o->ordered = 0;
    o->last = key;
    o->n++;
    return 0;
}

static void test_vacuum(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r, out;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "v", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open_ex(db, "v", 8, &t) == PS_OK);   /* tiny pool */

    uint32_t seed = 7;
    for (int32_t i = 0; i < 20000; i++) {
        int32_t k = (int32_t)(lcg(&seed) % 1000000u) * 10 + (i % 10);
        make_rec(&r, k);
        int rc = ps_insert(t, k, &r);
        CHECK(rc == PS_OK || rc == PS_ERR_DUPLICATE);
    }
    ps_table_stats st;
    ps_table_get_stats(t, &st);
    uint32_t before_live = st.live_records;

    /* delete ~75% */
    long deleted = 0;
    seed = 7;
    for (int32_t i = 0; i < 20000; i++) {
        int32_t k = (int32_t)(lcg(&seed) % 1000000u) * 10 + (i % 10);
        if (i % 4 != 0 && ps_delete(t, k) == PS_OK)
            deleted++;
    }

    ps_vacuum_result vr;
    CHECK(ps_vacuum(t, &vr) == PS_OK);
    CHECK(vr.live_records == before_live - (uint32_t)deleted);
    CHECK(vr.bytes_after < vr.bytes_before);
    ps_table_get_stats(t, &st);
    CHECK(st.dead_slots == 0 && st.live_records == vr.live_records);

    /* everything still reachable, in order */
    range_order o = { INT32_MIN, 1, 0 };
    CHECK(ps_range(t, INT32_MIN, INT32_MAX, range_order_cb, &o) == (long)vr.live_records);
    CHECK(o.ordered && o.n == (long)vr.live_records);

    /* engine stays writable after vacuum, and survives reopen */
    make_rec(&r, -5);
    CHECK(ps_insert(t, -5, &r) == PS_OK);
    CHECK(ps_table_close(t) == PS_OK);
    CHECK(ps_table_open(db, "v", &t) == PS_OK);
    CHECK(ps_get(t, -5, &out) == PS_OK && out.id == -5);
    ps_table_get_stats(t, &st);
    CHECK(st.live_records == vr.live_records + 1 && st.index_rebuilt == 0);
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

static void test_corrupt_index_rebuild(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r, out;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "c", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open(db, "c", &t) == PS_OK);
    for (int32_t i = 0; i < 3000; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }
    CHECK(ps_delete(t, 10) == PS_OK);
    CHECK(ps_table_close(t) == PS_OK);

    /* flip one byte inside the index entries: CRC must catch it */
    int fd = open(TEST_DIR "/c.ndx", O_RDWR);
    CHECK(fd >= 0);
    uint8_t b;
    CHECK(pread(fd, &b, 1, 100) == 1);
    b ^= 0xFF;
    CHECK(pwrite(fd, &b, 1, 100) == 1);
    close(fd);

    CHECK(ps_table_open(db, "c", &t) == PS_OK);
    ps_table_stats st;
    ps_table_get_stats(t, &st);
    CHECK(st.index_rebuilt == 1 && st.live_records == 2999);
    CHECK(ps_get(t, 2999, &out) == PS_OK && out.id == 2999);
    CHECK(ps_get(t, 10, &out) == PS_ERR_NOT_FOUND);
    CHECK(ps_table_close(t) == PS_OK);

    /* missing index file entirely */
    unlink(TEST_DIR "/c.ndx");
    CHECK(ps_table_open(db, "c", &t) == PS_OK);
    ps_table_get_stats(t, &st);
    CHECK(st.index_rebuilt == 1 && st.live_records == 2999);
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

/* Child process writes, flushes, writes more and dies without closing.
 * Parent reopens: the unclean-shutdown flag forces a rebuild and every
 * record written before the flush must be present. */
static void test_crash_recovery(void)
{
    wipe_dir();
    ps_db *db;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "crash", sizeof(rec_t)) == PS_OK);
    ps_db_close(db);

    pid_t pid = fork();
    if (pid == 0) {
        ps_db *cdb;
        ps_table *ct;
        rec_t r;
        if (ps_db_open(TEST_DIR, &cdb) != PS_OK || ps_table_open_ex(cdb, "crash", 4, &ct) != PS_OK)
            _exit(2);
        for (int32_t i = 0; i < 4000; i++) {
            make_rec(&r, i);
            if (ps_insert(ct, i, &r) != PS_OK) _exit(3);
        }
        if (ps_delete(ct, 7) != PS_OK || ps_flush(ct) != PS_OK) _exit(4);
        for (int32_t i = 4000; i < 6000; i++) {   /* unflushed tail */
            make_rec(&r, i);
            ps_insert(ct, i, &r);
        }
        _exit(0);   /* simulated crash: no close, no index write */
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    ps_table *t;
    rec_t out;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_open(db, "crash", &t) == PS_OK);
    ps_table_stats st;
    ps_table_get_stats(t, &st);
    CHECK(st.index_rebuilt == 1);
    CHECK(st.live_records >= 3999);           /* every flushed record survived */
    int all_present = 1;
    for (int32_t i = 0; i < 4000; i++) {
        if (i == 7) continue;
        if (ps_get(t, i, &out) != PS_OK || out.id != i) all_present = 0;
    }
    CHECK(all_present);
    CHECK(ps_get(t, 7, &out) == PS_ERR_NOT_FOUND);   /* flushed delete stuck */
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

static void test_page_corruption(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r, out;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "chk", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open(db, "chk", &t) == PS_OK);
    for (int32_t i = 0; i < 200; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }
    CHECK(ps_table_close(t) == PS_OK);

    /* Flip 1 bit inside data page 1 payload (offset 4096 + 32): per-page CRC must catch it */
    int fd = open(TEST_DIR "/chk.dat", O_RDWR);
    CHECK(fd >= 0);
    uint8_t b;
    CHECK(pread(fd, &b, 1, PS_PAGE_SIZE + 32) == 1);
    b ^= 0x01;
    CHECK(pwrite(fd, &b, 1, PS_PAGE_SIZE + 32) == 1);
    close(fd);

    CHECK(ps_table_open(db, "chk", &t) == PS_OK);
    CHECK(ps_get(t, 0, &out) == PS_ERR_CORRUPT);   /* key 0 sits on corrupted page 1 */
    CHECK(ps_get(t, 199, &out) == PS_OK && out.id == 199); /* key 199 sits on intact later page */
    CHECK(ps_table_close(t) == PS_OK);

    /* Flip 1 bit in page 0 header: open must reject as PS_ERR_CORRUPT */
    fd = open(TEST_DIR "/chk.dat", O_RDWR);
    CHECK(fd >= 0);
    CHECK(pread(fd, &b, 1, 12) == 1);
    b ^= 0x80;
    CHECK(pwrite(fd, &b, 1, 12) == 1);
    close(fd);
    CHECK(ps_table_open(db, "chk", &t) == PS_ERR_CORRUPT);
    ps_db_close(db);
}

static void test_slot_recycling(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r, out;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "churn", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open(db, "churn", &t) == PS_OK);

    for (int32_t i = 0; i < 2000; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }
    CHECK(ps_flush(t) == PS_OK);
    struct stat sb_before, sb_after;
    CHECK(stat(TEST_DIR "/churn.dat", &sb_before) == 0);

    /* Delete 1000 keys, then insert 1000 new keys: file size must NOT grow */
    for (int32_t i = 0; i < 1000; i++)
        CHECK(ps_delete(t, i) == PS_OK);
    for (int32_t i = 2000; i < 3000; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }
    CHECK(ps_flush(t) == PS_OK);
    CHECK(stat(TEST_DIR "/churn.dat", &sb_after) == 0);
    ps_table_stats st;
    ps_table_get_stats(t, &st);
    CHECK(st.total_slots == 2000 && st.live_records == 2000 && st.dead_slots == 0);
    CHECK(st.slots_reused == 1000);
    CHECK(sb_after.st_size == sb_before.st_size);

    /* Delete 500, close cleanly, reopen (.ndx bitset freelist rebuild), insert 500 */
    for (int32_t i = 1000; i < 1500; i++)
        CHECK(ps_delete(t, i) == PS_OK);
    CHECK(ps_table_close(t) == PS_OK);

    CHECK(ps_table_open(db, "churn", &t) == PS_OK);
    ps_table_get_stats(t, &st);
    CHECK(st.index_rebuilt == 0 && st.dead_slots == 500);
    for (int32_t i = 5000; i < 5500; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }
    ps_table_get_stats(t, &st);
    CHECK(st.total_slots == 2000 && st.dead_slots == 0 && st.slots_reused == 500);
    CHECK(ps_get(t, 5250, &out) == PS_OK && out.id == 5250);
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

static void test_avl_bulk_and_boundaries(void)
{
    /* O(n) sorted bulk-load check */
    const size_t n = 100000;
    avl_kv *items = malloc(n * sizeof *items);
    CHECK(items != NULL);
    for (size_t i = 0; i < n; i++) {
        items[i].key = (int32_t)i;
        items[i].val = (uint32_t)i;
    }
    avl_tree tree;
    avl_init(&tree);
    CHECK(avl_build_sorted(&tree, items, n) == 0);
    CHECK(tree.count == n && avl_height(&tree) == 17 && avl_validate(&tree));

    /* Unsorted input must be rejected */
    items[500].key = 10;
    CHECK(avl_build_sorted(&tree, items, n) == -1);
    avl_free(&tree);
    free(items);

    /* Boundary keys: INT32_MIN, -1, 0, 1, INT32_MAX */
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r, out;
    int32_t keys[] = { INT32_MIN, -1, 0, 1, INT32_MAX };
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "bounds", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open(db, "bounds", &t) == PS_OK);
    for (size_t i = 0; i < 5; i++) {
        make_rec(&r, keys[i]);
        CHECK(ps_insert(t, keys[i], &r) == PS_OK);
        CHECK(ps_exists(t, keys[i]) == 1);
    }
    CHECK(ps_count(t) == 5 && ps_exists(t, 42) == 0);
    CHECK(ps_range(t, INT32_MIN, INT32_MAX, NULL, NULL) == 5);
    CHECK(ps_table_close(t) == PS_OK);

    CHECK(ps_table_open(db, "bounds", &t) == PS_OK);
    for (size_t i = 0; i < 5; i++)
        CHECK(ps_get(t, keys[i], &out) == PS_OK && out.id == keys[i]);
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

typedef struct {
    ps_table *t;
    int       ok;
    int       seen;
} reent_ctx;

static int reentrant_cb(int32_t key, const void *rec, void *ctx)
{
    reent_ctx *rc = ctx;
    const rec_t *r = rec;
    /* Force buffer pool evictions on a tiny 2-page pool by reading distant keys */
    rec_t other;
    if (ps_get(rc->t, (key + 500) % 1000, &other) != PS_OK)
        rc->ok = 0;
    /* Original record pointer must remain intact even after eviction */
    if (r->id != key)
        rc->ok = 0;
    rc->seen++;
    return 0;
}

static void test_drop_and_reentrant_scan(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "reent", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open_ex(db, "reent", 2, &t) == PS_OK);   /* 2-page pool */
    for (int32_t i = 0; i < 1000; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }

    reent_ctx ctx = { t, 1, 0 };
    CHECK(ps_range(t, 0, 99, reentrant_cb, &ctx) == 100 && ctx.ok && ctx.seen == 100);
    ctx.seen = 0;
    CHECK(ps_scan(t, reentrant_cb, &ctx) == 1000 && ctx.ok && ctx.seen == 1000);

    /* Drop must fail while table is open, succeed after close */
    CHECK(ps_table_drop(db, "reent") == PS_ERR_BUSY);
    CHECK(ps_table_close(t) == PS_OK);
    CHECK(ps_table_drop(db, "reent") == PS_OK);
    CHECK(ps_table_drop(db, "reent") == PS_ERR_NOT_FOUND);
    char names[4][PS_MAX_NAME + 1];
    CHECK(ps_db_list_tables(db, names, 4) == 0);
    ps_db_close(db);
}

static void test_wal_unflushed_durability(void)
{
    wipe_dir();
    ps_db *db;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "wal_dur", sizeof(rec_t)) == PS_OK);
    ps_db_close(db);

    /* Child writes 500 records into a 256-page buffer pool (zero evictions, zero flushes)
     * and exits abruptly. Only the WAL has the records. */
    pid_t pid = fork();
    if (pid == 0) {
        ps_db *cdb;
        ps_table *ct;
        rec_t r;
        if (ps_db_open(TEST_DIR, &cdb) != PS_OK ||
            ps_table_open_ex(cdb, "wal_dur", 256, &ct) != PS_OK)
            _exit(2);
        for (int32_t i = 0; i < 500; i++) {
            make_rec(&r, i);
            if (ps_insert(ct, i, &r) != PS_OK) _exit(3);
        }
        make_rec(&r, 42);
        snprintf(r.payload, sizeof r.payload, "wal-updated-42");
        if (ps_update(ct, 42, &r) != PS_OK) _exit(4);
        if (ps_delete(ct, 13) != PS_OK) _exit(5);
        _exit(0); /* abrupt crash without ps_flush or ps_table_close */
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    ps_table *t;
    rec_t out;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_open(db, "wal_dur", &t) == PS_OK);
    ps_table_stats st;
    ps_table_get_stats(t, &st);
    CHECK(st.index_rebuilt == 1);
    CHECK(st.live_records == 499);
    CHECK(st.wal_records_replayed == 502);
    CHECK(ps_get(t, 42, &out) == PS_OK && strcmp(out.payload, "wal-updated-42") == 0);
    CHECK(ps_get(t, 13, &out) == PS_ERR_NOT_FOUND);
    CHECK(ps_get(t, 499, &out) == PS_OK && out.id == 499);
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

static void test_bloom_and_dynamic_tables(void)
{
    wipe_dir();
    ps_db *db;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);

    /* Open 24 tables simultaneously (> initial 16 capacity) */
    ps_table *tables[24];
    for (int i = 0; i < 24; i++) {
        char name[32];
        snprintf(name, sizeof name, "tbl_%02d", i);
        CHECK(ps_table_create(db, name, sizeof(rec_t)) == PS_OK);
        CHECK(ps_table_open(db, name, &tables[i]) == PS_OK);
    }

    rec_t r, out;
    for (int32_t k = 0; k < 200; k++) {
        make_rec(&r, k);
        CHECK(ps_insert(tables[0], k, &r) == PS_OK);
    }
    /* Query 1,000 missing keys: Bloom filter must reject almost all of them in O(1) */
    for (int32_t k = 10000; k < 11000; k++)
        CHECK(ps_get(tables[0], k, &out) == PS_ERR_NOT_FOUND);

    ps_table_stats st;
    ps_table_get_stats(tables[0], &st);
    CHECK(st.bloom_rejections >= 990);

    for (int i = 0; i < 24; i++)
        CHECK(ps_table_close(tables[i]) == PS_OK);
    ps_db_close(db);
}

typedef struct {
    ps_table *t;
    int32_t   thread_id;
    int       errors;
} mt_arg;

static void *mt_reader(void *arg)
{
    mt_arg *a = arg;
    rec_t out;
    for (int i = 0; i < 500; i++) {
        int32_t k = i % 200;
        if (ps_get(a->t, k, &out) != PS_OK || out.id != k)
            a->errors++;
    }
    return NULL;
}

static void *mt_writer(void *arg)
{
    mt_arg *a = arg;
    rec_t r;
    int32_t base = 1000 + a->thread_id * 500;
    for (int32_t i = 0; i < 200; i++) {
        make_rec(&r, base + i);
        if (ps_insert(a->t, base + i, &r) != PS_OK)
            a->errors++;
    }
    return NULL;
}

static void test_concurrency_rwlock(void)
{
    wipe_dir();
    ps_db *db;
    ps_table *t;
    rec_t r;
    CHECK(ps_db_open(TEST_DIR, &db) == PS_OK);
    CHECK(ps_table_create(db, "mt", sizeof(rec_t)) == PS_OK);
    CHECK(ps_table_open_ex(db, "mt", 16, &t) == PS_OK);

    for (int32_t i = 0; i < 200; i++) {
        make_rec(&r, i);
        CHECK(ps_insert(t, i, &r) == PS_OK);
    }

    pthread_t r_tids[6], w_tids[2];
    mt_arg r_args[6], w_args[2];
    for (int i = 0; i < 6; i++) {
        r_args[i].t = t;
        r_args[i].thread_id = i;
        r_args[i].errors = 0;
        pthread_create(&r_tids[i], NULL, mt_reader, &r_args[i]);
    }
    for (int i = 0; i < 2; i++) {
        w_args[i].t = t;
        w_args[i].thread_id = i;
        w_args[i].errors = 0;
        pthread_create(&w_tids[i], NULL, mt_writer, &w_args[i]);
    }
    for (int i = 0; i < 6; i++) {
        pthread_join(r_tids[i], NULL);
        CHECK(r_args[i].errors == 0);
    }
    for (int i = 0; i < 2; i++) {
        pthread_join(w_tids[i], NULL);
        CHECK(w_args[i].errors == 0);
    }
    CHECK(ps_count(t) == 600);
    CHECK(ps_table_close(t) == PS_OK);
    ps_db_close(db);
}

static void test_btree_index(void)
{
    wipe_dir();
    mkdir(TEST_DIR, 0755);
    ps_btree_t *bt = NULL;
    CHECK(ps_btree_open(TEST_DIR "/idx.btree", 16, &bt) == PS_OK);

    /* Insert 2,000 keys (forces multiple leaf and internal node splits) */
    for (int32_t i = 0; i < 2000; i++) {
        int32_t k = (i * 73) % 2000;
        CHECK(ps_btree_insert(bt, k, (uint32_t)(k * 10)) == PS_OK);
    }
    CHECK(ps_btree_insert(bt, 10, 999) == PS_ERR_DUPLICATE);
    CHECK(ps_btree_count(bt) == 2000);
    CHECK(ps_btree_height(bt) >= 2);

    uint32_t val = 0;
    CHECK(ps_btree_find(bt, 1234, &val) == 1 && val == 12340);
    CHECK(ps_btree_find(bt, 9999, &val) == 0);
    CHECK(ps_btree_range(bt, 100, 199, NULL, NULL) == 100);
    CHECK(ps_btree_remove(bt, 150) == 1 && ps_btree_find(bt, 150, &val) == 0);
    CHECK(ps_btree_range(bt, 100, 199, NULL, NULL) == 99);
    CHECK(ps_btree_close(bt) == PS_OK);

    /* Reopen and verify persistence */
    CHECK(ps_btree_open(TEST_DIR "/idx.btree", 16, &bt) == PS_OK);
    CHECK(ps_btree_count(bt) == 1999);
    CHECK(ps_btree_find(bt, 1999, &val) == 1 && val == 19990);
    CHECK(ps_btree_close(bt) == PS_OK);
}

static void test_varrec_and_secindex(void)
{
    wipe_dir();
    mkdir(TEST_DIR, 0755);
    ps_vartable_t *vt = NULL;
    CHECK(ps_var_open(TEST_DIR "/docs.var", 16, &vt) == PS_OK);

    const char *json1 = "{\"user\":\"alice\",\"role\":\"engineer\"}";
    CHECK(ps_var_put(vt, ps_key_str("user:alice"), json1, (uint32_t)strlen(json1) + 1) == PS_OK);

    /* Large 12KB payload spanning multiple 4KB overflow pages */
    uint8_t *big = malloc(12000);
    uint8_t *big_out = malloc(12000);
    CHECK(big != NULL && big_out != NULL);
    for (size_t i = 0; i < 12000; i++)
        big[i] = (uint8_t)((i * 31 + 7) & 0xFF);
    CHECK(ps_var_put(vt, ps_key_str("blob:report"), big, 12000) == PS_OK);

    char small_out[128];
    uint32_t out_len = 0;
    CHECK(ps_var_get(vt, ps_key_str("user:alice"), small_out, sizeof small_out, &out_len) == PS_OK);
    CHECK(strcmp(small_out, json1) == 0);

    CHECK(ps_var_get(vt, ps_key_str("blob:report"), big_out, 12000, &out_len) == PS_OK);
    CHECK(out_len == 12000 && memcmp(big, big_out, 12000) == 0);

    CHECK(ps_var_delete(vt, ps_key_str("user:alice")) == PS_OK);
    CHECK(ps_var_get(vt, ps_key_str("user:alice"), small_out, sizeof small_out, &out_len) == PS_ERR_NOT_FOUND);
    CHECK(ps_var_count(vt) == 1);
    CHECK(ps_var_close(vt) == PS_OK);
    free(big);
    free(big_out);

    /* Secondary index test */
    ps_secindex_t *si = ps_secindex_create(16);
    CHECK(si != NULL);
    CHECK(ps_secindex_put(si, "Cardiology", 101) == 0);
    CHECK(ps_secindex_put(si, "Cardiology", 102) == 0);
    CHECK(ps_secindex_put(si, "Pediatrics", 201) == 0);

    int32_t pks[8];
    CHECK(ps_secindex_lookup(si, "Cardiology", pks, 8) == 2);
    CHECK(pks[0] == 101 && pks[1] == 102);
    CHECK(ps_secindex_remove(si, "Cardiology", 101) == 1);
    CHECK(ps_secindex_lookup(si, "Cardiology", pks, 8) == 1 && pks[0] == 102);
    ps_secindex_destroy(si);
}

int main(void)
{
    mkdir("build", 0755);
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "crc32",                   test_crc32 },
        { "hashtable",               test_hashtable },
        { "avl",                     test_avl },
        { "bufferpool",              test_bufferpool },
        { "crud_and_persistence",    test_crud_and_persistence },
        { "vacuum",                  test_vacuum },
        { "corrupt_index_rebuild",   test_corrupt_index_rebuild },
        { "crash_recovery",          test_crash_recovery },
        { "page_corruption",         test_page_corruption },
        { "slot_recycling",          test_slot_recycling },
        { "avl_bulk_and_boundaries", test_avl_bulk_and_boundaries },
        { "drop_and_reentrant_scan", test_drop_and_reentrant_scan },
        { "wal_unflushed_durability",test_wal_unflushed_durability },
        { "bloom_and_dynamic_tables",test_bloom_and_dynamic_tables },
        { "concurrency_rwlock",      test_concurrency_rwlock },
        { "btree_index",             test_btree_index },
        { "varrec_and_secindex",     test_varrec_and_secindex },
    };
    size_t n = sizeof tests / sizeof tests[0];
    for (size_t i = 0; i < n; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("  %-26s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }
    wipe_dir();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
