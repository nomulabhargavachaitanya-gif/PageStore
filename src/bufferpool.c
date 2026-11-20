#define _POSIX_C_SOURCE 200809L
#include "bufferpool.h"
#include "crc32.h"
#include "hashtable.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NO_PAGE  UINT32_MAX
#define NIL      (-1)

typedef struct {
    uint32_t page_no;   /* NO_PAGE when the frame is empty */
    int      dirty;
    int      prev;      /* LRU links: head = most recent, tail = victim */
    int      next;
} frame_meta;

struct bufferpool {
    int             fd;
    int             checksummed;
    int             last_err;
    size_t          nframes;
    uint8_t        *data;        /* nframes * BP_PAGE_SIZE */
    frame_meta     *meta;
    ht_t           *page_table;  /* page_no -> frame index */
    int             head;
    int             tail;
    size_t          used;
    bp_stats_t      stats;
    pthread_mutex_t lock;
};

static void lru_unlink(bufferpool_t *bp, int f)
{
    frame_meta *m = &bp->meta[f];
    if (m->prev != NIL) bp->meta[m->prev].next = m->next; else bp->head = m->next;
    if (m->next != NIL) bp->meta[m->next].prev = m->prev; else bp->tail = m->prev;
    m->prev = m->next = NIL;
}

static void lru_push_front(bufferpool_t *bp, int f)
{
    frame_meta *m = &bp->meta[f];
    m->prev = NIL;
    m->next = bp->head;
    if (bp->head != NIL)
        bp->meta[bp->head].prev = f;
    bp->head = f;
    if (bp->tail == NIL)
        bp->tail = f;
}

static void lru_push_back(bufferpool_t *bp, int f)
{
    frame_meta *m = &bp->meta[f];
    m->next = NIL;
    m->prev = bp->tail;
    if (bp->tail != NIL)
        bp->meta[bp->tail].next = f;
    bp->tail = f;
    if (bp->head == NIL)
        bp->head = f;
}

static int pwrite_full(int fd, const uint8_t *buf, size_t len, off_t off)
{
    while (len > 0) {
        ssize_t n = pwrite(fd, buf, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

/* Reads a page; a completely missing page (past EOF) is zero-filled as a fresh
 * page. Partial pages or checksum mismatches return BP_ERR_CORRUPT. */
static int pread_page(bufferpool_t *bp, uint8_t *buf, uint32_t page_no)
{
    off_t off = (off_t)page_no * BP_PAGE_SIZE;
    size_t got = 0;
    while (got < BP_PAGE_SIZE) {
        ssize_t n = pread(bp->fd, buf + got, BP_PAGE_SIZE - got, off + (off_t)got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return BP_ERR_IO;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    if (got == 0) {
        memset(buf, 0, BP_PAGE_SIZE);
        return BP_OK;
    }
    if (got < BP_PAGE_SIZE)
        return BP_ERR_CORRUPT;
    if (bp->checksummed && page_no >= 1) {
        uint32_t stored_pg, stored_crc;
        memcpy(&stored_pg, buf, 4);
        memcpy(&stored_crc, buf + 4, 4);
        uint32_t actual = crc32_compute(buf + BP_PAGE_HDR, BP_PAGE_SIZE - BP_PAGE_HDR);
        if (stored_pg != page_no || stored_crc != actual)
            return BP_ERR_CORRUPT;
    }
    return BP_OK;
}

static int write_frame(bufferpool_t *bp, int f)
{
    frame_meta *m = &bp->meta[f];
    if (!m->dirty || m->page_no == NO_PAGE)
        return BP_OK;
    uint8_t *buf = bp->data + (size_t)f * BP_PAGE_SIZE;
    if (bp->checksummed && m->page_no >= 1) {
        uint32_t crc = crc32_compute(buf + BP_PAGE_HDR, BP_PAGE_SIZE - BP_PAGE_HDR);
        memcpy(buf, &m->page_no, 4);
        memcpy(buf + 4, &crc, 4);
    }
    if (pwrite_full(bp->fd, buf, BP_PAGE_SIZE, (off_t)m->page_no * BP_PAGE_SIZE) != 0)
        return BP_ERR_IO;
    m->dirty = 0;
    bp->stats.writebacks++;
    return BP_OK;
}

bufferpool_t *bp_create(int fd, size_t nframes, int checksummed)
{
    if (nframes < 2)
        nframes = 2;
    bufferpool_t *bp = calloc(1, sizeof *bp);
    if (!bp)
        return NULL;
    pthread_mutex_init(&bp->lock, NULL);
    bp->fd = fd;
    bp->checksummed = checksummed ? 1 : 0;
    bp->last_err = BP_OK;
    bp->nframes = nframes;
    bp->data = malloc(nframes * BP_PAGE_SIZE);
    bp->meta = malloc(nframes * sizeof *bp->meta);
    bp->page_table = ht_create(nframes * 2);
    if (!bp->data || !bp->meta || !bp->page_table) {
        bp_destroy(bp);
        return NULL;
    }
    for (size_t i = 0; i < nframes; i++) {
        bp->meta[i].page_no = NO_PAGE;
        bp->meta[i].dirty = 0;
        bp->meta[i].prev = bp->meta[i].next = NIL;
    }
    bp->head = bp->tail = NIL;
    bp->stats.frames = nframes;
    return bp;
}

void bp_destroy(bufferpool_t *bp)
{
    if (!bp)
        return;
    pthread_mutex_destroy(&bp->lock);
    free(bp->data);
    free(bp->meta);
    ht_free(bp->page_table);
    free(bp);
}

uint8_t *bp_fetch(bufferpool_t *bp, uint32_t page_no, int for_write)
{
    int64_t found;
    int f;

    pthread_mutex_lock(&bp->lock);
    bp->last_err = BP_OK;
    if (ht_get(bp->page_table, (int32_t)page_no, &found)) {
        f = (int)found;
        bp->stats.hits++;
        lru_unlink(bp, f);
        lru_push_front(bp, f);
    } else {
        bp->stats.misses++;
        if (bp->used < bp->nframes) {
            f = (int)bp->used++;
        } else {
            f = bp->tail;                         /* least recently used */
            if (write_frame(bp, f) != BP_OK) {
                bp->last_err = BP_ERR_IO;
                pthread_mutex_unlock(&bp->lock);
                return NULL;
            }
            if (bp->meta[f].page_no != NO_PAGE) {
                ht_remove(bp->page_table, (int32_t)bp->meta[f].page_no);
                bp->stats.evictions++;
            }
            lru_unlink(bp, f);
            bp->meta[f].page_no = NO_PAGE;
            bp->meta[f].dirty = 0;
        }
        uint8_t *buf = bp->data + (size_t)f * BP_PAGE_SIZE;
        int rc = pread_page(bp, buf, page_no);
        if (rc != BP_OK) {
            bp->meta[f].page_no = NO_PAGE;
            bp->meta[f].dirty = 0;
            lru_push_back(bp, f);   /* keep empty frame at victim end, no leak */
            bp->last_err = rc;
            pthread_mutex_unlock(&bp->lock);
            return NULL;
        }
        if (ht_put(bp->page_table, (int32_t)page_no, f) != 0) {
            bp->meta[f].page_no = NO_PAGE;
            bp->meta[f].dirty = 0;
            lru_push_back(bp, f);   /* return frame to LRU tail on OOM */
            bp->last_err = BP_ERR_NOMEM;
            pthread_mutex_unlock(&bp->lock);
            return NULL;
        }
        bp->meta[f].page_no = page_no;
        bp->meta[f].dirty = 0;
        lru_push_front(bp, f);
    }
    if (for_write)
        bp->meta[f].dirty = 1;
    uint8_t *ret = bp->data + (size_t)f * BP_PAGE_SIZE;
    pthread_mutex_unlock(&bp->lock);
    return ret;
}

int bp_last_error(const bufferpool_t *bp) { return bp ? bp->last_err : BP_ERR_IO; }

int bp_flush_all(bufferpool_t *bp)
{
    pthread_mutex_lock(&bp->lock);
    for (size_t i = 0; i < bp->used; i++)
        if (write_frame(bp, (int)i) != BP_OK) {
            bp->last_err = BP_ERR_IO;
            pthread_mutex_unlock(&bp->lock);
            return -1;
        }
    pthread_mutex_unlock(&bp->lock);
    return 0;
}

void bp_discard_all(bufferpool_t *bp)
{
    pthread_mutex_lock(&bp->lock);
    for (size_t i = 0; i < bp->nframes; i++) {
        bp->meta[i].page_no = NO_PAGE;
        bp->meta[i].dirty = 0;
        bp->meta[i].prev = bp->meta[i].next = NIL;
    }
    ht_clear(bp->page_table);
    bp->head = bp->tail = NIL;
    bp->used = 0;
    bp->last_err = BP_OK;
    pthread_mutex_unlock(&bp->lock);
}

void bp_set_fd(bufferpool_t *bp, int fd)
{
    pthread_mutex_lock(&bp->lock);
    bp->fd = fd;
    pthread_mutex_unlock(&bp->lock);
}

void bp_get_stats(const bufferpool_t *bp, bp_stats_t *st)
{
    bufferpool_t *m = (bufferpool_t *)bp;
    pthread_mutex_lock(&m->lock);
    *st = bp->stats;
    pthread_mutex_unlock(&m->lock);
}

void bp_reset_stats(bufferpool_t *bp)
{
    pthread_mutex_lock(&bp->lock);
    size_t frames = bp->stats.frames;
    memset(&bp->stats, 0, sizeof bp->stats);
    bp->stats.frames = frames;
    pthread_mutex_unlock(&bp->lock);
}
