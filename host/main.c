/*
 * KickSync - command line front end for the KickSync library.
 *
 * usage: KickSync [options] HOST[:PORT] Kickstart.zip [New_Kickstart.zip]
 *        KickSync --list HOST[:PORT]
 */
#include "kicksync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGNAME "KickSync"

typedef struct row {
    char *entry;
    char  zipver[24];
    char  amigaver[24];
    char  result[300];
} row;

typedef struct rows {
    row   *r;
    size_t n, cap;
} rows;

static void usage(FILE *fp)
{
    fprintf(fp,
        "usage: " PROGNAME " [options] HOST[:PORT] Kickstart.zip [New_Kickstart.zip]\n"
        "       " PROGNAME " --list HOST[:PORT]\n"
        "\n"
        "Updates Kickstart.zip with newer modules from an AmigaOS 4 machine running\n"
        "the KickServ commodity. HOST may be a name or IP address (IPv6 as [addr]);\n"
        "PORT defaults to %d. Without New_Kickstart.zip the result is written to\n"
        "Kickstart_<YYYY-MM-DD>.zip in the current directory.\n"
        "\n"
        "options:\n"
        "  -n, --dry-run        only show what would be updated\n"
        "  -f, --force          overwrite the output file if it exists\n"
        "  -l, --list           list the files offered by the Amiga and exit\n"
        "  -t, --timeout SECS   network timeout (default 15)\n"
        "  -V, --version        show version\n"
        "  -h, --help           show this help\n",
        KSYNC_DEFAULT_PORT);
}

static void on_warning(const char *msg, void *user)
{
    (void)user;
    fprintf(stderr, "warning: %s\n", msg);
}

static void on_entry(const ksync_entry_report *rep, void *user)
{
    rows *t = (rows *)user;
    row *r;

    if (t->n == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 32;
        row *nr = (row *)realloc(t->r, ncap * sizeof(row));
        if (!nr) {
            fprintf(stderr, PROGNAME ": out of memory\n");
            exit(1);
        }
        t->r = nr;
        t->cap = ncap;
    }
    r = &t->r[t->n++];
    r->entry = (char *)malloc(strlen(rep->entry_name) + 1);
    if (!r->entry) {
        fprintf(stderr, PROGNAME ": out of memory\n");
        exit(1);
    }
    strcpy(r->entry, rep->entry_name);
    ksync_format_version(&rep->zip_version, r->zipver, sizeof(r->zipver));
    if (rep->amiga_name)
        ksync_format_version(&rep->amiga_version, r->amigaver, sizeof(r->amigaver));
    else
        strcpy(r->amigaver, "-");

    switch (rep->action) {
    case KSYNC_ACT_UPDATED:
        snprintf(r->result, sizeof(r->result), "UPDATED%s",
                 rep->reason == KSYNC_REASON_NO_AMIGA_VERSION ? " (no version on Amiga)" :
                 rep->reason == KSYNC_REASON_NO_ZIP_VERSION   ? " (no version in zip)" : "");
        if (rep->renamed) {
            size_t l = strlen(r->result);
            snprintf(r->result + l, sizeof(r->result) - l, " (Amiga: %s, renamed)",
                     rep->amiga_name);
        }
        break;
    case KSYNC_ACT_SAME:
        snprintf(r->result, sizeof(r->result), "same%s",
                 rep->size_differs ? " (size differs!)" : "");
        break;
    case KSYNC_ACT_KEPT_ZIP_NEWER:
        snprintf(r->result, sizeof(r->result), "kept (zip is newer)");
        break;
    case KSYNC_ACT_KEPT_NOT_ON_AMIGA:
        snprintf(r->result, sizeof(r->result), "kept (not on Amiga)");
        break;
    }
}

static int do_list(const char *host, int port, int timeout_ms)
{
    char err[512];
    ksync_conn *c = ksync_connect(host, port, timeout_ms, err, sizeof(err));
    ksync_file_info *f;
    size_t n, i;
    int r;

    if (!c) {
        fprintf(stderr, PROGNAME ": %s\n", err);
        return 1;
    }
    r = ksync_list(c, &f, &n);
    if (r != KSYNC_OK) {
        fprintf(stderr, PROGNAME ": LIST failed: %s\n", ksync_conn_error(c));
        ksync_close(c);
        return 1;
    }
    for (i = 0; i < n; i++) {
        int y, mo, d, h, mi, s;
        ksync_amiga_date(f[i].date, &y, &mo, &d, &h, &mi, &s);
        printf("%-28s %8lu  %04d-%02d-%02d %02d:%02d  %s\n",
               f[i].name, f[i].size, y, mo, d, h, mi, f[i].ver);
    }
    ksync_free(f);
    ksync_close(c);
    return 0;
}

int main(int argc, char **argv)
{
    const char *pos[3];
    int npos = 0, i, list = 0, timeout = 15, rc;
    char host[256];
    int port;
    ksync_options opt;
    ksync_result res;
    rows table = { NULL, 0, 0 };

    ksync_options_init(&opt);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-n") == 0 || strcmp(a, "--dry-run") == 0) {
            opt.dry_run = 1;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--force") == 0) {
            opt.force = 1;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            list = 1;
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--timeout") == 0 ||
                   strncmp(a, "--timeout=", 10) == 0) {
            const char *v = a[1] == '-' && a[9] == '=' ? a + 10 : (++i < argc ? argv[i] : NULL);
            char *end;
            if (!v || (timeout = (int)strtol(v, &end, 10)) <= 0 || *end) {
                fprintf(stderr, PROGNAME ": invalid timeout\n");
                return 2;
            }
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf(PROGNAME " %s\n", KSYNC_LIB_VERSION);
            return 0;
        } else if (strcmp(a, "--") == 0) {
            for (i++; i < argc && npos < 3; i++)
                pos[npos++] = argv[i];
            break;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, PROGNAME ": unknown option %s\n", a);
            usage(stderr);
            return 2;
        } else if (npos < 3) {
            pos[npos++] = a;
        } else {
            fprintf(stderr, PROGNAME ": too many arguments\n");
            usage(stderr);
            return 2;
        }
    }

    if (npos < 1 || (!list && npos < 2)) {
        usage(stderr);
        return 2;
    }
    if (ksync_parse_hostport(pos[0], host, sizeof(host), &port) != KSYNC_OK) {
        fprintf(stderr, PROGNAME ": invalid HOST[:PORT] \"%s\"\n", pos[0]);
        return 2;
    }
    if (list)
        return do_list(host, port, timeout * 1000);

    opt.host = host;
    opt.port = port;
    opt.timeout_ms = timeout * 1000;
    opt.input_zip = pos[1];
    opt.output_zip = npos > 2 ? pos[2] : NULL;
    opt.on_entry = on_entry;
    opt.on_warning = on_warning;
    opt.user = &table;

    rc = ksync_update_zip(&opt, &res);

    if (table.n) {
        size_t w = 5, k;
        for (k = 0; k < table.n; k++)
            if (strlen(table.r[k].entry) > w)
                w = strlen(table.r[k].entry);
        printf("%-*s  %-10s %-10s %s\n", (int)w, "Entry", "Zip", "Amiga", "Result");
        for (k = 0; k < table.n; k++) {
            printf("%-*s  %-10s %-10s %s\n", (int)w, table.r[k].entry,
                   table.r[k].zipver, table.r[k].amigaver, table.r[k].result);
            free(table.r[k].entry);
        }
        free(table.r);
    }

    if (rc != KSYNC_OK) {
        fprintf(stderr, PROGNAME ": %s\n", res.error);
        return 1;
    }
    if (opt.dry_run)
        printf("\n%d module(s) would be updated (dry run, nothing written)\n", res.updated);
    else
        printf("\n%d module(s) updated, written to %s\n", res.updated, res.output_path);
    return 0;
}
