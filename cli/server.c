/*
 * pagestored - Multi-threaded TCP network server for the PageStore engine.
 *
 * Usage:
 *   ./build/pagestored <db_dir> [port]
 *
 * Line-oriented protocol:
 *   CREATE <table>
 *   USE <table>
 *   PUT <int32_key> <string_value>
 *   GET <int32_key>
 *   DEL <int32_key>
 *   RANGE <lo_key> <hi_key>
 *   COUNT
 *   VACUUM
 *   STATS
 *   QUIT
 */
#define _POSIX_C_SOURCE 200809L
#include "pagestore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PSD_DEFAULT_PORT 7654
#define PSD_VAL_SIZE     128

static volatile sig_atomic_t g_running   = 1;
static int                   g_listen_fd = -1;
static ps_db                *g_db        = NULL;

static void on_sig(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static int send_fmt(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int send_fmt(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return -1;
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    const char *p = buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static ssize_t read_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 1) {
            if (c == '\n')
                break;
            if (c == '\r')
                continue;
            buf[n++] = c;
        } else if (r == 0) {
            if (n == 0)
                return 0;
            break;
        } else {
            if (errno == EINTR)
                continue;
            return -1;
        }
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

static int range_send_cb(int32_t key, const void *record, void *ctx)
{
    int fd = *(int *)ctx;
    const char *val = record;
    send_fmt(fd, "ROW %d %s\n", key, val);
    return 0;
}

static void *client_worker(void *arg)
{
    int fd = *(int *)arg;
    free(arg);

    ps_table *tbl = NULL;
    char line[512];
    send_fmt(fd, "OK PageStore server ready\n");

    while (read_line(fd, line, sizeof line) > 0) {
        char *save = NULL;
        char *cmd = strtok_r(line, " \t", &save);
        if (!cmd)
            continue;

        if (strcmp(cmd, "QUIT") == 0) {
            send_fmt(fd, "OK bye\n");
            break;
        }
        if (strcmp(cmd, "CREATE") == 0) {
            char *name = strtok_r(NULL, " \t", &save);
            if (!name) {
                send_fmt(fd, "ERR usage: CREATE <table>\n");
                continue;
            }
            int rc = ps_table_create(g_db, name, PSD_VAL_SIZE);
            if (rc == PS_OK) send_fmt(fd, "OK created %s\n", name);
            else send_fmt(fd, "ERR %s\n", ps_strerror(rc));
            continue;
        }
        if (strcmp(cmd, "USE") == 0) {
            char *name = strtok_r(NULL, " \t", &save);
            if (!name) {
                send_fmt(fd, "ERR usage: USE <table>\n");
                continue;
            }
            if (tbl) {
                ps_table_close(tbl);
                tbl = NULL;
            }
            int rc = ps_table_open(g_db, name, &tbl);
            if (rc == PS_OK) send_fmt(fd, "OK using %s\n", name);
            else send_fmt(fd, "ERR %s\n", ps_strerror(rc));
            continue;
        }
        if (!tbl) {
            send_fmt(fd, "ERR no table selected (USE <table>)\n");
            continue;
        }
        if (strcmp(cmd, "PUT") == 0) {
            char *kstr = strtok_r(NULL, " \t", &save);
            char *val  = strtok_r(NULL, "", &save);
            if (!kstr || !val) {
                send_fmt(fd, "ERR usage: PUT <key> <value>\n");
                continue;
            }
            int32_t key = (int32_t)strtol(kstr, NULL, 10);
            char rec[PSD_VAL_SIZE];
            memset(rec, 0, sizeof rec);
            snprintf(rec, sizeof rec, "%s", val);
            int rc = ps_insert(tbl, key, rec);
            if (rc == PS_ERR_DUPLICATE)
                rc = ps_update(tbl, key, rec);
            if (rc == PS_OK) send_fmt(fd, "OK\n");
            else send_fmt(fd, "ERR %s\n", ps_strerror(rc));
            continue;
        }
        if (strcmp(cmd, "GET") == 0) {
            char *kstr = strtok_r(NULL, " \t", &save);
            if (!kstr) {
                send_fmt(fd, "ERR usage: GET <key>\n");
                continue;
            }
            int32_t key = (int32_t)strtol(kstr, NULL, 10);
            char rec[PSD_VAL_SIZE];
            int rc = ps_get(tbl, key, rec);
            if (rc == PS_OK) send_fmt(fd, "OK %s\n", rec);
            else send_fmt(fd, "ERR %s\n", ps_strerror(rc));
            continue;
        }
        if (strcmp(cmd, "DEL") == 0) {
            char *kstr = strtok_r(NULL, " \t", &save);
            if (!kstr) {
                send_fmt(fd, "ERR usage: DEL <key>\n");
                continue;
            }
            int32_t key = (int32_t)strtol(kstr, NULL, 10);
            int rc = ps_delete(tbl, key);
            if (rc == PS_OK) send_fmt(fd, "OK\n");
            else send_fmt(fd, "ERR %s\n", ps_strerror(rc));
            continue;
        }
        if (strcmp(cmd, "RANGE") == 0) {
            char *lostr = strtok_r(NULL, " \t", &save);
            char *histr = strtok_r(NULL, " \t", &save);
            if (!lostr || !histr) {
                send_fmt(fd, "ERR usage: RANGE <lo> <hi>\n");
                continue;
            }
            int32_t lo = (int32_t)strtol(lostr, NULL, 10);
            int32_t hi = (int32_t)strtol(histr, NULL, 10);
            long n = ps_range(tbl, lo, hi, range_send_cb, &fd);
            send_fmt(fd, "END %ld\n", n);
            continue;
        }
        if (strcmp(cmd, "COUNT") == 0) {
            send_fmt(fd, "OK %u\n", ps_count(tbl));
            continue;
        }
        if (strcmp(cmd, "VACUUM") == 0) {
            ps_vacuum_result vr;
            int rc = ps_vacuum(tbl, &vr);
            if (rc == PS_OK)
                send_fmt(fd, "OK live=%u reclaimed=%u\n", vr.live_records, vr.slots_reclaimed);
            else
                send_fmt(fd, "ERR %s\n", ps_strerror(rc));
            continue;
        }
        if (strcmp(cmd, "STATS") == 0) {
            ps_table_stats st;
            ps_table_get_stats(tbl, &st);
            send_fmt(fd, "OK live=%u dead=%u hits=%llu misses=%llu bloom_rejects=%llu wal_logged=%llu\n",
                     st.live_records, st.dead_slots,
                     (unsigned long long)st.cache_hits,
                     (unsigned long long)st.cache_misses,
                     (unsigned long long)st.bloom_rejections,
                     (unsigned long long)st.wal_records_logged);
            continue;
        }
        send_fmt(fd, "ERR unknown command\n");
    }

    if (tbl)
        ps_table_close(tbl);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *dir = (argc >= 2) ? argv[1] : "build/server_db";
    int port = (argc >= 3) ? atoi(argv[2]) : PSD_DEFAULT_PORT;

    if (ps_db_open(dir, &g_db) != PS_OK) {
        fprintf(stderr, "pagestored: failed to open db directory '%s'\n", dir);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        ps_db_close(g_db);
        return 1;
    }
    int yes = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(g_listen_fd, 64) < 0) {
        perror("bind/listen");
        close(g_listen_fd);
        ps_db_close(g_db);
        return 1;
    }

    printf("pagestored listening on port %d (db=%s)\n", port, dir);
    fflush(stdout);

    while (g_running) {
        struct sockaddr_in ca;
        socklen_t cal = sizeof ca;
        int cfd = accept(g_listen_fd, (struct sockaddr *)&ca, &cal);
        if (cfd < 0) {
            if (errno == EINTR && g_running)
                continue;
            break;
        }
        int *arg = malloc(sizeof *arg);
        if (!arg) {
            close(cfd);
            continue;
        }
        *arg = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker, arg) == 0)
            pthread_detach(tid);
        else {
            free(arg);
            close(cfd);
        }
    }

    if (g_listen_fd >= 0)
        close(g_listen_fd);
    ps_db_close(g_db);
    printf("pagestored shut down cleanly.\n");
    return 0;
}
