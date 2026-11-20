/*
 * fuzz/fuzz_page.c - Fuzz testing harness for PageStore page deserialization,
 * index loading, and WAL recovery.
 *
 * Compatible with clang -fsanitize=fuzzer -DLIBFUZZER, and includes a
 * standalone deterministic PRNG fuzz runner when built without -DLIBFUZZER.
 */
#define _POSIX_C_SOURCE 200809L
#include "pagestore.h"
#include "btree.h"
#include "varrec.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FUZZ_DIR "/tmp/pagestore_fuzz_db"

static int write_file_bytes(const char *path, const uint8_t *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, data, size);
    (void)w;
    close(fd);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    mkdir(FUZZ_DIR, 0755);

    ps_db *db = NULL;
    if (ps_db_open(FUZZ_DIR, &db) != PS_OK)
        return 0;

    /* 1. Fuzz .dat + .ndx + .wal recovery & read path */
    write_file_bytes(FUZZ_DIR "/fuzz.dat", data, size);
    write_file_bytes(FUZZ_DIR "/fuzz.ndx", data, size / 2);
    write_file_bytes(FUZZ_DIR "/fuzz.wal", data + (size / 2), size - (size / 2));

    ps_table *t = NULL;
    if (ps_table_open(db, "fuzz", &t) == PS_OK) {
        uint8_t out[PS_MAX_RECORD_SIZE];
        (void)ps_get(t, 0, out);
        (void)ps_get(t, 42, out);
        (void)ps_range(t, -100, 100, NULL, NULL);
        ps_table_close(t);
    }

    unlink(FUZZ_DIR "/fuzz.dat");
    unlink(FUZZ_DIR "/fuzz.ndx");
    unlink(FUZZ_DIR "/fuzz.wal");
    ps_db_close(db);
    return 0;
}

#ifndef LIBFUZZER
static uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

int main(void)
{
    uint64_t seed = UINT64_C(0xFA11DEAD12345678);
    uint8_t *buf = malloc(PS_PAGE_SIZE * 3);
    if (!buf)
        return 1;

    /* Create one valid baseline table to mutate against */
    mkdir(FUZZ_DIR, 0755);
    ps_db *db = NULL;
    ps_table *t = NULL;
    uint8_t valid_dat[PS_PAGE_SIZE * 2];
    size_t valid_len = 0;

    if (ps_db_open(FUZZ_DIR, &db) == PS_OK &&
        ps_table_create(db, "seed", 32) == PS_OK &&
        ps_table_open(db, "seed", &t) == PS_OK) {
        char rec[32] = "seed-record";
        for (int32_t i = 0; i < 20; i++)
            ps_insert(t, i, rec);
        ps_table_close(t);
        ps_db_close(db);

        int fd = open(FUZZ_DIR "/seed.dat", O_RDONLY);
        if (fd >= 0) {
            ssize_t r = read(fd, valid_dat, sizeof valid_dat);
            if (r > 0) valid_len = (size_t)r;
            close(fd);
        }
        unlink(FUZZ_DIR "/seed.dat");
        unlink(FUZZ_DIR "/seed.ndx");
        unlink(FUZZ_DIR "/seed.wal");
    }

    for (int iter = 0; iter < 1500; iter++) {
        size_t len = (size_t)(xorshift64(&seed) % (PS_PAGE_SIZE * 2 + 128));
        if ((iter & 1) && valid_len > 0) {
            len = valid_len;
            memcpy(buf, valid_dat, valid_len);
            /* Flip 1..8 random bytes in the valid image */
            int flips = 1 + (int)(xorshift64(&seed) % 8);
            for (int f = 0; f < flips; f++) {
                size_t pos = (size_t)(xorshift64(&seed) % len);
                buf[pos] ^= (uint8_t)(1u << (xorshift64(&seed) & 7u));
            }
        } else {
            for (size_t i = 0; i < len; i++)
                buf[i] = (uint8_t)(xorshift64(&seed) & 0xFFu);
        }
        LLVMFuzzerTestOneInput(buf, len);
    }

    free(buf);
    rmdir(FUZZ_DIR);
    printf("  fuzz_page                  ok (1,500 mutated page/WAL images tested)\n");
    return 0;
}
#endif
