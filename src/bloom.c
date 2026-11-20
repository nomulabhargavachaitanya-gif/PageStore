#include "bloom.h"
#include <string.h>

static uint64_t splitmix64(uint64_t x)
{
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static uint64_t fnv1a_bytes(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return splitmix64(h);
}

void bloom_clear(ps_bloom_t *bf)
{
    if (!bf)
        return;
    memset(bf, 0, sizeof *bf);
}

static void add_hash(ps_bloom_t *bf, uint64_t h)
{
    uint32_t h1 = (uint32_t)h;
    uint32_t h2 = (uint32_t)(h >> 32) | 1u;
    for (uint32_t i = 0; i < PS_BLOOM_K; i++) {
        uint32_t bit = (h1 + i * h2) & (PS_BLOOM_BITS - 1u);
        bf->bits[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    }
}

static int query_hash(const ps_bloom_t *bf, uint64_t h)
{
    uint32_t h1 = (uint32_t)h;
    uint32_t h2 = (uint32_t)(h >> 32) | 1u;
    for (uint32_t i = 0; i < PS_BLOOM_K; i++) {
        uint32_t bit = (h1 + i * h2) & (PS_BLOOM_BITS - 1u);
        if (!(bf->bits[bit >> 3] & (uint8_t)(1u << (bit & 7u))))
            return 0;
    }
    return 1;
}

void bloom_add_i32(ps_bloom_t *bf, int32_t key)
{
    add_hash(bf, splitmix64((uint64_t)(uint32_t)key));
}

int bloom_may_contain_i32(const ps_bloom_t *bf, int32_t key)
{
    return query_hash(bf, splitmix64((uint64_t)(uint32_t)key));
}

void bloom_add_bytes(ps_bloom_t *bf, const void *key, size_t len)
{
    add_hash(bf, fnv1a_bytes(key, len));
}

int bloom_may_contain_bytes(const ps_bloom_t *bf, const void *key, size_t len)
{
    return query_hash(bf, fnv1a_bytes(key, len));
}
