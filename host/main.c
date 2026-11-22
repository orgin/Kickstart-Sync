/*
 * KickSync - command line front end for the KickSync library.
 *
 * usage: KickSync [options] HOST[:PORT] Kickstart.zip [New_Kickstart.zip]
 *        KickSync --list HOST[:PORT]
 */
#include "kicksync.h"

#include <stdarg.h>
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
    char  *notes;               /* printed after the table */
    size_t notes_len;
} rows;

static void oom(void)
{
    fprintf(stderr, PROGNAME ": out of memory\n");
    exit(1);
}

static void note(rows *t, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    char *nn;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !(nn = (char *)realloc(t->notes, t->notes_len + (size_t)n + 1)))
        oom();
    t->notes = nn;
    vsnprintf(t->notes + t->notes_len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    t->notes_len += (size_t)n;
}

/* What differs between the two Kicklayouts, for the notes after the table. */
static void layout_notes(rows *t, const ksync_entry_report *rep)
{
    size_t i;

    if (rep->action == KSYNC_ACT_KEPT_KICKLAYOUT)
        note(t, "\n%s: the Amiga's copy (%s) differs; the zip's copy was kept.\n",
             rep->entry_name, rep->amiga_name);
    else
        note(t, "\n%s: replaced by the Amiga's copy (%s).\n",
             rep->entry_name, rep->amiga_name);
    for (i = 0; i < rep->layout_only_zip_count; i++)
        note(t, "  only in the zip's copy:    MODULE %s\n", rep->layout_only_zip[i]);
    for (i = 0; i < rep->layout_only_amiga_count; i++)
        note(t, "  only in the Amiga's copy:  MODULE %s\n", rep->layout_only_amiga[i]);
    if (!rep->layout_only_zip_count && !rep->layout_only_amiga_count)
        note(t, "  Both load the same modules; they differ only in other lines\n"
                "  (upper/lower case, comments, order or other keywords).\n");
    if (rep->action == KSYNC_ACT_KEPT_KICKLAYOUT)
        note(t, "  Merge by hand if needed, or use --take-kicklayout to take the Amiga's copy.\n");
}

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
        "  -k, --take-kicklayout  replace Kicklayout with the Amiga's copy if it\n"
        "                       differs (by default Kicklayout is never replaced)\n"
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
        if (!nr)
            oom();
        t->r = nr;
        t->cap = ncap;
    }
    r = &t->r[t->n++];
    r->entry = (char *)malloc(strlen(rep->entry_name) + 1);
    if (!r->entry)
        oom();
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
                 rep->reason == KSYNC_REASON_NO_ZIP_VERSION   ? " (no version in zip)" :
                 rep->reason == KSYNC_REASON_TAKE_KICKLAYOUT  ? " (--take-kicklayout, see below)" : "");
        if (rep->renamed) {
            size_t l = strlen(r->result);
            snprintf(r->result + l, sizeof(r->result) - l, " (Amiga: %s, renamed)",
                     rep->amiga_name);
        }
        if (rep->reason == KSYNC_REASON_TAKE_KICKLAYOUT)
            layout_notes(t, rep);
        break;
    case KSYNC_ACT_SAME:
        snprintf(r->result, sizeof(r->result), "same%s",
                 rep->identical    ? " (identical)" :
                 rep->size_differs ? " (size differs!)" : "");
        break;
    case KSYNC_ACT_KEPT_KICKLAYOUT:
        snprintf(r->result, sizeof(r->result), "kept (Kicklayout differs, see below)");
        layout_notes(t, rep);
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
    rows table = { NULL, 0, 0, NULL, 0 };

    ksync_options_init(&opt);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-n") == 0 || strcmp(a, "--dry-run") == 0) {
            opt.dry_run = 1;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--force") == 0) {
            opt.force = 1;
        } else if (strcmp(a, "-k") == 0 || strcmp(a, "--take-kicklayout") == 0) {
            opt.take_kicklayout = 1;
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
    if (table.notes) {
        fputs(table.notes, stdout);
        free(table.notes);
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
