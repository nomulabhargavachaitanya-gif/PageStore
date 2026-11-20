#define _POSIX_C_SOURCE 200809L
#include "wal.h"
#include "crc32.h"
#include "pagestore.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint32_t magic;
    uint32_t op;
    uint64_t lsn;
    int32_t  key;
    uint32_t payload_len;
    uint32_t crc32;
} wal_rec_hdr;

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int pread_all(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, p + got, len - got, off + (off_t)got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return (got == 0) ? 0 : -1; /* 0 = clean EOF, -1 = torn record */
        got += (size_t)n;
    }
    return 1;
}

int ps_wal_open(ps_wal_t *wal, const char *path)
{
    if (!wal || !path)
        return PS_ERR_INVALID;
    memset(wal, 0, sizeof *wal);
    wal->fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal->fd < 0)
        return PS_ERR_IO;
    wal->next_lsn = 1;
    wal->sync_on_write = 0;
    return PS_OK;
}

void ps_wal_close(ps_wal_t *wal)
{
    if (!wal)
        return;
    if (wal->fd >= 0) {
        close(wal->fd);
        wal->fd = -1;
    }
}

int ps_wal_has_records(const ps_wal_t *wal)
{
    if (!wal || wal->fd < 0)
        return 0;
    struct stat st;
    if (fstat(wal->fd, &st) != 0)
        return 0;
    return st.st_size > 0;
}

int ps_wal_append(ps_wal_t *wal, ps_wal_op op, int32_t key,
                  const void *payload, uint32_t payload_len)
{
    if (!wal || wal->fd < 0)
        return PS_ERR_INVALID;
    if (payload_len > PS_MAX_RECORD_SIZE || (payload_len > 0 && !payload))
        return PS_ERR_INVALID;

    uint8_t frame[sizeof(wal_rec_hdr) + PS_MAX_RECORD_SIZE];
    wal_rec_hdr hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic       = PS_WAL_MAGIC;
    hdr.op          = (uint32_t)op;
    hdr.lsn         = wal->next_lsn++;
    hdr.key         = key;
    hdr.payload_len = payload_len;
    hdr.crc32       = 0;

    uint32_t crc = crc32_update(0, &hdr, offsetof(wal_rec_hdr, crc32));
    if (payload_len > 0)
        crc = crc32_update(crc, payload, payload_len);
    hdr.crc32 = crc;

    memcpy(frame, &hdr, sizeof hdr);
    if (payload_len > 0)
        memcpy(frame + sizeof hdr, payload, payload_len);

    if (write_all(wal->fd, frame, sizeof hdr + payload_len) != 0)
        return PS_ERR_IO;
    if (wal->sync_on_write && fsync(wal->fd) != 0)
        return PS_ERR_IO;

    wal->records_logged++;
    return PS_OK;
}

int ps_wal_checkpoint(ps_wal_t *wal)
{
    if (!wal || wal->fd < 0)
        return PS_ERR_INVALID;
    if (ftruncate(wal->fd, 0) != 0)
        return PS_ERR_IO;
    if (lseek(wal->fd, 0, SEEK_SET) < 0)
        return PS_ERR_IO;
    if (fsync(wal->fd) != 0)
        return PS_ERR_IO;
    return PS_OK;
}

int ps_wal_replay(ps_wal_t *wal, uint32_t expected_rec_size,
                  ps_wal_replay_fn fn, void *ctx)
{
    if (!wal || wal->fd < 0)
        return PS_ERR_INVALID;

    off_t off = 0;
    uint8_t payload[PS_MAX_RECORD_SIZE];

    for (;;) {
        wal_rec_hdr hdr;
        int rc = pread_all(wal->fd, &hdr, sizeof hdr, off);
        if (rc == 0)
            break; /* clean EOF */
        if (rc < 0)
            break; /* torn header at tail: stop replay and truncate */

        if (hdr.magic != PS_WAL_MAGIC ||
            hdr.payload_len > PS_MAX_RECORD_SIZE ||
            ((hdr.op == PS_WAL_INSERT || hdr.op == PS_WAL_UPDATE) &&
             hdr.payload_len != expected_rec_size) ||
            (hdr.op == PS_WAL_DELETE && hdr.payload_len != 0)) {
            break; /* corrupt header */
        }

        if (hdr.payload_len > 0) {
            if (pread_all(wal->fd, payload, hdr.payload_len,
                          off + (off_t)sizeof hdr) <= 0)
                break; /* torn payload */
        }

        uint32_t crc = crc32_update(0, &hdr, offsetof(wal_rec_hdr, crc32));
        if (hdr.payload_len > 0)
            crc = crc32_update(crc, payload, hdr.payload_len);
        if (crc != hdr.crc32)
            break; /* CRC mismatch at tail */

        if (fn) {
            int cb_rc = fn(hdr.lsn, (ps_wal_op)hdr.op, hdr.key,
                           hdr.payload_len ? payload : NULL,
                           hdr.payload_len, ctx);
            if (cb_rc != PS_OK)
                return cb_rc;
        }

        if (hdr.lsn >= wal->next_lsn)
            wal->next_lsn = hdr.lsn + 1;
        wal->records_replayed++;
        off += (off_t)(sizeof hdr + hdr.payload_len);
    }

    /* Truncate any torn tail beyond the last valid record */
    if (ftruncate(wal->fd, off) != 0)
        return PS_ERR_IO;
    return PS_OK;
}
