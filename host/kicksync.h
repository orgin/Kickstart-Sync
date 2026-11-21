/*
 * KickSync - library for updating a Kickstart.zip with newer modules
 * served by the KickServ commodity running on AmigaOS 4.
 *
 * Add kicksync.c to your program, #include "kicksync.h" and link with
 * zlib (-lz; on Windows also ws2_32).
 *
 * Simple use:
 *
 *     ksync_options opt;
 *     ksync_result  res;
 *     ksync_options_init(&opt);
 *     opt.host = "localhost";
 *     opt.port = 6800;
 *     opt.input_zip = "Kickstart.zip";
 *     if (ksync_update_zip(&opt, &res) != KSYNC_OK)
 *         fprintf(stderr, "%s\n", res.error);
 *
 * Rules used by ksync_update_zip():
 *   - Each zip entry is looked up on the Amiga by its file name without the
 *     directory part, ignoring upper/lower case like AmigaOS does.
 *   - The Amiga copy is taken unless both copies carry a $VER: version and
 *     the Amiga version is not newer. So a missing $VER: on either side
 *     means the Amiga copy is taken.
 *   - Entries are written with the exact name (and case) from the original
 *     zip, in the original order. Unchanged entries are copied verbatim.
 *
 * The library keeps no global state (apart from WSAStartup on Windows), so
 * separate connections may be used from separate threads.
 */
#ifndef KICKSYNC_H
#define KICKSYNC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KSYNC_LIB_VERSION   "1.0"
#define KSYNC_DEFAULT_PORT  6800
#define KSYNC_PROTO_VERSION 1
#define KSYNC_NAME_MAX      256
#define KSYNC_VER_MAX       201

/* Status codes. Negative values are errors. */
enum {
    KSYNC_OK            =  0,
    KSYNC_NOT_FOUND     =  1,   /* ksync_info()/ksync_get(): no such file on the Amiga */
    KSYNC_ERR_ARGS      = -1,   /* invalid options */
    KSYNC_ERR_CONNECT   = -2,   /* cannot connect / resolve host */
    KSYNC_ERR_NET       = -3,   /* send/receive failed, timeout, connection lost */
    KSYNC_ERR_PROTOCOL  = -4,   /* unexpected reply from the server */
    KSYNC_ERR_SERVER    = -5,   /* server answered ERR */
    KSYNC_ERR_ZIP       = -6,   /* input zip is damaged or uses unsupported features */
    KSYNC_ERR_IO        = -7,   /* local file could not be read/written */
    KSYNC_ERR_EXISTS    = -8,   /* output exists and force is not set */
    KSYNC_ERR_NOMEM     = -9
};

const char *ksync_status_string(int status);

/* ---------------------------------------------------------------------- */
/* Versions                                                                */

/* A version like 54.30 or 1.2.3; n == 0 means "no version". */
typedef struct ksync_version {
    int n;
    int part[3];
} ksync_version;

/* Copies the text after the first "$VER:" in data into out (empty if none).
   Returns 1 if a $VER: tag was found, 0 otherwise. */
int  ksync_find_verstring(const unsigned char *data, size_t len, char *out, size_t outlen);

/* "exec.library 54.30 (1.2.2021)" -> 54.30. Returns 1 if a version was found. */
int  ksync_parse_version(const char *verstring, ksync_version *v);

/* <0, 0, >0 like strcmp. A missing version compares lower than any version. */
int  ksync_compare_version(const ksync_version *a, const ksync_version *b);

/* Writes "54.30" (or "?" for no version) into buf. */
void ksync_format_version(const ksync_version *v, char *buf, size_t len);

/* ---------------------------------------------------------------------- */
/* Talking to KickServ                                                     */

typedef struct ksync_conn ksync_conn;

typedef struct ksync_file_info {
    char          name[KSYNC_NAME_MAX];  /* spelling used on the Amiga */
    unsigned long size;
    unsigned long date;               /* seconds since 1978-01-01, Amiga local time */
    char          ver[KSYNC_VER_MAX];    /* $VER: string, may be empty */
} ksync_file_info;

/* Returns NULL on failure with a message in err. timeout_ms <= 0: 15 s. */
ksync_conn    *ksync_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen);
void        ksync_close(ksync_conn *c);
const char *ksync_conn_error(const ksync_conn *c);   /* message for the last failure */

/* KSYNC_OK, KSYNC_NOT_FOUND or an error. info->name is set to the name asked for. */
int  ksync_info(ksync_conn *c, const char *name, ksync_file_info *info);

/* KSYNC_OK (free *data with ksync_free()), KSYNC_NOT_FOUND or an error. */
int  ksync_get(ksync_conn *c, const char *name, unsigned char **data, size_t *len);

/* All files in the served directory. Free *files with ksync_free(). */
int  ksync_list(ksync_conn *c, ksync_file_info **files, size_t *count);

void ksync_free(void *p);

/* ---------------------------------------------------------------------- */
/* Updating a Kickstart.zip                                                */

typedef enum ksync_action {
    KSYNC_ACT_UPDATED,          /* Amiga copy put into the new zip */
    KSYNC_ACT_SAME,             /* same version, zip copy kept */
    KSYNC_ACT_KEPT_ZIP_NEWER,   /* zip copy has a higher version */
    KSYNC_ACT_KEPT_NOT_ON_AMIGA /* no file of that name on the Amiga */
} ksync_action;

typedef enum ksync_reason {
    KSYNC_REASON_NONE,
    KSYNC_REASON_NEWER,             /* Amiga version is higher */
    KSYNC_REASON_NO_AMIGA_VERSION,  /* Amiga file has no $VER: */
    KSYNC_REASON_NO_ZIP_VERSION     /* zip entry has no $VER: */
} ksync_reason;

typedef struct ksync_entry_report {
    const char *entry_name;     /* name in the zip (kept in the new zip) */
    const char *amiga_name;     /* name on the Amiga, NULL if not found */
    ksync_version  zip_version;
    ksync_version  amiga_version;
    ksync_action   action;
    ksync_reason   reason;         /* why it was updated */
    int         renamed;        /* Amiga spelling differs in case */
    int         size_differs;   /* KSYNC_ACT_SAME but sizes differ */
} ksync_entry_report;

/* Pointers in the report are only valid during the call. */
typedef void (*ksync_entry_fn)(const ksync_entry_report *r, void *user);
typedef void (*ksync_warn_fn)(const char *message, void *user);

typedef struct ksync_options {
    const char *host;           /* required unless conn is given */
    int         port;           /* default KSYNC_DEFAULT_PORT */
    int         timeout_ms;     /* default 15000 */
    ksync_conn    *conn;           /* optional: use this open connection */
    const char *input_zip;      /* required */
    const char *output_zip;     /* NULL: Kickstart_<YYYY-MM-DD>.zip in cwd */
    int         dry_run;        /* only report, write nothing */
    int         force;          /* overwrite an existing output file */
    ksync_entry_fn on_entry;       /* optional, called once per file entry */
    ksync_warn_fn  on_warning;     /* optional */
    void       *user;
} ksync_options;

typedef struct ksync_result {
    int  entries;               /* file entries examined */
    int  updated;               /* entries taken from the Amiga */
    char output_path[1024];
    char error[512];            /* set when ksync_update_zip() fails */
} ksync_result;

void ksync_options_init(ksync_options *opt);
int  ksync_update_zip(const ksync_options *opt, ksync_result *res);

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */

/* "host", "host:port", "1.2.3.4:port", "[::1]:port". Returns KSYNC_OK or KSYNC_ERR_ARGS. */
int  ksync_parse_hostport(const char *s, char *host, size_t hostlen, int *port);

/* "Kickstart_<YYYY-MM-DD>.zip" using today's local date. */
void ksync_default_output_name(char *buf, size_t len);

/* Amiga date (seconds since 1978-01-01) to calendar fields. */
void ksync_amiga_date(unsigned long secs, int *year, int *month, int *day,
                   int *hour, int *minute, int *second);

/* Compare names like AmigaOS does (case-insensitive, ISO-8859-1). */
int  ksync_name_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* KICKSYNC_H */
