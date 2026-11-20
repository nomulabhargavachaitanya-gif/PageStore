#ifndef PS_BUFFERPOOL_H
#define PS_BUFFERPOOL_H

#include <stddef.h>
#include <stdint.h>

#define BP_PAGE_SIZE    4096u
#define BP_PAGE_HDR     8u    /* uint32 page_no + uint32 crc32 (when checksummed) */

#define BP_OK           0
#define BP_ERR_IO      -1
#define BP_ERR_CORRUPT -2
#define BP_ERR_NOMEM   -3

/* Fixed-size pool of page frames over a file descriptor.
 * - LRU replacement (doubly linked list over frames)
 * - page table: page number -> frame (hash table)
 * - write-back: dirty frames are written on eviction or flush
 * - optional per-page IEEE CRC-32 verification for data pages (page_no >= 1)
 *
 * A pointer returned by bp_fetch() stays valid until the next bp_* call. */
typedef struct bufferpool bufferpool_t;

typedef struct {
    size_t   frames;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
} bp_stats_t;

bufferpool_t *bp_create(int fd, size_t nframes, int checksummed);
void          bp_destroy(bufferpool_t *bp);   /* does not flush */
uint8_t      *bp_fetch(bufferpool_t *bp, uint32_t page_no, int for_write);
int           bp_last_error(const bufferpool_t *bp);
int           bp_flush_all(bufferpool_t *bp); /* 0 ok, -1 io error */
void          bp_discard_all(bufferpool_t *bp);
void          bp_set_fd(bufferpool_t *bp, int fd);
void          bp_get_stats(const bufferpool_t *bp, bp_stats_t *st);
void          bp_reset_stats(bufferpool_t *bp);

#endif
