/*
 * pagestore - interactive shell over the PageStore engine.
 * Tables created here use a fixed "contact" schema (name, phone, email);
 * the engine itself is schema-agnostic and only sees fixed-size bytes.
 */
#define _POSIX_C_SOURCE 200809L
#include "pagestore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[32];
    char phone[16];
    char email[48];
} contact;

static ps_db    *g_db;
static ps_table *g_tbl;

static void help(void)
{
    puts("Commands:\n"
         "  open <dir>                         open/create a database directory\n"
         "  tables                             list tables in the database\n"
         "  create <table>                     create a contacts table\n"
         "  drop <table>                       delete a table (.dat and .ndx)\n"
         "  use <table>                        open a table (closes the current one)\n"
         "  insert <id> <name> <phone> <email> add a contact\n"
         "  get <id>                           point lookup (hash index)\n"
         "  exists <id>                        check whether a key exists\n"
         "  update <id> <name> <phone> <email> overwrite a contact in place\n"
         "  delete <id>                        soft delete (recycled by freelist / vacuum)\n"
         "  count                              number of live records\n"
         "  range <lo> <hi>                    key-ordered scan (AVL index)\n"
         "  list                               all contacts in key order\n"
         "  find <name|phone|email> <value>    full scan on a non-key field\n"
         "  vacuum                             compact the table, cluster by key\n"
         "  flush                              write dirty pages to disk\n"
         "  stats                              index, file, freelist and cache statistics\n"
         "  help | quit");
}

static void fill(contact *c, const char *name, const char *phone, const char *email)
{
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->phone, sizeof c->phone, "%s", phone);
    snprintf(c->email, sizeof c->email, "%s", email);
}

static void print_contact(int32_t id, const contact *c)
{
    printf("  %-8d %-20s %-14s %s\n", id, c->name, c->phone, c->email);
}

static int print_cb(int32_t key, const void *rec, void *ctx)
{
    (void)ctx;
    print_contact(key, rec);
    return 0;
}

typedef struct {
    const char *field;
    const char *value;
    int         hits;
} find_ctx;

static int find_cb(int32_t key, const void *rec, void *ctx)
{
    find_ctx *f = ctx;
    const contact *c = rec;
    const char *v = strcmp(f->field, "name") == 0  ? c->name
                  : strcmp(f->field, "phone") == 0 ? c->phone
                                                   : c->email;
    if (strcmp(v, f->value) == 0) {
        print_contact(key, c);
        f->hits++;
    }
    return 0;
}

static int need_db(void)
{
    if (!g_db) puts("error: no database open (use: open <dir>)");
    return g_db != NULL;
}

static int need_tbl(void)
{
    if (!g_tbl) puts("error: no table selected (use: use <table>)");
    return g_tbl != NULL;
}

static int parse_key(const char *s, int32_t *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
        puts("error: key must be a 32-bit integer");
        return 0;
    }
    *out = (int32_t)v;
    return 1;
}

static void report(int rc, const char *ok_msg)
{
    if (rc == PS_OK) puts(ok_msg);
    else printf("error: %s\n", ps_strerror(rc));
}

static void close_table(void)
{
    if (g_tbl) {
        int rc = ps_table_close(g_tbl);
        if (rc != PS_OK) printf("warning: close failed: %s\n", ps_strerror(rc));
        g_tbl = NULL;
    }
}

static void cmd_stats(void)
{
    ps_table_stats s;
    ps_table_get_stats(g_tbl, &s);
    uint64_t total = s.cache_hits + s.cache_misses;
    printf("  table            %s\n", ps_table_name(g_tbl));
    printf("  live records     %u\n", s.live_records);
    printf("  dead slots       %u (reusable / reclaimable by vacuum)\n", s.dead_slots);
    printf("  slots reused     %llu\n", (unsigned long long)s.slots_reused);
    printf("  slot size        %u bytes, %u slots/page\n", s.slot_size, s.slots_per_page);
    printf("  data pages       %u\n", s.data_pages);
    printf("  hash buckets     %zu\n", s.hash_buckets);
    printf("  avl height       %d\n", s.avl_height);
    printf("  index source     %s\n", s.index_rebuilt ? "rebuilt from .dat (recovery)" : ".ndx file");
    printf("  buffer pool      %zu pages\n", s.pool_pages);
    printf("  cache hits/miss  %llu / %llu (%.1f%% hit ratio)\n",
           (unsigned long long)s.cache_hits, (unsigned long long)s.cache_misses,
           total ? 100.0 * (double)s.cache_hits / (double)total : 0.0);
    printf("  evictions        %llu, writebacks %llu\n",
           (unsigned long long)s.cache_evictions, (unsigned long long)s.cache_writebacks);
}

int main(int argc, char **argv)
{
    char line[512];
    puts("PageStore shell - type 'help' for commands");

    if (argc > 1) {
        int rc = ps_db_open(argv[1], &g_db);
        if (rc != PS_OK) printf("error: %s\n", ps_strerror(rc));
        else printf("opened database '%s'\n", argv[1]);
    }

    for (;;) {
        printf("pagestore%s%s> ", g_tbl ? ":" : "", g_tbl ? ps_table_name(g_tbl) : "");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;

        char *argv_[8];
        int n = 0;
        for (char *tok = strtok(line, " \t\r\n"); tok && n < 8; tok = strtok(NULL, " \t\r\n"))
            argv_[n++] = tok;
        if (n == 0)
            continue;
        const char *cmd = argv_[0];
        int32_t key, hi;
        contact c;

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0) {
            help();
        } else if (strcmp(cmd, "open") == 0 && n == 2) {
            close_table();
            ps_db_close(g_db);
            g_db = NULL;
            int rc = ps_db_open(argv_[1], &g_db);
            report(rc, "database opened");
        } else if (strcmp(cmd, "tables") == 0) {
            if (!need_db()) continue;
            char names[64][PS_MAX_NAME + 1];
            int cnt = ps_db_list_tables(g_db, names, 64);
            if (cnt < 0) { printf("error: %s\n", ps_strerror(cnt)); continue; }
            if (cnt == 0) puts("  (no tables)");
            for (int i = 0; i < cnt; i++) printf("  %s\n", names[i]);
        } else if (strcmp(cmd, "create") == 0 && n == 2) {
            if (!need_db()) continue;
            report(ps_table_create(g_db, argv_[1], sizeof(contact)), "table created");
        } else if (strcmp(cmd, "drop") == 0 && n == 2) {
            if (!need_db()) continue;
            if (g_tbl && strcmp(ps_table_name(g_tbl), argv_[1]) == 0)
                close_table();
            report(ps_table_drop(g_db, argv_[1]), "table dropped");
        } else if (strcmp(cmd, "use") == 0 && n == 2) {
            if (!need_db()) continue;
            close_table();
            int rc = ps_table_open(g_db, argv_[1], &g_tbl);
            if (rc == PS_OK && ps_record_size(g_tbl) != sizeof(contact)) {
                puts("error: table does not use the contact schema");
                close_table();
                continue;
            }
            report(rc, "table opened");
        } else if (strcmp(cmd, "insert") == 0 && n == 5) {
            if (!need_tbl() || !parse_key(argv_[1], &key)) continue;
            fill(&c, argv_[2], argv_[3], argv_[4]);
            report(ps_insert(g_tbl, key, &c), "inserted");
        } else if (strcmp(cmd, "update") == 0 && n == 5) {
            if (!need_tbl() || !parse_key(argv_[1], &key)) continue;
            fill(&c, argv_[2], argv_[3], argv_[4]);
            report(ps_update(g_tbl, key, &c), "updated");
        } else if (strcmp(cmd, "get") == 0 && n == 2) {
            if (!need_tbl() || !parse_key(argv_[1], &key)) continue;
            int rc = ps_get(g_tbl, key, &c);
            if (rc == PS_OK) print_contact(key, &c);
            else printf("error: %s\n", ps_strerror(rc));
        } else if (strcmp(cmd, "exists") == 0 && n == 2) {
            if (!need_tbl() || !parse_key(argv_[1], &key)) continue;
            puts(ps_exists(g_tbl, key) == 1 ? "yes" : "no");
        } else if (strcmp(cmd, "count") == 0) {
            if (!need_tbl()) continue;
            printf("  %u\n", ps_count(g_tbl));
        } else if (strcmp(cmd, "delete") == 0 && n == 2) {
            if (!need_tbl() || !parse_key(argv_[1], &key)) continue;
            report(ps_delete(g_tbl, key), "deleted");
        } else if (strcmp(cmd, "range") == 0 && n == 3) {
            if (!need_tbl() || !parse_key(argv_[1], &key) || !parse_key(argv_[2], &hi)) continue;
            long cnt = ps_range(g_tbl, key, hi, print_cb, NULL);
            if (cnt < 0) printf("error: %s\n", ps_strerror((int)cnt));
            else printf("  (%ld records)\n", cnt);
        } else if (strcmp(cmd, "list") == 0) {
            if (!need_tbl()) continue;
            long cnt = ps_range(g_tbl, INT32_MIN, INT32_MAX, print_cb, NULL);
            if (cnt < 0) printf("error: %s\n", ps_strerror((int)cnt));
            else printf("  (%ld records)\n", cnt);
        } else if (strcmp(cmd, "find") == 0 && n == 3) {
            if (!need_tbl()) continue;
            if (strcmp(argv_[1], "name") && strcmp(argv_[1], "phone") && strcmp(argv_[1], "email")) {
                puts("error: field must be name, phone or email");
                continue;
            }
            find_ctx f = { argv_[1], argv_[2], 0 };
            long scanned = ps_scan(g_tbl, find_cb, &f);
            if (scanned < 0) printf("error: %s\n", ps_strerror((int)scanned));
            else printf("  (%d matches, %ld records scanned)\n", f.hits, scanned);
        } else if (strcmp(cmd, "vacuum") == 0) {
            if (!need_tbl()) continue;
            ps_vacuum_result r;
            int rc = ps_vacuum(g_tbl, &r);
            if (rc != PS_OK) { printf("error: %s\n", ps_strerror(rc)); continue; }
            printf("  reclaimed %u slots, file %llu -> %llu bytes\n", r.slots_reclaimed,
                   (unsigned long long)r.bytes_before, (unsigned long long)r.bytes_after);
        } else if (strcmp(cmd, "flush") == 0) {
            if (!need_tbl()) continue;
            report(ps_flush(g_tbl), "flushed");
        } else if (strcmp(cmd, "stats") == 0) {
            if (!need_tbl()) continue;
            cmd_stats();
        } else {
            puts("unknown command or wrong arguments (type 'help')");
        }
    }

    close_table();
    ps_db_close(g_db);
    return 0;
}
