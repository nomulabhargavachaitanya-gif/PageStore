#include "hashtable.h"

#include <stdlib.h>

typedef struct ht_entry {
    int32_t          key;
    int64_t          val;
    struct ht_entry *next;
} ht_entry;

struct ht {
    ht_entry **buckets;
    size_t     nbuckets;   /* always a power of two */
    size_t     size;
};

/* Fibonacci-style integer mix so sequential keys spread across buckets. */
static size_t hash_key(int32_t key, size_t nbuckets)
{
    uint32_t x = (uint32_t)key;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (size_t)x & (nbuckets - 1);
}

static size_t round_pow2(size_t n)
{
    size_t p = 16;
    while (p < n)
        p <<= 1;
    return p;
}

ht_t *ht_create(size_t initial_buckets)
{
    ht_t *h = malloc(sizeof *h);
    if (!h)
        return NULL;
    h->nbuckets = round_pow2(initial_buckets);
    h->size = 0;
    h->buckets = calloc(h->nbuckets, sizeof *h->buckets);
    if (!h->buckets) {
        free(h);
        return NULL;
    }
    return h;
}

void ht_clear(ht_t *h)
{
    if (!h)
        return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        ht_entry *e = h->buckets[i];
        while (e) {
            ht_entry *n = e->next;
            free(e);
            e = n;
        }
        h->buckets[i] = NULL;
    }
    h->size = 0;
}

void ht_free(ht_t *h)
{
    if (!h)
        return;
    ht_clear(h);
    free(h->buckets);
    free(h);
}

static int ht_grow(ht_t *h)
{
    size_t nb = h->nbuckets << 1;
    ht_entry **fresh = calloc(nb, sizeof *fresh);
    if (!fresh)
        return -1;
    for (size_t i = 0; i < h->nbuckets; i++) {
        ht_entry *e = h->buckets[i];
        while (e) {
            ht_entry *n = e->next;
            size_t b = hash_key(e->key, nb);
            e->next = fresh[b];
            fresh[b] = e;
            e = n;
        }
    }
    free(h->buckets);
    h->buckets = fresh;
    h->nbuckets = nb;
    return 0;
}

int ht_put(ht_t *h, int32_t key, int64_t val)
{
    size_t b = hash_key(key, h->nbuckets);
    for (ht_entry *e = h->buckets[b]; e; e = e->next) {
        if (e->key == key) {
            e->val = val;
            return 0;
        }
    }
    /* keep load factor <= 0.75 */
    if ((h->size + 1) * 4 > h->nbuckets * 3) {
        if (ht_grow(h) != 0)
            return -1;
        b = hash_key(key, h->nbuckets);
    }
    ht_entry *e = malloc(sizeof *e);
    if (!e)
        return -1;
    e->key = key;
    e->val = val;
    e->next = h->buckets[b];
    h->buckets[b] = e;
    h->size++;
    return 0;
}

int ht_get(const ht_t *h, int32_t key, int64_t *out)
{
    for (ht_entry *e = h->buckets[hash_key(key, h->nbuckets)]; e; e = e->next) {
        if (e->key == key) {
            if (out)
                *out = e->val;
            return 1;
        }
    }
    return 0;
}

int ht_remove(ht_t *h, int32_t key)
{
    ht_entry **pp = &h->buckets[hash_key(key, h->nbuckets)];
    while (*pp) {
        if ((*pp)->key == key) {
            ht_entry *dead = *pp;
            *pp = dead->next;
            free(dead);
            h->size--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

size_t ht_size(const ht_t *h) { return h->size; }
size_t ht_buckets(const ht_t *h) { return h->nbuckets; }
