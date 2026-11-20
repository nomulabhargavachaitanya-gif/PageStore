#ifndef PS_SECINDEX_H
#define PS_SECINDEX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Secondary Index mapping a non-primary attribute (string or byte key) to
 * a list of int32_t primary keys (1-to-many secondary index).
 */

typedef struct ps_secindex ps_secindex_t;

ps_secindex_t *ps_secindex_create(size_t initial_buckets);
void           ps_secindex_destroy(ps_secindex_t *si);
int            ps_secindex_put(ps_secindex_t *si, const char *sec_key, int32_t primary_key);
int            ps_secindex_remove(ps_secindex_t *si, const char *sec_key, int32_t primary_key);
size_t         ps_secindex_lookup(const ps_secindex_t *si, const char *sec_key,
                                  int32_t *pks_out, size_t max_out);

#endif /* PS_SECINDEX_H */
