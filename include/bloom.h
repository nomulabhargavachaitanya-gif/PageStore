#ifndef PS_BLOOM_H
#define PS_BLOOM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Fast 64KB (524,288-bit) 4-probe Bloom filter for negative lookup filtering.
 * Uses Kirsch-Mitzenmacher double hashing (h1 + i * h2) to compute 4 bit
 * positions from a single 64-bit SplitMix/FNV-1a hash.
 */

#define PS_BLOOM_BITS  (524288u)            /* 2^19 bits */
#define PS_BLOOM_BYTES (PS_BLOOM_BITS / 8u) /* 64 KB */
#define PS_BLOOM_K     4u

typedef struct {
    uint8_t  bits[PS_BLOOM_BYTES];
    uint64_t rejections;
} ps_bloom_t;

void bloom_clear(ps_bloom_t *bf);
void bloom_add_i32(ps_bloom_t *bf, int32_t key);
int  bloom_may_contain_i32(const ps_bloom_t *bf, int32_t key);

void bloom_add_bytes(ps_bloom_t *bf, const void *key, size_t len);
int  bloom_may_contain_bytes(const ps_bloom_t *bf, const void *key, size_t len);

#endif /* PS_BLOOM_H */
