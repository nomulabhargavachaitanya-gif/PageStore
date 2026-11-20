#define _POSIX_C_SOURCE 200809L
#include "btree.h"
#include "bufferpool.h"
#include "crc32.h"
#include "pagestore.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BTREE_MAGIC "PSBTREE1"

typedef struct {
    char     magic[8];
    uint32_t root_page;
    uint32_t total_pages;
    uint32_t count;
    uint32_t height;
    uint32_t crc;
} btree_hdr;

/* Fits inside 4096 - 8 = 4088 bytes:
 * 16B header + 240 * 4B keys (960B) + 241 * 4B children/vals (964B) = 1940B <= 4088B */
typedef struct {
    uint32_t is_leaf;
    uint32_t nkeys;
    uint32_t next_leaf; /* leaf pages only: page_no of right sibling (0 = none) */
    uint32_t reserved;
    int32_t  keys[PS_BTREE_MAX_KEYS];
    uint32_t ptrs[PS_BTREE_MAX_KEYS + 1]; /* leaf: ptrs[0..nkeys-1] = values;
                                           * internal: ptrs[0..nkeys] = child page_nos */
} btree_node;

struct ps_btree {
    int           fd;
    btree_hdr     hdr;
    bufferpool_t *pool;
};

static uint32_t hdr_crc(const btree_hdr *h)
{
    return crc32_compute(h, offsetof(btree_hdr, crc));
}

static int save_hdr(ps_btree_t *bt)
{
    uint8_t page0[BP_PAGE_SIZE];
    memset(page0, 0, sizeof page0);
    bt->hdr.crc = hdr_crc(&bt->hdr);
    memcpy(page0, &bt->hdr, sizeof bt->hdr);
    return pwrite(bt->fd, page0, sizeof page0, 0) == (ssize_t)sizeof page0 ? PS_OK : PS_ERR_IO;
}

static btree_node *fetch_node(ps_btree_t *bt, uint32_t page_no, int for_write)
{
    uint8_t *raw = bp_fetch(bt->pool, page_no, for_write);
    if (!raw)
        return NULL;
    return (btree_node *)(raw + BP_PAGE_HDR);
}

static uint32_t alloc_page(ps_btree_t *bt, int is_leaf)
{
    uint32_t pno = ++bt->hdr.total_pages;
    btree_node *n = fetch_node(bt, pno, 1);
    if (!n)
        return 0;
    memset(n, 0, sizeof *n);
    n->is_leaf = is_leaf ? 1u : 0u;
    return pno;
}

static uint32_t lower_bound_keys(const int32_t *keys, uint32_t nkeys, int32_t key)
{
    uint32_t lo = 0, hi = nkeys;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int ps_btree_open(const char *path, size_t pool_pages, ps_btree_t **out)
{
    if (!path || !out)
        return PS_ERR_INVALID;

    ps_btree_t *bt = calloc(1, sizeof *bt);
    if (!bt)
        return PS_ERR_NOMEM;

    bt->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (bt->fd < 0) {
        free(bt);
        return PS_ERR_IO;
    }

    bt->pool = bp_create(bt->fd, pool_pages ? pool_pages : 64, 1);
    if (!bt->pool) {
        close(bt->fd);
        free(bt);
        return PS_ERR_NOMEM;
    }

    off_t sz = lseek(bt->fd, 0, SEEK_END);
    if (sz == 0) {
        memcpy(bt->hdr.magic, BTREE_MAGIC, 8);
        bt->hdr.total_pages = 0;
        bt->hdr.count = 0;
        bt->hdr.height = 1;
        bt->hdr.root_page = alloc_page(bt, 1);
        if (!bt->hdr.root_page || bp_flush_all(bt->pool) != 0 || save_hdr(bt) != PS_OK) {
            ps_btree_close(bt);
            return PS_ERR_IO;
        }
    } else {
        if (pread(bt->fd, &bt->hdr, sizeof bt->hdr, 0) != (ssize_t)sizeof bt->hdr ||
            memcmp(bt->hdr.magic, BTREE_MAGIC, 8) != 0 ||
            bt->hdr.crc != hdr_crc(&bt->hdr) ||
            bt->hdr.root_page == 0) {
            bp_destroy(bt->pool);
            close(bt->fd);
            free(bt);
            return PS_ERR_CORRUPT;
        }
    }

    *out = bt;
    return PS_OK;
}

int ps_btree_close(ps_btree_t *bt)
{
    if (!bt)
        return PS_ERR_INVALID;
    int rc = PS_OK;
    if (bt->pool) {
        if (bp_flush_all(bt->pool) != 0)
            rc = PS_ERR_IO;
        bp_destroy(bt->pool);
    }
    if (bt->fd >= 0) {
        if (save_hdr(bt) != PS_OK || fsync(bt->fd) != 0)
            rc = PS_ERR_IO;
        close(bt->fd);
    }
    free(bt);
    return rc;
}

int ps_btree_find(ps_btree_t *bt, int32_t key, uint32_t *val_out)
{
    if (!bt)
        return 0;
    uint32_t pno = bt->hdr.root_page;
    for (;;) {
        btree_node *n = fetch_node(bt, pno, 0);
        if (!n)
            return 0;
        if (n->is_leaf) {
            uint32_t pos = lower_bound_keys(n->keys, n->nkeys, key);
            if (pos < n->nkeys && n->keys[pos] == key) {
                if (val_out)
                    *val_out = n->ptrs[pos];
                return 1;
            }
            return 0;
        }
        /* Internal node: follow child pointer i where key < keys[i] */
        uint32_t idx = 0;
        while (idx < n->nkeys && key >= n->keys[idx])
            idx++;
        pno = n->ptrs[idx];
    }
}

/* Recursive insert helper that returns 1 if child split (setting *split_key and *new_child). */
static int insert_rec(ps_btree_t *bt, uint32_t pno, int32_t key, uint32_t val,
                      int32_t *split_key, uint32_t *new_child, int *status)
{
    btree_node *raw = fetch_node(bt, pno, 0);
    if (!raw) {
        *status = PS_ERR_IO;
        return 0;
    }
    btree_node snap = *raw; /* local copy safe across buffer pool evictions */

    if (snap.is_leaf) {
        uint32_t pos = lower_bound_keys(snap.keys, snap.nkeys, key);
        if (pos < snap.nkeys && snap.keys[pos] == key) {
            *status = PS_ERR_DUPLICATE;
            return 0;
        }
        for (uint32_t i = snap.nkeys; i > pos; i--) {
            snap.keys[i] = snap.keys[i - 1];
            snap.ptrs[i] = snap.ptrs[i - 1];
        }
        snap.keys[pos] = key;
        snap.ptrs[pos] = val;
        snap.nkeys++;

        if (snap.nkeys < PS_BTREE_MAX_KEYS) {
            btree_node *dst = fetch_node(bt, pno, 1);
            if (!dst) { *status = PS_ERR_IO; return 0; }
            *dst = snap;
            return 0;
        }

        /* Split full leaf in half */
        uint32_t right_pno = alloc_page(bt, 1);
        if (!right_pno) { *status = PS_ERR_IO; return 0; }

        uint32_t mid = snap.nkeys / 2;
        btree_node right_node;
        memset(&right_node, 0, sizeof right_node);
        right_node.is_leaf = 1;
        right_node.nkeys = snap.nkeys - mid;
        right_node.next_leaf = snap.next_leaf;
        for (uint32_t i = 0; i < right_node.nkeys; i++) {
            right_node.keys[i] = snap.keys[mid + i];
            right_node.ptrs[i] = snap.ptrs[mid + i];
        }

        snap.nkeys = mid;
        snap.next_leaf = right_pno;

        btree_node *dst_r = fetch_node(bt, right_pno, 1);
        if (!dst_r) { *status = PS_ERR_IO; return 0; }
        *dst_r = right_node;

        btree_node *dst_l = fetch_node(bt, pno, 1);
        if (!dst_l) { *status = PS_ERR_IO; return 0; }
        *dst_l = snap;

        *split_key = right_node.keys[0];
        *new_child = right_pno;
        return 1;
    }

    /* Internal node */
    uint32_t idx = 0;
    while (idx < snap.nkeys && key >= snap.keys[idx])
        idx++;

    int32_t child_split_k = 0;
    uint32_t child_new_pno = 0;
    int did_split = insert_rec(bt, snap.ptrs[idx], key, val,
                               &child_split_k, &child_new_pno, status);
    if (*status != PS_OK || !did_split)
        return 0;

    for (uint32_t i = snap.nkeys; i > idx; i--) {
        snap.keys[i]     = snap.keys[i - 1];
        snap.ptrs[i + 1] = snap.ptrs[i];
    }
    snap.keys[idx]     = child_split_k;
    snap.ptrs[idx + 1] = child_new_pno;
    snap.nkeys++;

    if (snap.nkeys < PS_BTREE_MAX_KEYS) {
        btree_node *dst = fetch_node(bt, pno, 1);
        if (!dst) { *status = PS_ERR_IO; return 0; }
        *dst = snap;
        return 0;
    }

    /* Split full internal node: promote middle key */
    uint32_t right_pno = alloc_page(bt, 0);
    if (!right_pno) { *status = PS_ERR_IO; return 0; }

    uint32_t mid = snap.nkeys / 2;
    int32_t promote_key = snap.keys[mid];

    btree_node right_node;
    memset(&right_node, 0, sizeof right_node);
    right_node.is_leaf = 0;
    right_node.nkeys = snap.nkeys - mid - 1;
    for (uint32_t i = 0; i < right_node.nkeys; i++) {
        right_node.keys[i] = snap.keys[mid + 1 + i];
        right_node.ptrs[i] = snap.ptrs[mid + 1 + i];
    }
    right_node.ptrs[right_node.nkeys] = snap.ptrs[snap.nkeys];

    snap.nkeys = mid;

    btree_node *dst_r = fetch_node(bt, right_pno, 1);
    if (!dst_r) { *status = PS_ERR_IO; return 0; }
    *dst_r = right_node;

    btree_node *dst_l = fetch_node(bt, pno, 1);
    if (!dst_l) { *status = PS_ERR_IO; return 0; }
    *dst_l = snap;

    *split_key = promote_key;
    *new_child = right_pno;
    return 1;
}

int ps_btree_insert(ps_btree_t *bt, int32_t key, uint32_t val)
{
    if (!bt)
        return PS_ERR_INVALID;
    int status = PS_OK;
    int32_t split_key = 0;
    uint32_t new_child = 0;
    int split = insert_rec(bt, bt->hdr.root_page, key, val,
                           &split_key, &new_child, &status);
    if (status != PS_OK)
        return status;

    if (split) {
        uint32_t new_root = alloc_page(bt, 0);
        if (!new_root)
            return PS_ERR_IO;
        btree_node *rn = fetch_node(bt, new_root, 1);
        if (!rn)
            return PS_ERR_IO;
        rn->is_leaf = 0;
        rn->nkeys   = 1;
        rn->keys[0] = split_key;
        rn->ptrs[0] = bt->hdr.root_page;
        rn->ptrs[1] = new_child;
        bt->hdr.root_page = new_root;
        bt->hdr.height++;
    }
    bt->hdr.count++;
    return PS_OK;
}

int ps_btree_remove(ps_btree_t *bt, int32_t key)
{
    if (!bt)
        return 0;
    uint32_t pno = bt->hdr.root_page;
    for (;;) {
        btree_node *n = fetch_node(bt, pno, 0);
        if (!n)
            return 0;
        if (n->is_leaf) {
            uint32_t pos = lower_bound_keys(n->keys, n->nkeys, key);
            if (pos >= n->nkeys || n->keys[pos] != key)
                return 0;
            btree_node *w = fetch_node(bt, pno, 1);
            if (!w)
                return 0;
            for (uint32_t i = pos; i + 1 < w->nkeys; i++) {
                w->keys[i] = w->keys[i + 1];
                w->ptrs[i] = w->ptrs[i + 1];
            }
            w->nkeys--;
            bt->hdr.count--;
            return 1;
        }
        uint32_t idx = 0;
        while (idx < n->nkeys && key >= n->keys[idx])
            idx++;
        pno = n->ptrs[idx];
    }
}

size_t ps_btree_range(ps_btree_t *bt, int32_t lo, int32_t hi,
                      ps_btree_visit_fn fn, void *ctx)
{
    if (!bt || lo > hi)
        return 0;

    /* Descend to the leaf containing `lo` */
    uint32_t pno = bt->hdr.root_page;
    for (;;) {
        btree_node *n = fetch_node(bt, pno, 0);
        if (!n)
            return 0;
        if (n->is_leaf)
            break;
        uint32_t idx = 0;
        while (idx < n->nkeys && lo >= n->keys[idx])
            idx++;
        pno = n->ptrs[idx];
    }

    size_t visited = 0;
    while (pno != 0) {
        btree_node *leaf = fetch_node(bt, pno, 0);
        if (!leaf)
            break;
        btree_node snap = *leaf;
        for (uint32_t i = 0; i < snap.nkeys; i++) {
            int32_t k = snap.keys[i];
            if (k < lo)
                continue;
            if (k > hi)
                return visited;
            visited++;
            if (fn && fn(k, snap.ptrs[i], ctx))
                return visited;
        }
        pno = snap.next_leaf;
    }
    return visited;
}

size_t ps_btree_count(const ps_btree_t *bt)
{
    return bt ? bt->hdr.count : 0;
}

int ps_btree_height(const ps_btree_t *bt)
{
    return bt ? (int)bt->hdr.height : 0;
}
