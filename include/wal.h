#ifndef PS_WAL_H
#define PS_WAL_H

#include <stddef.h>
#include <stdint.h>

#define PS_WAL_MAGIC 0x5053574Cu /* "PSWL" */

typedef enum {
    PS_WAL_INSERT = 1,
    PS_WAL_UPDATE = 2,
    PS_WAL_DELETE = 3
} ps_wal_op;

typedef struct {
    int      fd;
    uint64_t next_lsn;
    uint64_t records_logged;
    uint64_t records_replayed;
    int      sync_on_write; /* 1 = fsync every record, 0 = OS write + fsync on flush */
} ps_wal_t;

/* Callback invoked for each valid WAL record during crash recovery replay. */
typedef int (*ps_wal_replay_fn)(uint64_t lsn, ps_wal_op op, int32_t key,
                                const void *payload, uint32_t payload_len,
                                void *ctx);

int  ps_wal_open(ps_wal_t *wal, const char *path);
void ps_wal_close(ps_wal_t *wal);
int  ps_wal_append(ps_wal_t *wal, ps_wal_op op, int32_t key,
                   const void *payload, uint32_t payload_len);
int  ps_wal_checkpoint(ps_wal_t *wal);
int  ps_wal_replay(ps_wal_t *wal, uint32_t expected_rec_size,
                   ps_wal_replay_fn fn, void *ctx);
int  ps_wal_has_records(const ps_wal_t *wal);

#endif /* PS_WAL_H */
