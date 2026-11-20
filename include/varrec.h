#ifndef PS_VARREC_H
#define PS_VARREC_H

#include "pagestore.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Variable-length record table (VARCHAR / BLOB) with arbitrary string or
 * composite keys (PSKey) and automatic overflow-page chaining.
 *
 * 4KB Slotted-Page Layout:
 *   [8B BP_PAGE_HDR][8B var_page_hdr][Slot Directory --->   <--- Payloads]
 *
 * Payloads larger than a single 4KB page are automatically segmented across
 * chained overflow pages.
 */

typedef struct {
    const void *data;
    uint16_t    len;
} PSKey;

typedef struct ps_vartable ps_vartable_t;

/* Helper to construct a PSKey from a NUL-terminated C string. */
PSKey ps_key_str(const char *s);

/* Helper to construct a composite PSKey from arbitrary bytes. */
PSKey ps_key_bytes(const void *data, uint16_t len);

int ps_var_open(const char *path, size_t pool_pages, ps_vartable_t **out);
int ps_var_close(ps_vartable_t *vt);

int ps_var_put(ps_vartable_t *vt, PSKey key, const void *val, uint32_t val_len);
int ps_var_get(ps_vartable_t *vt, PSKey key, void *val_out, uint32_t buf_cap, uint32_t *val_len_out);
int ps_var_delete(ps_vartable_t *vt, PSKey key);
uint32_t ps_var_count(const ps_vartable_t *vt);

#endif /* PS_VARREC_H */
