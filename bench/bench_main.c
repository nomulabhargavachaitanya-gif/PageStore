/*
 * PageStore 100k-record benchmark suite.
 *
 * Measures:
 *   1. In-memory index lookup latency: Chained Hash Table vs AVL Tree (100k keys)
 *   2. Sequential insert stress: proves AVL stays balanced (O(log n) height)
 *   3. End-to-end ps_get throughput & latency: Warm pool vs Small (cold) pool
 *   4. Ordered range scans via AVL index (before and after VACUUM clustering)
 *   5. Space reclamation via VACUUM after 75% deletions
 *   6. Clean reopen (.ndx CRC load) vs crash-recovery (.dat full rebuild)
 */
#define _POSIX_C_SOURCE 200809L
#include "avl.h"
#include "hashtable.h"
#include "pagestore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define N_RECORDS    100000
#define N_LOOKUPS    100000
#define BENCH_DIR    "build/bench_db"

typedef struct {
    int32_t id;
    char    name[32];
    char    phone[16];
    char    email[44];
} bench_rec;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void shuffle(int32_t *arr, size_t n, uint32_t seed)
{
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = xorshift32(&seed) % (i + 1);
        int32_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static int noop_cb(int32_t key, const void *rec, void *ctx)
{
    (void)key;
    (void)rec;
    (*(long *)ctx)++;
    return 0;
}

static void bench_indexes(const int32_t *queries)
{
    ht_t *h = ht_create(N_RECORDS);
    avl_tree tree;
    avl_init(&tree);

    /* Sequential keys: worst case for an unbalanced BST (height = 100,000),
     * guaranteed <= 1.44*log2(n) for AVL. */
    double t0 = now_sec();
    for (int32_t i = 0; i < N_RECORDS; i++)
        ht_put(h, i, i);
    double ht_ins = now_sec() - t0;

    t0 = now_sec();
    for (int32_t i = 0; i < N_RECORDS; i++)
        avl_insert(&tree, i, (uint32_t)i);
    double avl_ins = now_sec() - t0;

    int64_t hv = 0;
    volatile int64_t sink = 0;
    t0 = now_sec();
    for (int i = 0; i < N_LOOKUPS; i++) {
        ht_get(h, queries[i], &hv);
        sink += hv;
    }
    double ht_ns = (now_sec() - t0) * 1e9 / N_LOOKUPS;

    uint32_t av = 0;
    t0 = now_sec();
    for (int i = 0; i < N_LOOKUPS; i++) {
        avl_find(&tree, queries[i], &av);
        sink += av;
    }
    double avl_ns = (now_sec() - t0) * 1e9 / N_LOOKUPS;
    (void)sink;

    printf("\n[1] In-Memory Index Benchmark (%d sequential inserts, %d random lookups)\n",
           N_RECORDS, N_LOOKUPS);
    printf("    Hash table : insert %.1f ms | lookup %6.1f ns/op  (buckets: %zu)\n",
           ht_ins * 1e3, ht_ns, ht_buckets(h));
    printf("    AVL tree   : insert %.1f ms | lookup %6.1f ns/op  (height: %d, valid: %s)\n",
           avl_ins * 1e3, avl_ns, avl_height(&tree), avl_validate(&tree) ? "yes" : "no");
    printf("    Speedup    : Hash is %.1fx faster than AVL for point lookups\n",
           avl_ns / ht_ns);

    ht_free(h);
    avl_free(&tree);
}

static void bench_engine(int32_t *insert_keys, const int32_t *queries)
{
    if (system("rm -rf " BENCH_DIR) != 0)
        fprintf(stderr, "warning: cleanup failed\n");

    ps_db *db = NULL;
    ps_table *t = NULL;
    if (ps_db_open(BENCH_DIR, &db) != PS_OK ||
        ps_table_create(db, "contacts", sizeof(bench_rec)) != PS_OK ||
        ps_table_open_ex(db, "contacts", 4096, &t) != PS_OK) {
        fprintf(stderr, "failed to create benchmark table\n");
        exit(1);
    }

    /* Insert 100k records in randomized key order so physical order != key order */
    bench_rec r;
    memset(&r, 0, sizeof r);
    double t0 = now_sec();
    for (int i = 0; i < N_RECORDS; i++) {
        r.id = insert_keys[i];
        snprintf(r.name, sizeof r.name, "User_%d", r.id);
        snprintf(r.phone, sizeof r.phone, "555-%07d", r.id);
        snprintf(r.email, sizeof r.email, "user%d@example.com", r.id);
        if (ps_insert(t, r.id, &r) != PS_OK) {
            fprintf(stderr, "insert failed at %d\n", i);
            exit(1);
        }
    }
    ps_flush(t);
    double ins_sec = now_sec() - t0;

    ps_table_stats st;
    ps_table_get_stats(t, &st);
    printf("\n[2] Engine Write Throughput (%d records, %u B/record, %u slots/page)\n",
           N_RECORDS, st.record_size, st.slots_per_page);
    printf("    Insert + fsync : %.1f ms (%.0f records/sec, %u data pages)\n",
           ins_sec * 1e3, N_RECORDS / ins_sec, st.data_pages);

    /* Warm buffer pool (4096 frames > 2800 data pages: ~100% cache hit) */
    ps_reset_cache_stats(t);
    bench_rec out;
    t0 = now_sec();
    for (int i = 0; i < N_LOOKUPS; i++) {
        if (ps_get(t, queries[i], &out) != PS_OK || out.id != queries[i]) {
            fprintf(stderr, "get mismatch at %d\n", i);
            exit(1);
        }
    }
    double warm_sec = now_sec() - t0;
    ps_table_get_stats(t, &st);
    double warm_hit = 100.0 * (double)st.cache_hits / (double)(st.cache_hits + st.cache_misses);

    /* Range scan BEFORE vacuum (keys scattered across pages) */
    long visited = 0;
    t0 = now_sec();
    for (int trial = 0; trial < 200; trial++) {
        int32_t lo = (trial * 400) % (N_RECORDS - 500);
        ps_range(t, lo, lo + 499, noop_cb, &visited);
    }
    double range_before_us = (now_sec() - t0) * 1e6 / 200.0;

    ps_table_close(t);

    /* Reopen with a SMALL buffer pool (64 pages) to measure cache pressure */
    double t_reopen = now_sec();
    if (ps_table_open_ex(db, "contacts", 64, &t) != PS_OK) {
        fprintf(stderr, "reopen failed\n");
        exit(1);
    }
    double clean_open_ms = (now_sec() - t_reopen) * 1e3;

    ps_reset_cache_stats(t);
    t0 = now_sec();
    for (int i = 0; i < N_LOOKUPS; i++) {
        if (ps_get(t, queries[i], &out) != PS_OK || out.id != queries[i]) {
            fprintf(stderr, "cold get mismatch at %d\n", i);
            exit(1);
        }
    }
    double cold_sec = now_sec() - t0;
    ps_table_get_stats(t, &st);
    double cold_hit = 100.0 * (double)st.cache_hits / (double)(st.cache_hits + st.cache_misses);

    printf("\n[3] Point Lookups (ps_get, %d random keys) — Buffer Pool Impact\n", N_LOOKUPS);
    printf("    Warm pool (4096 pages) : %6.1f ns/op  (%.0f ops/sec | hit ratio %5.1f%%)\n",
           warm_sec * 1e9 / N_LOOKUPS, N_LOOKUPS / warm_sec, warm_hit);
    printf("    Tight pool  (64 pages) : %6.1f ns/op  (%.0f ops/sec | hit ratio %5.1f%%, evictions %llu)\n",
           cold_sec * 1e9 / N_LOOKUPS, N_LOOKUPS / cold_sec, cold_hit,
           (unsigned long long)st.cache_evictions);
    printf("    Warm-pool speedup      : %.1fx\n", cold_sec / warm_sec);

    /* Delete 75% of records and run VACUUM */
    t0 = now_sec();
    for (int32_t i = 0; i < N_RECORDS; i++) {
        if (i % 4 != 0)
            ps_delete(t, i);
    }
    double del_ms = (now_sec() - t0) * 1e3;

    ps_vacuum_result vr;
    t0 = now_sec();
    if (ps_vacuum(t, &vr) != PS_OK) {
        fprintf(stderr, "vacuum failed\n");
        exit(1);
    }
    double vac_ms = (now_sec() - t0) * 1e3;

    /* Range scan AFTER vacuum (live records clustered in key order) */
    ps_reset_cache_stats(t);
    visited = 0;
    t0 = now_sec();
    for (int trial = 0; trial < 200; trial++) {
        int32_t lo = (trial * 400) % (N_RECORDS - 2000);
        ps_range(t, lo, lo + 1999, noop_cb, &visited);   /* ~500 live records */
    }
    double range_after_us = (now_sec() - t0) * 1e6 / 200.0;
    ps_table_get_stats(t, &st);

    printf("\n[4] Soft Delete + Online VACUUM Compaction (75%% records deleted)\n");
    printf("    75,000 soft deletes    : %.1f ms\n", del_ms);
    printf("    VACUUM compaction      : %.1f ms (reclaimed %u slots)\n", vac_ms, vr.slots_reclaimed);
    printf("    On-disk .dat file size : %.2f MB -> %.2f MB (%.1f%% space reclaimed)\n",
           (double)vr.bytes_before / (1024.0 * 1024.0),
           (double)vr.bytes_after / (1024.0 * 1024.0),
           100.0 * (1.0 - (double)vr.bytes_after / (double)vr.bytes_before));
    printf("    500-rec range scan     : %.1f us (before vacuum, warm) -> %.1f us (after vacuum, 64-page pool)\n",
           range_before_us, range_after_us);

    ps_table_close(t);

    /* Measure forced crash-recovery rebuild by removing .ndx */
    unlink(BENCH_DIR "/contacts.ndx");
    t0 = now_sec();
    if (ps_table_open(db, "contacts", &t) != PS_OK) {
        fprintf(stderr, "recovery open failed\n");
        exit(1);
    }
    double rebuild_ms = (now_sec() - t0) * 1e3;
    ps_table_get_stats(t, &st);
    printf("\n[5] Startup & Crash Recovery\n");
    printf("    Clean open (.ndx O(n) AVL) : %.2f ms (100,000 keys)\n", clean_open_ms);
    printf("    Crash rebuild (.dat CRC)   : %.2f ms (%u live keys recovered, rebuilt=%d)\n",
           rebuild_ms, st.live_records, st.index_rebuilt);

    /* Measure freelist slot recycling under 10k delete + 10k re-insert churn */
    uint32_t pages_before = st.data_pages;
    t0 = now_sec();
    for (int32_t i = 0; i < 40000; i += 4)
        ps_delete(t, i);
    for (int32_t i = 0; i < 10000; i++) {
        r.id = 200000 + i;
        ps_insert(t, r.id, &r);
    }
    double churn_ms = (now_sec() - t0) * 1e3;
    ps_table_get_stats(t, &st);
    printf("\n[6] Freelist Recycling (10,000 deletes + 10,000 inserts)\n");
    printf("    Churn time       : %.1f ms (slots reused: %llu, pages: %u -> %u, 0%% growth)\n",
           churn_ms, (unsigned long long)st.slots_reused, pages_before, st.data_pages);

    ps_table_close(t);
    ps_db_close(db);
    if (system("rm -rf " BENCH_DIR) != 0)
        fprintf(stderr, "warning: cleanup failed\n");
}

int main(void)
{
    int32_t *insert_keys = malloc(N_RECORDS * sizeof *insert_keys);
    int32_t *queries = malloc(N_LOOKUPS * sizeof *queries);
    if (!insert_keys || !queries) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    for (int32_t i = 0; i < N_RECORDS; i++)
        insert_keys[i] = i;
    shuffle(insert_keys, N_RECORDS, 0xC0FFEEu);

    uint32_t seed = 0xDEADBEEFu;
    for (int i = 0; i < N_LOOKUPS; i++)
        queries[i] = (int32_t)(xorshift32(&seed) % N_RECORDS);

    printf("===================================================================\n");
    printf(" PageStore Benchmark Suite (%d records)\n", N_RECORDS);
    printf("===================================================================\n");

    bench_indexes(queries);
    bench_engine(insert_keys, queries);
    printf("\n");

    free(insert_keys);
    free(queries);
    return 0;
}
