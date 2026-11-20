#ifndef PS_AVL_H
#define PS_AVL_H

#include <stddef.h>
#include <stdint.h>

/* Self-balancing AVL tree: int32 key -> uint32 value.
 * Height stays within ~1.44 log2(n), even for sorted insert order. */
typedef struct avl_node avl_node;

typedef struct {
    avl_node *root;
    size_t    count;
} avl_tree;

typedef struct {
    int32_t  key;
    uint32_t val;
} avl_kv;

/* Return non-zero from the visitor to stop the traversal. */
typedef int (*avl_visit_fn)(int32_t key, uint32_t val, void *ctx);

void   avl_init(avl_tree *t);
void   avl_free(avl_tree *t);
int    avl_insert(avl_tree *t, int32_t key, uint32_t val);  /* 0 ok, 1 duplicate, -1 oom */
int    avl_build_sorted(avl_tree *t, const avl_kv *items, size_t n); /* O(n) bulk load */
int    avl_find(const avl_tree *t, int32_t key, uint32_t *out); /* 1 found */
int    avl_set(avl_tree *t, int32_t key, uint32_t val);     /* 1 updated, 0 missing */
int    avl_remove(avl_tree *t, int32_t key);                /* 1 removed, 0 missing */
size_t avl_range(const avl_tree *t, int32_t lo, int32_t hi, avl_visit_fn fn, void *ctx);
int    avl_height(const avl_tree *t);
int    avl_validate(const avl_tree *t);  /* 1 if ordering, heights and balance hold */

#endif
