#include "secindex.h"

#include <stdlib.h>
#include <string.h>

#define SEC_KEY_MAX 64

typedef struct sec_node {
    char             sec_key[SEC_KEY_MAX];
    int32_t         *pks;
    size_t           count;
    size_t           cap;
    struct sec_node *next;
} sec_node;

struct ps_secindex {
    sec_node **buckets;
    size_t     nbuckets;
};

static size_t hash_str(const char *s, size_t nbuckets)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return (size_t)h & (nbuckets - 1);
}

ps_secindex_t *ps_secindex_create(size_t initial_buckets)
{
    size_t nb = 16;
    while (nb < initial_buckets)
        nb <<= 1;

    ps_secindex_t *si = malloc(sizeof *si);
    if (!si)
        return NULL;
    si->nbuckets = nb;
    si->buckets = calloc(nb, sizeof *si->buckets);
    if (!si->buckets) {
        free(si);
        return NULL;
    }
    return si;
}

void ps_secindex_destroy(ps_secindex_t *si)
{
    if (!si)
        return;
    for (size_t i = 0; i < si->nbuckets; i++) {
        sec_node *n = si->buckets[i];
        while (n) {
            sec_node *next = n->next;
            free(n->pks);
            free(n);
            n = next;
        }
    }
    free(si->buckets);
    free(si);
}

int ps_secindex_put(ps_secindex_t *si, const char *sec_key, int32_t primary_key)
{
    if (!si || !sec_key)
        return -1;
    size_t b = hash_str(sec_key, si->nbuckets);
    sec_node *n = si->buckets[b];
    while (n) {
        if (strncmp(n->sec_key, sec_key, SEC_KEY_MAX - 1) == 0)
            break;
        n = n->next;
    }
    if (!n) {
        n = calloc(1, sizeof *n);
        if (!n)
            return -1;
        strncpy(n->sec_key, sec_key, SEC_KEY_MAX - 1);
        n->sec_key[SEC_KEY_MAX - 1] = '\0';
        n->next = si->buckets[b];
        si->buckets[b] = n;
    }
    for (size_t i = 0; i < n->count; i++) {
        if (n->pks[i] == primary_key)
            return 0; /* already indexed */
    }
    if (n->count == n->cap) {
        size_t ncap = n->cap ? n->cap * 2 : 8;
        int32_t *np = realloc(n->pks, ncap * sizeof *np);
        if (!np)
            return -1;
        n->pks = np;
        n->cap = ncap;
    }
    n->pks[n->count++] = primary_key;
    return 0;
}

int ps_secindex_remove(ps_secindex_t *si, const char *sec_key, int32_t primary_key)
{
    if (!si || !sec_key)
        return 0;
    size_t b = hash_str(sec_key, si->nbuckets);
    for (sec_node *n = si->buckets[b]; n; n = n->next) {
        if (strncmp(n->sec_key, sec_key, SEC_KEY_MAX - 1) == 0) {
            for (size_t i = 0; i < n->count; i++) {
                if (n->pks[i] == primary_key) {
                    n->pks[i] = n->pks[--n->count];
                    return 1;
                }
            }
            return 0;
        }
    }
    return 0;
}

size_t ps_secindex_lookup(const ps_secindex_t *si, const char *sec_key,
                          int32_t *pks_out, size_t max_out)
{
    if (!si || !sec_key)
        return 0;
    size_t b = hash_str(sec_key, si->nbuckets);
    for (sec_node *n = si->buckets[b]; n; n = n->next) {
        if (strncmp(n->sec_key, sec_key, SEC_KEY_MAX - 1) == 0) {
            if (pks_out && max_out > 0) {
                size_t to_copy = n->count < max_out ? n->count : max_out;
                memcpy(pks_out, n->pks, to_copy * sizeof *pks_out);
            }
            return n->count;
        }
    }
    return 0;
}
