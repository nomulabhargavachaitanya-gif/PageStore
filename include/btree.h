#ifndef PS_BTREE_H
#define PS_BTREE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Disk-resident B+ Tree index backed by 4KB buffer pool pages.
 *
 * Page layout (4096 bytes):
 *   Page 0    : B+ Tree file header (magic, root_page, total_pages, count, height)
 *   Pages 1..N: 8-byte bufferpool checksum header + B+ Tree node (leaf or internal)
 *
 * High fanout (up to 240 keys per 4KB node) bounds tree height to <= 3 for
 * millions of keys. Leaf pages form a singly-linked list via `next_leaf` for
 * sequential range scans.
 */

#define PS_BTREE_MAX_KEYS 240u

typedef struct ps_btree ps_btree_t;

typedef int (*ps_btree_visit_fn)(int32_t key, uint32_t val, void *ctx);

int    ps_btree_open(const char *path, size_t pool_pages, ps_btree_t **out);
int    ps_btree_close(ps_btree_t *bt);
int    ps_btree_insert(ps_btree_t *bt, int32_t key, uint32_t val);
int    ps_btree_find(ps_btree_t *bt, int32_t key, uint32_t *val_out);
int    ps_btree_remove(ps_btree_t *bt, int32_t key);
size_t ps_btree_range(ps_btree_t *bt, int32_t lo, int32_t hi,
                      ps_btree_visit_fn fn, void *ctx);
size_t ps_btree_count(const ps_btree_t *bt);
int    ps_btree_height(const ps_btree_t *bt);

#endif /* PS_BTREE_H */
