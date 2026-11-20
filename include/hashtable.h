#ifndef PS_HASHTABLE_H
#define PS_HASHTABLE_H

#include <stddef.h>
#include <stdint.h>

/* Separate-chaining hash table: int32 key -> int64 value.
 * Power-of-two bucket count, doubles when load factor exceeds 0.75. */
typedef struct ht ht_t;

ht_t  *ht_create(size_t initial_buckets);
void   ht_free(ht_t *h);
void   ht_clear(ht_t *h);
int    ht_put(ht_t *h, int32_t key, int64_t val);          /* 0 ok, -1 out of memory */
int    ht_get(const ht_t *h, int32_t key, int64_t *out);   /* 1 found, 0 missing */
int    ht_remove(ht_t *h, int32_t key);                    /* 1 removed, 0 missing */
size_t ht_size(const ht_t *h);
size_t ht_buckets(const ht_t *h);

#endif
