#define _POSIX_C_SOURCE 200809L
#include "varrec.h"
#include "bloom.h"
#include "bufferpool.h"
#include "crc32.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VAR_MAGIC       "PSVAR001"
#define VAR_INLINE_MAX  3000u
#define VAR_OVF_CHUNK   (BP_PAGE_SIZE - BP_PAGE_HDR - 8u)

#define VSLOT_LIVE      1u
#define VSLOT_DEAD      2u
#define VSLOT_OVERFLOW  4u

typedef struct {
    char     magic[8];
    uint32_t total_pages;
    uint32_t active_data_page;
    uint32_t live_records;
    uint32_t crc;
} var_file_hdr;

typedef struct {
    uint16_t num_slots;
    uint16_t free_upper; /* starts at BP_PAGE_SIZE (4096) and grows downward */
    uint32_t page_type;  /* 1 = slotted data page, 2 = overflow page */
} var_page_hdr;

typedef struct {
    uint16_t offset;
    uint16_t key_len;
    uint16_t rec_len;
    uint16_t flags;
} var_slot;

typedef struct {
    uint32_t total_val_len;
    uint32_t first_ovf_page;
} ovf_ref;

struct ps_vartable {
    int           fd;
    var_file_hdr  hdr;
    bufferpool_t *pool;
    ps_bloom_t    bloom;
};

PSKey ps_key_str(const char *s)
{
    PSKey k;
    k.data = s;
    k.len  = s ? (uint16_t)strlen(s) : 0;
    return k;
}

PSKey ps_key_bytes(const void *data, uint16_t len)
{
    PSKey k;
    k.data = data;
    k.len  = len;
    return k;
}

static uint32_t var_hdr_crc(const var_file_hdr *h)
{
    return crc32_compute(h, offsetof(var_file_hdr, crc));
}

static int save_var_hdr(ps_vartable_t *vt)
{
    uint8_t page0[BP_PAGE_SIZE];
    memset(page0, 0, sizeof page0);
    vt->hdr.crc = var_hdr_crc(&vt->hdr);
    memcpy(page0, &vt->hdr, sizeof vt->hdr);
    return pwrite(vt->fd, page0, sizeof page0, 0) == (ssize_t)sizeof page0 ? PS_OK : PS_ERR_IO;
}

static uint32_t alloc_slotted_page(ps_vartable_t *vt)
{
    uint32_t pno = ++vt->hdr.total_pages;
    uint8_t *pg = bp_fetch(vt->pool, pno, 1);
    if (!pg)
        return 0;
    memset(pg, 0, BP_PAGE_SIZE);
    var_page_hdr ph;
    ph.num_slots  = 0;
    ph.free_upper = BP_PAGE_SIZE;
    ph.page_type  = 1;
    memcpy(pg + BP_PAGE_HDR, &ph, sizeof ph);
    vt->hdr.active_data_page = pno;
    return pno;
}

static uint32_t write_overflow_chain(ps_vartable_t *vt, const void *val, uint32_t val_len)
{
    const uint8_t *p = val;
    uint32_t remaining = val_len;
    uint32_t first_page = 0, prev_page = 0;

    while (remaining > 0) {
        uint32_t chunk = remaining > VAR_OVF_CHUNK ? VAR_OVF_CHUNK : remaining;
        uint32_t pno = ++vt->hdr.total_pages;
        uint8_t *pg = bp_fetch(vt->pool, pno, 1);
        if (!pg)
            return 0;
        memset(pg, 0, BP_PAGE_SIZE);
        uint32_t next_pno = 0;
        memcpy(pg + BP_PAGE_HDR, &next_pno, 4);
        memcpy(pg + BP_PAGE_HDR + 4, &chunk, 4);
        memcpy(pg + BP_PAGE_HDR + 8, p, chunk);

        if (prev_page != 0) {
            uint8_t *prev_pg = bp_fetch(vt->pool, prev_page, 1);
            if (!prev_pg)
                return 0;
            memcpy(prev_pg + BP_PAGE_HDR, &pno, 4);
        } else {
            first_page = pno;
        }
        prev_page = pno;
        p += chunk;
        remaining -= chunk;
    }
    return first_page;
}

static int read_overflow_chain(ps_vartable_t *vt, uint32_t first_page,
                               uint32_t total_len, void *out, uint32_t cap)
{
    uint8_t *dst = out;
    uint32_t copied = 0;
    uint32_t pno = first_page;

    while (pno != 0 && copied < total_len) {
        uint8_t *pg = bp_fetch(vt->pool, pno, 0);
        if (!pg)
            return PS_ERR_IO;
        uint32_t next_pno = 0, chunk = 0;
        memcpy(&next_pno, pg + BP_PAGE_HDR, 4);
        memcpy(&chunk, pg + BP_PAGE_HDR + 4, 4);
        if (chunk > VAR_OVF_CHUNK)
            return PS_ERR_CORRUPT;
        if (dst && copied < cap) {
            uint32_t to_copy = (copied + chunk <= cap) ? chunk : (cap - copied);
            memcpy(dst + copied, pg + BP_PAGE_HDR + 8, to_copy);
        }
        copied += chunk;
        pno = next_pno;
    }
    return copied == total_len ? PS_OK : PS_ERR_CORRUPT;
}

int ps_var_open(const char *path, size_t pool_pages, ps_vartable_t **out)
{
    if (!path || !out)
        return PS_ERR_INVALID;

    ps_vartable_t *vt = calloc(1, sizeof *vt);
    if (!vt)
        return PS_ERR_NOMEM;
    bloom_clear(&vt->bloom);

    vt->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (vt->fd < 0) {
        free(vt);
        return PS_ERR_IO;
    }

    vt->pool = bp_create(vt->fd, pool_pages ? pool_pages : 64, 1);
    if (!vt->pool) {
        close(vt->fd);
        free(vt);
        return PS_ERR_NOMEM;
    }

    off_t sz = lseek(vt->fd, 0, SEEK_END);
    if (sz == 0) {
        memcpy(vt->hdr.magic, VAR_MAGIC, 8);
        vt->hdr.total_pages = 0;
        vt->hdr.live_records = 0;
        if (!alloc_slotted_page(vt) || bp_flush_all(vt->pool) != 0 || save_var_hdr(vt) != PS_OK) {
            ps_var_close(vt);
            return PS_ERR_IO;
        }
    } else {
        if (pread(vt->fd, &vt->hdr, sizeof vt->hdr, 0) != (ssize_t)sizeof vt->hdr ||
            memcmp(vt->hdr.magic, VAR_MAGIC, 8) != 0 ||
            vt->hdr.crc != var_hdr_crc(&vt->hdr)) {
            bp_destroy(vt->pool);
            close(vt->fd);
            free(vt);
            return PS_ERR_CORRUPT;
        }
        /* Populate bloom filter from existing slotted pages */
        for (uint32_t p = 1; p <= vt->hdr.total_pages; p++) {
            uint8_t *pg = bp_fetch(vt->pool, p, 0);
            if (!pg) continue;
            var_page_hdr ph;
            memcpy(&ph, pg + BP_PAGE_HDR, sizeof ph);
            if (ph.page_type != 1) continue;
            for (uint16_t s = 0; s < ph.num_slots; s++) {
                var_slot sl;
                memcpy(&sl, pg + BP_PAGE_HDR + sizeof ph + s * sizeof sl, sizeof sl);
                if (sl.flags & VSLOT_LIVE)
                    bloom_add_bytes(&vt->bloom, pg + sl.offset, sl.key_len);
            }
        }
    }

    *out = vt;
    return PS_OK;
}

int ps_var_close(ps_vartable_t *vt)
{
    if (!vt)
        return PS_ERR_INVALID;
    int rc = PS_OK;
    if (vt->pool) {
        if (bp_flush_all(vt->pool) != 0)
            rc = PS_ERR_IO;
        bp_destroy(vt->pool);
    }
    if (vt->fd >= 0) {
        if (save_var_hdr(vt) != PS_OK || fsync(vt->fd) != 0)
            rc = PS_ERR_IO;
        close(vt->fd);
    }
    free(vt);
    return rc;
}

/* Locate an existing live slot for `key`. If found, sets *page_out and *slot_idx_out. */
static int find_var_slot(ps_vartable_t *vt, PSKey key, uint32_t *page_out, uint16_t *slot_out)
{
    if (!bloom_may_contain_bytes(&vt->bloom, key.data, key.len))
        return 0;

    for (uint32_t p = 1; p <= vt->hdr.total_pages; p++) {
        uint8_t *pg = bp_fetch(vt->pool, p, 0);
        if (!pg)
            continue;
        var_page_hdr ph;
        memcpy(&ph, pg + BP_PAGE_HDR, sizeof ph);
        if (ph.page_type != 1)
            continue;
        for (uint16_t s = 0; s < ph.num_slots; s++) {
            var_slot sl;
            memcpy(&sl, pg + BP_PAGE_HDR + sizeof ph + s * sizeof sl, sizeof sl);
            if ((sl.flags & VSLOT_LIVE) && sl.key_len == key.len &&
                memcmp(pg + sl.offset, key.data, key.len) == 0) {
                if (page_out) *page_out = p;
                if (slot_out) *slot_out = s;
                return 1;
            }
        }
    }
    return 0;
}

int ps_var_put(ps_vartable_t *vt, PSKey key, const void *val, uint32_t val_len)
{
    if (!vt || !key.data || key.len == 0 || key.len > 512 || (val_len > 0 && !val))
        return PS_ERR_INVALID;

    /* If key already exists, tombstone the old slot first */
    uint32_t old_p = 0;
    uint16_t old_s = 0;
    if (find_var_slot(vt, key, &old_p, &old_s)) {
        uint8_t *pg = bp_fetch(vt->pool, old_p, 1);
        if (!pg) return PS_ERR_IO;
        var_slot sl;
        size_t sl_off = BP_PAGE_HDR + sizeof(var_page_hdr) + old_s * sizeof sl;
        memcpy(&sl, pg + sl_off, sizeof sl);
        sl.flags = VSLOT_DEAD;
        memcpy(pg + sl_off, &sl, sizeof sl);
        vt->hdr.live_records--;
    }

    int use_ovf = (key.len + val_len > VAR_INLINE_MAX);
    uint16_t inline_val_len = use_ovf ? (uint16_t)sizeof(ovf_ref) : (uint16_t)val_len;
    uint16_t total_entry_len = (uint16_t)(key.len + inline_val_len);

    ovf_ref oref;
    if (use_ovf) {
        oref.total_val_len  = val_len;
        oref.first_ovf_page = write_overflow_chain(vt, val, val_len);
        if (!oref.first_ovf_page)
            return PS_ERR_IO;
    }

    uint32_t pno = vt->hdr.active_data_page;
    uint8_t *pg = bp_fetch(vt->pool, pno, 1);
    if (!pg)
        return PS_ERR_IO;

    var_page_hdr ph;
    memcpy(&ph, pg + BP_PAGE_HDR, sizeof ph);
    size_t dir_end = BP_PAGE_HDR + sizeof ph + (size_t)(ph.num_slots + 1) * sizeof(var_slot);

    if (ph.page_type != 1 || ph.free_upper < dir_end + total_entry_len) {
        pno = alloc_slotted_page(vt);
        if (!pno)
            return PS_ERR_IO;
        pg = bp_fetch(vt->pool, pno, 1);
        if (!pg)
            return PS_ERR_IO;
        memcpy(&ph, pg + BP_PAGE_HDR, sizeof ph);
    }

    ph.free_upper = (uint16_t)(ph.free_upper - total_entry_len);
    memcpy(pg + ph.free_upper, key.data, key.len);
    if (use_ovf) {
        memcpy(pg + ph.free_upper + key.len, &oref, sizeof oref);
    } else if (val_len > 0) {
        memcpy(pg + ph.free_upper + key.len, val, val_len);
    }

    var_slot sl;
    sl.offset  = ph.free_upper;
    sl.key_len = key.len;
    sl.rec_len = inline_val_len;
    sl.flags   = (uint16_t)(VSLOT_LIVE | (use_ovf ? VSLOT_OVERFLOW : 0u));

    memcpy(pg + BP_PAGE_HDR + sizeof ph + ph.num_slots * sizeof sl, &sl, sizeof sl);
    ph.num_slots++;
    memcpy(pg + BP_PAGE_HDR, &ph, sizeof ph);

    bloom_add_bytes(&vt->bloom, key.data, key.len);
    vt->hdr.live_records++;
    return PS_OK;
}

int ps_var_get(ps_vartable_t *vt, PSKey key, void *val_out, uint32_t buf_cap, uint32_t *val_len_out)
{
    if (!vt || !key.data || key.len == 0)
        return PS_ERR_INVALID;

    uint32_t pno = 0;
    uint16_t sidx = 0;
    if (!find_var_slot(vt, key, &pno, &sidx))
        return PS_ERR_NOT_FOUND;

    uint8_t *pg = bp_fetch(vt->pool, pno, 0);
    if (!pg)
        return PS_ERR_IO;

    var_slot sl;
    memcpy(&sl, pg + BP_PAGE_HDR + sizeof(var_page_hdr) + sidx * sizeof sl, sizeof sl);

    if (sl.flags & VSLOT_OVERFLOW) {
        ovf_ref oref;
        memcpy(&oref, pg + sl.offset + sl.key_len, sizeof oref);
        if (val_len_out)
            *val_len_out = oref.total_val_len;
        return read_overflow_chain(vt, oref.first_ovf_page, oref.total_val_len, val_out, buf_cap);
    }

    if (val_len_out)
        *val_len_out = sl.rec_len;
    if (val_out && buf_cap > 0) {
        uint32_t to_copy = sl.rec_len < buf_cap ? sl.rec_len : buf_cap;
        memcpy(val_out, pg + sl.offset + sl.key_len, to_copy);
    }
    return PS_OK;
}

int ps_var_delete(ps_vartable_t *vt, PSKey key)
{
    if (!vt || !key.data || key.len == 0)
        return PS_ERR_INVALID;

    uint32_t pno = 0;
    uint16_t sidx = 0;
    if (!find_var_slot(vt, key, &pno, &sidx))
        return PS_ERR_NOT_FOUND;

    uint8_t *pg = bp_fetch(vt->pool, pno, 1);
    if (!pg)
        return PS_ERR_IO;

    var_slot sl;
    size_t sl_off = BP_PAGE_HDR + sizeof(var_page_hdr) + sidx * sizeof sl;
    memcpy(&sl, pg + sl_off, sizeof sl);
    sl.flags = VSLOT_DEAD;
    memcpy(pg + sl_off, &sl, sizeof sl);
    vt->hdr.live_records--;
    return PS_OK;
}

uint32_t ps_var_count(const ps_vartable_t *vt)
{
    return vt ? vt->hdr.live_records : 0;
}
