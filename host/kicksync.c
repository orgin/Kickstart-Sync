/*
 * kicksync.c - see kicksync.h. Needs zlib.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "kicksync.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zlib.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET ksync_socket;
#  define KSYNC_BADSOCK   INVALID_SOCKET
#  define ksync_closesock closesocket
#  define KSYNC_SENDFLAGS 0
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
typedef int ksync_socket;
#  define KSYNC_BADSOCK   (-1)
#  define ksync_closesock close
#  ifdef MSG_NOSIGNAL
#    define KSYNC_SENDFLAGS MSG_NOSIGNAL
#  else
#    define KSYNC_SENDFLAGS 0
#  endif
#endif

#define KSYNC_LINE_MAX   4096
#define KSYNC_RECVBUF    16384
#define KSYNC_MAX_FILE   (256UL * 1024 * 1024)   /* sanity limit for one module */

/* ====================================================================== */
/* small helpers                                                          */

static void set_err(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (!buf || !len)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    size_t n;
    if (!dstlen)
        return;
    n = strlen(src);
    if (n >= dstlen)
        n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

const char *ksync_status_string(int status)
{
    switch (status) {
    case KSYNC_OK:           return "ok";
    case KSYNC_NOT_FOUND:    return "not found";
    case KSYNC_ERR_ARGS:     return "invalid arguments";
    case KSYNC_ERR_CONNECT:  return "cannot connect";
    case KSYNC_ERR_NET:      return "network error";
    case KSYNC_ERR_PROTOCOL: return "protocol error";
    case KSYNC_ERR_SERVER:   return "server error";
    case KSYNC_ERR_ZIP:      return "zip error";
    case KSYNC_ERR_IO:       return "file error";
    case KSYNC_ERR_EXISTS:   return "output file exists";
    case KSYNC_ERR_NOMEM:    return "out of memory";
    default:              return "unknown error";
    }
}

void ksync_free(void *p)
{
    free(p);
}

/* AmigaOS compares names case-insensitively using ISO-8859-1. */
static int latin1_lower(int c)
{
    c &= 0xFF;
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    return c;
}

int ksync_name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (latin1_lower((unsigned char)*a) != latin1_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* ====================================================================== */
/* dates                                                                  */

/* days since 1970-01-01 -> y/m/d (proleptic Gregorian) */
static void civil_from_days(long z, int *y, int *m, int *d)
{
    long era, yoe, doy, mp, yy;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    yoe = z - era * 146097;
    yy  = (yoe - yoe / 1460 + yoe / 36524 - yoe / 146096) / 365;
    doy = yoe - (365 * yy + yy / 4 - yy / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m  = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y  = (int)(yy + era * 400 + (*m <= 2));
}

void ksync_amiga_date(unsigned long secs, int *year, int *month, int *day,
                   int *hour, int *minute, int *second)
{
    /* 1978-01-01 is day 2922 after 1970-01-01 */
    long days = (long)(secs / 86400UL) + 2922;
    unsigned long rem = secs % 86400UL;
    civil_from_days(days, year, month, day);
    *hour = (int)(rem / 3600);
    *minute = (int)(rem / 60 % 60);
    *second = (int)(rem % 60);
}

static void amiga_to_dos(unsigned long secs, uint16_t *dtime, uint16_t *ddate)
{
    int y, mo, d, h, mi, s;
    ksync_amiga_date(secs, &y, &mo, &d, &h, &mi, &s);
    if (y < 1980) {
        y = 1980; mo = 1; d = 1; h = mi = s = 0;
    }
    if (y > 2107)
        y = 2107;
    *ddate = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *dtime = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
}

void ksync_default_output_name(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    snprintf(buf, len, "Kickstart_%04d-%02d-%02d.zip",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

/* ====================================================================== */
/* versions                                                               */

int ksync_find_verstring(const unsigned char *data, size_t len, char *out, size_t outlen)
{
    size_t i, o = 0;

    if (outlen)
        out[0] = '\0';
    for (i = 0; i + 5 <= len; i++) {
        if (data[i] == '$' && memcmp(data + i, "$VER:", 5) == 0) {
            size_t j = i + 5, end;
            while (j < len && data[j] == ' ')
                j++;
            end = j + (KSYNC_VER_MAX - 1) < len ? j + (KSYNC_VER_MAX - 1) : len;
            for (; j < end && o + 1 < outlen; j++) {
                unsigned char c = data[j];
                if (c == 0 || c == '\n' || c == '\r')
                    break;
                out[o++] = (char)((c == '\t' || c < 32) ? ' ' : c);
            }
            while (o > 0 && out[o - 1] == ' ')
                o--;
            if (outlen)
                out[o] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Is [s, s+len) exactly "digits.digits[.digits]"? */
static int parse_ver_token(const char *s, size_t len, ksync_version *v)
{
    ksync_version t;
    size_t i = 0;

    memset(&t, 0, sizeof(t));
    while (t.n < 3) {
        long n = 0;
        size_t start = i;
        while (i < len && isdigit((unsigned char)s[i])) {
            if (n < 100000000L)
                n = n * 10 + (s[i] - '0');
            i++;
        }
        if (i == start)
            return 0;
        t.part[t.n++] = (int)n;
        if (i == len)
            break;
        if (s[i] != '.')
            return 0;
        i++;
    }
    if (i != len || t.n < 2)
        return 0;
    *v = t;
    return 1;
}

int ksync_parse_version(const char *vs, ksync_version *v)
{
    const char *tok[64];
    size_t toklen[64];
    int ntok = 0, i;
    const char *p;

    memset(v, 0, sizeof(*v));
    if (!vs || !*vs)
        return 0;

    for (p = vs; *p && ntok < 64; ) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        tok[ntok] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        toklen[ntok] = (size_t)(p - tok[ntok]);
        ntok++;
    }
    /* Skip the module name, then the first "x.y" token; then the name itself. */
    for (i = 1; i < ntok; i++)
        if (parse_ver_token(tok[i], toklen[i], v))
            return 1;
    if (ntok > 0 && parse_ver_token(tok[0], toklen[0], v))
        return 1;

    /* Last resort: first "digits.digits" anywhere. */
    for (p = vs; *p; p++) {
        if (isdigit((unsigned char)*p) && (p == vs || !isdigit((unsigned char)p[-1]))) {
            const char *q = p;
            long a = 0, b = 0;
            while (isdigit((unsigned char)*q))
                a = a * 10 + (*q++ - '0');
            if (*q == '.' && isdigit((unsigned char)q[1])) {
                q++;
                while (isdigit((unsigned char)*q))
                    b = b * 10 + (*q++ - '0');
                v->n = 2;
                v->part[0] = (int)a;
                v->part[1] = (int)b;
                return 1;
            }
        }
    }
    return 0;
}

int ksync_compare_version(const ksync_version *a, const ksync_version *b)
{
    int i;
    for (i = 0; i < a->n && i < b->n; i++)
        if (a->part[i] != b->part[i])
            return a->part[i] < b->part[i] ? -1 : 1;
    return a->n == b->n ? 0 : (a->n < b->n ? -1 : 1);
}

void ksync_format_version(const ksync_version *v, char *buf, size_t len)
{
    if (v->n == 0)
        snprintf(buf, len, "?");
    else if (v->n == 2)
        snprintf(buf, len, "%d.%d", v->part[0], v->part[1]);
    else
        snprintf(buf, len, "%d.%d.%d", v->part[0], v->part[1], v->part[2]);
}

/* ====================================================================== */
/* host:port                                                              */

int ksync_parse_hostport(const char *s, char *host, size_t hostlen, int *port)
{
    const char *h = s, *pstr = NULL;
    size_t hl;
    const char *colon = strchr(s, ':');

    if (s[0] == '[') {                       /* [ipv6]:port */
        const char *rb = strchr(s, ']');
        if (!rb)
            return KSYNC_ERR_ARGS;
        h = s + 1;
        hl = (size_t)(rb - h);
        if (rb[1] == ':')
            pstr = rb + 2;
        else if (rb[1] != '\0')
            return KSYNC_ERR_ARGS;
    } else if (colon && !strchr(colon + 1, ':')) {   /* host:port */
        hl = (size_t)(colon - s);
        pstr = colon + 1;
    } else {                                 /* host, or bare IPv6 */
        hl = strlen(s);
    }
    if (hl == 0 || hl >= hostlen)
        return KSYNC_ERR_ARGS;
    memcpy(host, h, hl);
    host[hl] = '\0';

    if (pstr && *pstr) {
        char *end;
        long p = strtol(pstr, &end, 10);
        if (*end != '\0' || p <= 0 || p > 65535)
            return KSYNC_ERR_ARGS;
        *port = (int)p;
    } else if (pstr) {
        return KSYNC_ERR_ARGS;                  /* "host:" */
    } else {
        *port = KSYNC_DEFAULT_PORT;
    }
    return KSYNC_OK;
}

/* ====================================================================== */
/* network                                                                */

struct ksync_conn {
    ksync_socket     sock;
    int           wsa;
    size_t        pos, len;
    unsigned char buf[KSYNC_RECVBUF];
    char          err[256];
};

const char *ksync_conn_error(const ksync_conn *c)
{
    return c ? c->err : "no connection";
}

static int sock_set_nonblock(ksync_socket s, int on)
{
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static void sock_set_timeouts(ksync_socket s, int ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#  ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#  endif
#endif
}

static int sock_errno(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int connect_in_progress(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EINPROGRESS;
#endif
}

/* Connect one address with a timeout. Returns 0 on success. */
static int connect_timeout(ksync_socket s, const struct sockaddr *sa, int salen, int ms)
{
    fd_set wfds, efds;
    struct timeval tv;
    int r, soerr = 0;
#ifdef _WIN32
    int sl = sizeof(soerr);
#else
    socklen_t sl = sizeof(soerr);
#endif

    if (sock_set_nonblock(s, 1) < 0)
        return -1;
    r = connect(s, sa, salen);
    if (r != 0) {
        if (!connect_in_progress(sock_errno()))
            return -1;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        r = select((int)s + 1, NULL, &wfds, &efds, &tv);
        if (r <= 0) {
            if (r == 0)
                errno = ETIMEDOUT;
            return -1;
        }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) < 0 || soerr) {
            errno = soerr;
            return -1;
        }
    }
    return sock_set_nonblock(s, 0);
}

static int conn_fill(ksync_conn *c)
{
    int n;
    c->pos = 0;
    c->len = 0;
    n = (int)recv(c->sock, (char *)c->buf, (int)sizeof(c->buf), 0);
    if (n == 0) {
        set_err(c->err, sizeof(c->err), "connection closed by server");
        return KSYNC_ERR_NET;
    }
    if (n < 0) {
        set_err(c->err, sizeof(c->err), "receive failed or timed out");
        return KSYNC_ERR_NET;
    }
    c->len = (size_t)n;
    return KSYNC_OK;
}

static int conn_read_line(ksync_conn *c, char *line, size_t max)
{
    size_t o = 0;
    for (;;) {
        unsigned char ch;
        if (c->pos >= c->len) {
            int r = conn_fill(c);
            if (r != KSYNC_OK)
                return r;
        }
        ch = c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (o + 1 >= max) {
            set_err(c->err, sizeof(c->err), "reply line too long");
            return KSYNC_ERR_PROTOCOL;
        }
        line[o++] = (char)ch;
    }
    if (o > 0 && line[o - 1] == '\r')
        o--;
    line[o] = '\0';
    return KSYNC_OK;
}

static int conn_read_exact(ksync_conn *c, unsigned char *dst, size_t n)
{
    while (n > 0) {
        size_t avail;
        if (c->pos >= c->len) {
            int r = conn_fill(c);
            if (r != KSYNC_OK)
                return r;
        }
        avail = c->len - c->pos;
        if (avail > n)
            avail = n;
        memcpy(dst, c->buf + c->pos, avail);
        c->pos += avail;
        dst += avail;
        n -= avail;
    }
    return KSYNC_OK;
}

static int conn_send(ksync_conn *c, const char *cmd, const char *arg)
{
    char line[KSYNC_NAME_MAX + 32];
    const char *p;
    int len, sent = 0;

    if (arg) {
        for (p = arg; *p; p++)
            if (*p == '\n' || *p == '\r') {
                set_err(c->err, sizeof(c->err), "invalid file name");
                return KSYNC_ERR_ARGS;
            }
        if (strlen(arg) >= KSYNC_NAME_MAX) {
            set_err(c->err, sizeof(c->err), "file name too long");
            return KSYNC_ERR_ARGS;
        }
        len = snprintf(line, sizeof(line), "%s %s\n", cmd, arg);
    } else {
        len = snprintf(line, sizeof(line), "%s\n", cmd);
    }
    while (sent < len) {
        int n = (int)send(c->sock, line + sent, len - sent, KSYNC_SENDFLAGS);
        if (n <= 0) {
            set_err(c->err, sizeof(c->err), "send failed");
            return KSYNC_ERR_NET;
        }
        sent += n;
    }
    return KSYNC_OK;
}

/* Split a reply in place at TABs. Returns the number of fields. */
static int split_fields(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    while (n < max) {
        char *t = strchr(f[n - 1], '\t');
        if (!t)
            break;
        *t = '\0';
        f[n++] = t + 1;
    }
    return n;
}

static int parse_ulong(const char *s, unsigned long *out)
{
    char *end;
    if (!isdigit((unsigned char)*s))
        return 0;
    errno = 0;
    *out = strtoul(s, &end, 10);
    return *end == '\0' && errno == 0;
}

ksync_conn *ksync_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    ksync_conn *c;
    char line[KSYNC_LINE_MAX];
    char *f[4];
    int nf, r;

    if (!host || !*host || port <= 0 || port > 65535) {
        set_err(err, errlen, "invalid host or port");
        return NULL;
    }
    if (timeout_ms <= 0)
        timeout_ms = 15000;

    c = (ksync_conn *)calloc(1, sizeof(*c));
    if (!c) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    c->sock = KSYNC_BADSOCK;

#ifdef _WIN32
    {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            set_err(err, errlen, "WSAStartup failed");
            free(c);
            return NULL;
        }
        c->wsa = 1;
    }
#endif

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    r = getaddrinfo(host, portstr, &hints, &res);
    if (r != 0) {
#ifdef _WIN32
        set_err(err, errlen, "cannot resolve %s", host);
#else
        set_err(err, errlen, "cannot resolve %s: %s", host, gai_strerror(r));
#endif
        ksync_close(c);
        return NULL;
    }

    set_err(err, errlen, "cannot connect to %s:%d", host, port);
    for (ai = res; ai; ai = ai->ai_next) {
        ksync_socket s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == KSYNC_BADSOCK)
            continue;
        if (connect_timeout(s, ai->ai_addr, (int)ai->ai_addrlen, timeout_ms) == 0) {
            c->sock = s;
            break;
        }
#ifndef _WIN32
        set_err(err, errlen, "cannot connect to %s:%d: %s", host, port, strerror(errno));
#endif
        ksync_closesock(s);
    }
    freeaddrinfo(res);
    if (c->sock == KSYNC_BADSOCK) {
        ksync_close(c);
        return NULL;
    }
    sock_set_timeouts(c->sock, timeout_ms);

    /* greeting */
    if (conn_read_line(c, line, sizeof(line)) != KSYNC_OK) {
        set_err(err, errlen, "%s:%d: %s", host, port, c->err);
        ksync_close(c);
        return NULL;
    }
    nf = split_fields(line, f, 4);
    if (strcmp(f[0], "ERR") == 0) {
        set_err(err, errlen, "server refused connection: %s", nf > 1 ? f[1] : "");
        ksync_close(c);
        return NULL;
    }
    if (strcmp(f[0], "KICKSERV") != 0) {
        set_err(err, errlen, "%s:%d is not a KickServ server", host, port);
        ksync_close(c);
        return NULL;
    }
    err[0] = '\0';
    return c;
}

void ksync_close(ksync_conn *c)
{
    if (!c)
        return;
    if (c->sock != KSYNC_BADSOCK) {
        send(c->sock, "QUIT\n", 5, KSYNC_SENDFLAGS);
        ksync_closesock(c->sock);
    }
#ifdef _WIN32
    if (c->wsa)
        WSACleanup();
#endif
    free(c);
}

/* Reads a reply line; handles ERR. Returns KSYNC_OK with fields set. */
static int read_reply(ksync_conn *c, char *line, size_t linelen, char **f, int maxf, int *nf)
{
    int r = conn_read_line(c, line, linelen);
    if (r != KSYNC_OK)
        return r;
    *nf = split_fields(line, f, maxf);
    if (strcmp(f[0], "ERR") == 0) {
        set_err(c->err, sizeof(c->err), "%s", *nf > 1 ? f[1] : "error");
        return strcmp(*nf > 1 ? f[1] : "", "not found") == 0 ? KSYNC_NOT_FOUND : KSYNC_ERR_SERVER;
    }
    if (strcmp(f[0], "OK") != 0) {
        set_err(c->err, sizeof(c->err), "unexpected reply \"%.60s\"", f[0]);
        return KSYNC_ERR_PROTOCOL;
    }
    return KSYNC_OK;
}

int ksync_info(ksync_conn *c, const char *name, ksync_file_info *info)
{
    char line[KSYNC_LINE_MAX];
    char *f[4];
    int nf, r;

    memset(info, 0, sizeof(*info));
    copy_str(info->name, sizeof(info->name), name);
    if ((r = conn_send(c, "INFO", name)) != KSYNC_OK)
        return r;
    if ((r = read_reply(c, line, sizeof(line), f, 4, &nf)) != KSYNC_OK)
        return r;
    if (nf < 3 || !parse_ulong(f[1], &info->size) || !parse_ulong(f[2], &info->date)) {
        set_err(c->err, sizeof(c->err), "malformed INFO reply");
        return KSYNC_ERR_PROTOCOL;
    }
    if (nf > 3)
        copy_str(info->ver, sizeof(info->ver), f[3]);
    return KSYNC_OK;
}

int ksync_get(ksync_conn *c, const char *name, unsigned char **data, size_t *len)
{
    char line[KSYNC_LINE_MAX];
    char *f[3];
    unsigned long size;
    int nf, r;
    unsigned char *buf;

    *data = NULL;
    *len = 0;
    if ((r = conn_send(c, "GET", name)) != KSYNC_OK)
        return r;
    if ((r = read_reply(c, line, sizeof(line), f, 3, &nf)) != KSYNC_OK)
        return r;
    if (nf < 2 || !parse_ulong(f[1], &size) || size > KSYNC_MAX_FILE) {
        set_err(c->err, sizeof(c->err), "malformed GET reply");
        return KSYNC_ERR_PROTOCOL;
    }
    buf = (unsigned char *)malloc(size ? size : 1);
    if (!buf) {
        set_err(c->err, sizeof(c->err), "out of memory");
        return KSYNC_ERR_NOMEM;
    }
    if ((r = conn_read_exact(c, buf, size)) != KSYNC_OK) {
        free(buf);
        return r;
    }
    *data = buf;
    *len = size;
    return KSYNC_OK;
}

int ksync_list(ksync_conn *c, ksync_file_info **files, size_t *count)
{
    char line[KSYNC_LINE_MAX];
    char *f[5];
    int nf, r;
    ksync_file_info *arr = NULL;
    size_t n = 0, cap = 0;

    *files = NULL;
    *count = 0;
    if ((r = conn_send(c, "LIST", NULL)) != KSYNC_OK)
        return r;
    if ((r = read_reply(c, line, sizeof(line), f, 5, &nf)) != KSYNC_OK)
        return r == KSYNC_NOT_FOUND ? KSYNC_ERR_SERVER : r;

    for (;;) {
        ksync_file_info *fi;
        if ((r = conn_read_line(c, line, sizeof(line))) != KSYNC_OK)
            goto fail;
        if (strcmp(line, ".") == 0)
            break;
        nf = split_fields(line, f, 5);
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            ksync_file_info *na = (ksync_file_info *)realloc(arr, ncap * sizeof(*arr));
            if (!na) {
                set_err(c->err, sizeof(c->err), "out of memory");
                r = KSYNC_ERR_NOMEM;
                goto fail;
            }
            arr = na;
            cap = ncap;
        }
        fi = &arr[n];
        memset(fi, 0, sizeof(*fi));
        if (nf < 3 || !parse_ulong(f[0], &fi->size) || !parse_ulong(f[1], &fi->date)) {
            set_err(c->err, sizeof(c->err), "malformed LIST line");
            r = KSYNC_ERR_PROTOCOL;
            goto fail;
        }
        copy_str(fi->name, sizeof(fi->name), f[2]);
        if (nf > 3)
            copy_str(fi->ver, sizeof(fi->ver), f[3]);
        n++;
    }
    *files = arr;
    *count = n;
    return KSYNC_OK;

fail:
    free(arr);
    return r;
}

/* ====================================================================== */
/* zip reading                                                            */

typedef struct zentry {
    char                *name;
    uint16_t             made_by, needed, flags, method, dtime, ddate, int_attr;
    uint32_t             crc, csize, usize, ext_attr, local_off;
    const unsigned char *cextra;  uint16_t cextra_len;
    const unsigned char *comment; uint16_t comment_len;
    const unsigned char *lextra;  uint16_t lextra_len;
    const unsigned char *data;    /* compressed data inside the zip buffer */
} zentry;

typedef struct zarchive {
    unsigned char       *buf;
    size_t               size;
    zentry              *e;
    size_t               n;
    const unsigned char *comment;
    uint16_t             comment_len;
} zarchive;

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_file(const char *path, unsigned char **buf, size_t *size, char *err, size_t errlen)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    unsigned char *b;

    if (!fp) {
        set_err(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return KSYNC_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        set_err(err, errlen, "cannot read %s", path);
        fclose(fp);
        return KSYNC_ERR_IO;
    }
    b = (unsigned char *)malloc(sz ? (size_t)sz : 1);
    if (!b) {
        fclose(fp);
        set_err(err, errlen, "out of memory");
        return KSYNC_ERR_NOMEM;
    }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) {
        set_err(err, errlen, "cannot read %s", path);
        free(b);
        fclose(fp);
        return KSYNC_ERR_IO;
    }
    fclose(fp);
    *buf = b;
    *size = (size_t)sz;
    return KSYNC_OK;
}

static void zip_free(zarchive *z)
{
    size_t i;
    if (z->e)
        for (i = 0; i < z->n; i++)
            free(z->e[i].name);
    free(z->e);
    free(z->buf);
    memset(z, 0, sizeof(*z));
}

static int zip_parse(zarchive *z, const char *path, char *err, size_t errlen)
{
    const unsigned char *b = z->buf;
    size_t size = z->size, i, eocd = (size_t)-1, p;
    uint32_t cd_off, cd_size;
    uint16_t nent;

    if (size >= 22) {
        size_t lo = size > 22 + 65535 ? size - 22 - 65535 : 0;
        for (i = size - 22 + 1; i-- > lo; ) {
            if (rd32(b + i) == 0x06054b50 && i + 22 + rd16(b + i + 20) == size) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd == (size_t)-1) {
        set_err(err, errlen, "%s is not a zip file", path);
        return KSYNC_ERR_ZIP;
    }
    if (rd16(b + eocd + 4) != 0 || rd16(b + eocd + 6) != 0) {
        set_err(err, errlen, "%s: multi-part zip files are not supported", path);
        return KSYNC_ERR_ZIP;
    }
    nent    = rd16(b + eocd + 10);
    cd_size = rd32(b + eocd + 12);
    cd_off  = rd32(b + eocd + 16);
    z->comment_len = rd16(b + eocd + 20);
    z->comment = b + eocd + 22;
    if (nent == 0xFFFF || cd_off == 0xFFFFFFFFUL) {
        set_err(err, errlen, "%s: ZIP64 archives are not supported", path);
        return KSYNC_ERR_ZIP;
    }
    if ((size_t)cd_off + cd_size > eocd) {
        set_err(err, errlen, "%s: damaged central directory", path);
        return KSYNC_ERR_ZIP;
    }

    z->e = (zentry *)calloc(nent ? nent : 1, sizeof(zentry));
    if (!z->e) {
        set_err(err, errlen, "out of memory");
        return KSYNC_ERR_NOMEM;
    }

    p = cd_off;
    for (i = 0; i < nent; i++) {
        zentry *e = &z->e[i];
        uint16_t nlen, xlen, clen, lnlen;
        size_t lo;

        if (p + 46 > eocd || rd32(b + p) != 0x02014b50)
            goto damaged;
        e->made_by  = rd16(b + p + 4);
        e->needed   = rd16(b + p + 6);
        e->flags    = rd16(b + p + 8);
        e->method   = rd16(b + p + 10);
        e->dtime    = rd16(b + p + 12);
        e->ddate    = rd16(b + p + 14);
        e->crc      = rd32(b + p + 16);
        e->csize    = rd32(b + p + 20);
        e->usize    = rd32(b + p + 24);
        nlen        = rd16(b + p + 28);
        xlen        = rd16(b + p + 30);
        clen        = rd16(b + p + 32);
        e->int_attr = rd16(b + p + 36);
        e->ext_attr = rd32(b + p + 38);
        e->local_off = rd32(b + p + 42);
        if (p + 46 + nlen + xlen + clen > eocd)
            goto damaged;
        e->name = (char *)malloc(nlen + 1u);
        if (!e->name) {
            z->n = i;
            set_err(err, errlen, "out of memory");
            return KSYNC_ERR_NOMEM;
        }
        z->n = i + 1;
        memcpy(e->name, b + p + 46, nlen);
        e->name[nlen] = '\0';
        if (memchr(e->name, '\0', nlen))
            goto damaged;
        e->cextra = b + p + 46 + nlen;
        e->cextra_len = xlen;
        e->comment = e->cextra + xlen;
        e->comment_len = clen;
        p += 46u + nlen + xlen + clen;

        if (e->csize == 0xFFFFFFFFUL || e->usize == 0xFFFFFFFFUL || e->local_off == 0xFFFFFFFFUL) {
            set_err(err, errlen, "%s: ZIP64 entries are not supported", path);
            return KSYNC_ERR_ZIP;
        }
        if (e->flags & 0x0001) {
            set_err(err, errlen, "%s: encrypted entries are not supported (%s)", path, e->name);
            return KSYNC_ERR_ZIP;
        }
        if (e->method != 0 && e->method != 8) {
            set_err(err, errlen, "%s: unsupported compression method %u for %s",
                    path, e->method, e->name);
            return KSYNC_ERR_ZIP;
        }
        lo = e->local_off;
        if (lo + 30 > size || rd32(b + lo) != 0x04034b50)
            goto damaged;
        lnlen = rd16(b + lo + 26);
        e->lextra_len = rd16(b + lo + 28);
        e->lextra = b + lo + 30 + lnlen;
        e->data = e->lextra + e->lextra_len;
        if ((size_t)(e->data - b) + e->csize > size)
            goto damaged;
    }
    return KSYNC_OK;

damaged:
    set_err(err, errlen, "%s: damaged zip file", path);
    return KSYNC_ERR_ZIP;
}

static int zip_open(zarchive *z, const char *path, char *err, size_t errlen)
{
    int r;
    memset(z, 0, sizeof(*z));
    if ((r = load_file(path, &z->buf, &z->size, err, errlen)) != KSYNC_OK)
        return r;
    if ((r = zip_parse(z, path, err, errlen)) != KSYNC_OK)
        zip_free(z);
    return r;
}

static int is_dir_entry(const zentry *e)
{
    size_t l = strlen(e->name);
    return l > 0 && e->name[l - 1] == '/';
}

/* Uncompress an entry and check its CRC. *out must be freed. */
static int zip_extract(const zentry *e, unsigned char **out, char *err, size_t errlen)
{
    unsigned char *d = (unsigned char *)malloc(e->usize ? e->usize : 1);

    *out = NULL;
    if (!d) {
        set_err(err, errlen, "out of memory");
        return KSYNC_ERR_NOMEM;
    }
    if (e->method == 0) {
        if (e->csize != e->usize)
            goto bad;
        memcpy(d, e->data, e->usize);
    } else {
        z_stream zs;
        int r;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
            goto bad;
        zs.next_in = (Bytef *)e->data;
        zs.avail_in = e->csize;
        zs.next_out = d;
        zs.avail_out = e->usize;
        r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (r != Z_STREAM_END || zs.total_out != e->usize)
            goto bad;
    }
    if (crc32(0L, d, e->usize) != e->crc)
        goto bad;
    *out = d;
    return KSYNC_OK;

bad:
    free(d);
    set_err(err, errlen, "zip entry %s is damaged (CRC or data error)", e->name);
    return KSYNC_ERR_ZIP;
}

/* ====================================================================== */
/* zip writing                                                            */

typedef struct oentry {
    const zentry        *src;       /* original entry (name, attributes) */
    int                  replaced;  /* 1: data below is new */
    unsigned char       *cdata;     /* new compressed data (owned) */
    uint32_t             csize, usize, crc;
    uint16_t             method, dtime, ddate;
    uint32_t             offset;    /* filled while writing */
} oentry;

static void wr16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int compress_entry(oentry *o, const unsigned char *data, size_t len)
{
    z_stream zs;
    uLong bound;
    unsigned char *out;
    int r;

    o->usize = (uint32_t)len;
    o->crc = (uint32_t)crc32(0L, data, (uInt)len);

    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return KSYNC_ERR_NOMEM;
    bound = deflateBound(&zs, (uLong)len);
    out = (unsigned char *)malloc(bound + 1);
    if (!out) {
        deflateEnd(&zs);
        return KSYNC_ERR_NOMEM;
    }
    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)len;
    zs.next_out = out;
    zs.avail_out = (uInt)bound;
    r = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (r != Z_STREAM_END) {
        free(out);
        return KSYNC_ERR_NOMEM;
    }
    if (zs.total_out < len) {
        o->method = 8;
        o->csize = (uint32_t)zs.total_out;
    } else {                     /* incompressible: store */
        memcpy(out, data, len);
        o->method = 0;
        o->csize = (uint32_t)len;
    }
    o->cdata = out;
    return KSYNC_OK;
}

static int fwrite_all(FILE *fp, const void *p, size_t n, uint32_t *pos)
{
    if (n && fwrite(p, 1, n, fp) != n)
        return 0;
    *pos += (uint32_t)n;
    return 1;
}

static int zip_write(const char *path, const zarchive *z, oentry *o, size_t n,
                     char *err, size_t errlen)
{
    FILE *fp = fopen(path, "wb");
    unsigned char h[46];
    uint32_t pos = 0, cd_start;
    size_t i;

    if (!fp) {
        set_err(err, errlen, "cannot create %s: %s", path, strerror(errno));
        return KSYNC_ERR_IO;
    }
    for (i = 0; i < n; i++) {
        const zentry *s = o[i].src;
        uint16_t nlen = (uint16_t)strlen(s->name);
        uint16_t flags = (uint16_t)(s->flags & ~0x0008);   /* no data descriptor */
        uint16_t needed = o[i].replaced ? (uint16_t)(o[i].method == 8 ? 20 : 10) : s->needed;
        uint16_t xlen = o[i].replaced ? 0 : s->lextra_len;
        const unsigned char *data = o[i].replaced ? o[i].cdata : s->data;

        if (o[i].replaced)
            flags &= 0x0800;                                 /* keep only UTF-8 flag */
        o[i].offset = pos;
        wr32(h, 0x04034b50);
        wr16(h + 4, needed);
        wr16(h + 6, flags);
        wr16(h + 8, o[i].method);
        wr16(h + 10, o[i].dtime);
        wr16(h + 12, o[i].ddate);
        wr32(h + 14, o[i].crc);
        wr32(h + 18, o[i].csize);
        wr32(h + 22, o[i].usize);
        wr16(h + 26, nlen);
        wr16(h + 28, xlen);
        if (!fwrite_all(fp, h, 30, &pos) || !fwrite_all(fp, s->name, nlen, &pos) ||
            !fwrite_all(fp, s->lextra, xlen, &pos) || !fwrite_all(fp, data, o[i].csize, &pos))
            goto ioerr;
    }
    cd_start = pos;
    for (i = 0; i < n; i++) {
        const zentry *s = o[i].src;
        uint16_t nlen = (uint16_t)strlen(s->name);
        uint16_t flags = (uint16_t)(s->flags & ~0x0008);
        uint16_t needed = o[i].replaced ? (uint16_t)(o[i].method == 8 ? 20 : 10) : s->needed;
        uint16_t xlen = o[i].replaced ? 0 : s->cextra_len;

        if (o[i].replaced)
            flags &= 0x0800;
        wr32(h, 0x02014b50);
        wr16(h + 4, s->made_by);
        wr16(h + 6, needed);
        wr16(h + 8, flags);
        wr16(h + 10, o[i].method);
        wr16(h + 12, o[i].dtime);
        wr16(h + 14, o[i].ddate);
        wr32(h + 16, o[i].crc);
        wr32(h + 20, o[i].csize);
        wr32(h + 24, o[i].usize);
        wr16(h + 28, nlen);
        wr16(h + 30, xlen);
        wr16(h + 32, s->comment_len);
        wr16(h + 34, 0);
        wr16(h + 36, s->int_attr);
        wr32(h + 38, s->ext_attr);
        wr32(h + 42, o[i].offset);
        if (!fwrite_all(fp, h, 46, &pos) || !fwrite_all(fp, s->name, nlen, &pos) ||
            !fwrite_all(fp, s->cextra, xlen, &pos) ||
            !fwrite_all(fp, s->comment, s->comment_len, &pos))
            goto ioerr;
    }
    wr32(h, 0x06054b50);
    wr16(h + 4, 0);
    wr16(h + 6, 0);
    wr16(h + 8, (uint16_t)n);
    wr16(h + 10, (uint16_t)n);
    wr32(h + 12, pos - cd_start);
    wr32(h + 16, cd_start);
    wr16(h + 20, z->comment_len);
    if (!fwrite_all(fp, h, 22, &pos) || !fwrite_all(fp, z->comment, z->comment_len, &pos))
        goto ioerr;
    if (fclose(fp) != 0) {
        fp = NULL;
        goto ioerr;
    }
    return KSYNC_OK;

ioerr:
    set_err(err, errlen, "cannot write %s: %s", path, strerror(errno));
    if (fp)
        fclose(fp);
    return KSYNC_ERR_IO;
}

/* Re-read the written zip and check every entry. */
static int zip_verify(const char *path, size_t expected, char *err, size_t errlen)
{
    zarchive z;
    size_t i;
    int r = zip_open(&z, path, err, errlen);
    if (r != KSYNC_OK)
        return r;
    if (z.n != expected) {
        set_err(err, errlen, "verification of %s failed", path);
        zip_free(&z);
        return KSYNC_ERR_ZIP;
    }
    for (i = 0; i < z.n && r == KSYNC_OK; i++) {
        unsigned char *d;
        r = zip_extract(&z.e[i], &d, err, errlen);
        free(d);
    }
    zip_free(&z);
    return r;
}

/* ====================================================================== */
/* filesystem helpers                                                     */

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

static int same_file(const char *a, const char *b)
{
#ifdef _WIN32
    char fa[MAX_PATH], fb[MAX_PATH];
    if (!GetFullPathNameA(a, MAX_PATH, fa, NULL) || !GetFullPathNameA(b, MAX_PATH, fb, NULL))
        return 0;
    return _stricmp(fa, fb) == 0;
#else
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
        return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

static int replace_file(const char *tmp, const char *dst)
{
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp, dst);
#endif
}

/* ====================================================================== */
/* the update                                                             */

void ksync_options_init(ksync_options *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->port = KSYNC_DEFAULT_PORT;
    opt->timeout_ms = 15000;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void warn(const ksync_options *opt, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    if (!opt->on_warning)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    opt->on_warning(msg, opt->user);
}

/* ====================================================================== */
/* Kicklayout comparison                                                  */

typedef struct strlist {
    char  **s;
    size_t  n, cap;
} strlist;

static void strlist_free(strlist *l)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        free(l->s[i]);
    free(l->s);
    memset(l, 0, sizeof(*l));
}

static int strlist_has(const strlist *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (ksync_name_equal(l->s[i], s))
            return 1;
    return 0;
}

/* Adds a copy of s[0..len) unless it is already there (ignoring case). */
static int strlist_add(strlist *l, const char *s, size_t len)
{
    char *c = (char *)malloc(len + 1);
    if (!c)
        return KSYNC_ERR_NOMEM;
    memcpy(c, s, len);
    c[len] = '\0';
    if (strlist_has(l, c)) {
        free(c);
        return KSYNC_OK;
    }
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 64;
        char **ns = (char **)realloc(l->s, ncap * sizeof(*ns));
        if (!ns) {
            free(c);
            return KSYNC_ERR_NOMEM;
        }
        l->s = ns;
        l->cap = ncap;
    }
    l->s[l->n++] = c;
    return KSYNC_OK;
}

static int is_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* The paths of all MODULE lines of a Kicklayout, in order, without
   duplicates. Comment lines (";MODULE ...") don't start with MODULE and are
   skipped that way. */
static int layout_modules(const unsigned char *d, size_t len, strlist *out)
{
    static const char kw[] = "module";
    size_t p = 0;

    while (p < len) {
        size_t e = p, q, t, k;
        while (e < len && d[e] != '\n')
            e++;
        q = p;
        while (q < e && is_blank(d[q]))
            q++;
        t = q;
        while (t < e && !is_blank(d[t]))
            t++;
        if (t - q == sizeof(kw) - 1) {
            for (k = 0; k < t - q && latin1_lower(d[q + k]) == kw[k]; k++)
                ;
            if (k == t - q) {
                size_t a = t, b;
                int r;
                while (a < e && is_blank(d[a]))
                    a++;
                b = a;
                while (b < e && !is_blank(d[b]) && d[b] != ';')
                    b++;
                if (b > a && (r = strlist_add(out, (const char *)d + a, b - a)) != KSYNC_OK)
                    return r;
            }
        }
        p = e + 1;
    }
    return KSYNC_OK;
}

/* Fills only_zip / only_amiga with the MODULE paths found in one copy only. */
static int layout_diff(const unsigned char *zd, size_t zlen,
                       const unsigned char *ad, size_t alen,
                       strlist *only_zip, strlist *only_amiga)
{
    strlist zm = { NULL, 0, 0 }, am = { NULL, 0, 0 };
    size_t i;
    int r;

    if ((r = layout_modules(zd, zlen, &zm)) != KSYNC_OK ||
        (r = layout_modules(ad, alen, &am)) != KSYNC_OK)
        goto out;
    for (i = 0; i < zm.n && r == KSYNC_OK; i++)
        if (!strlist_has(&am, zm.s[i]))
            r = strlist_add(only_zip, zm.s[i], strlen(zm.s[i]));
    for (i = 0; i < am.n && r == KSYNC_OK; i++)
        if (!strlist_has(&zm, am.s[i]))
            r = strlist_add(only_amiga, am.s[i], strlen(am.s[i]));
out:
    strlist_free(&zm);
    strlist_free(&am);
    return r;
}

int ksync_update_zip(const ksync_options *opt, ksync_result *res)
{
    zarchive z;
    oentry *out = NULL;
    ksync_conn *conn = NULL;
    ksync_file_info *remote = NULL;
    size_t nremote = 0, i;
    int have_list = 0, r = KSYNC_OK;
    char *err = res->error;
    size_t errlen = sizeof(res->error);
    char tmp[1100];

    memset(res, 0, sizeof(*res));
    memset(&z, 0, sizeof(z));
    tmp[0] = '\0';

    if (!opt->input_zip || (!opt->conn && (!opt->host || !*opt->host))) {
        set_err(err, errlen, "host and input zip are required");
        return KSYNC_ERR_ARGS;
    }
    if (opt->output_zip && *opt->output_zip)
        copy_str(res->output_path, sizeof(res->output_path), opt->output_zip);
    else
        ksync_default_output_name(res->output_path, sizeof(res->output_path));

    if (!opt->dry_run && file_exists(res->output_path)) {
        if (same_file(res->output_path, opt->input_zip)) {
            set_err(err, errlen, "output would overwrite the input zip");
            return KSYNC_ERR_ARGS;
        }
        if (!opt->force) {
            set_err(err, errlen, "%s exists (use --force to overwrite)", res->output_path);
            return KSYNC_ERR_EXISTS;
        }
    }

    if ((r = zip_open(&z, opt->input_zip, err, errlen)) != KSYNC_OK)
        return r;

    out = (oentry *)calloc(z.n ? z.n : 1, sizeof(*out));
    if (!out) {
        set_err(err, errlen, "out of memory");
        r = KSYNC_ERR_NOMEM;
        goto done;
    }

    if (opt->conn) {
        conn = opt->conn;
    } else {
        conn = ksync_connect(opt->host, opt->port, opt->timeout_ms, err, errlen);
        if (!conn) {
            r = KSYNC_ERR_CONNECT;
            goto done;
        }
    }

    /* Names on the Amiga, to match case-insensitively. If LIST is refused we
       fall back to asking with the zip's spelling (AmigaOS ignores case). */
    r = ksync_list(conn, &remote, &nremote);
    if (r == KSYNC_OK) {
        have_list = 1;
    } else if (r == KSYNC_ERR_SERVER) {
        warn(opt, "server refused LIST (%s), looking up names as spelled in the zip",
             ksync_conn_error(conn));
        r = KSYNC_OK;
    } else {
        set_err(err, errlen, "%s", ksync_conn_error(conn));
        goto done;
    }

    for (i = 0; i < z.n; i++) {
        const zentry *e = &z.e[i];
        oentry *o = &out[i];
        ksync_entry_report rep;
        ksync_file_info info;
        unsigned char *data = NULL, *blob = NULL;
        size_t blen = 0;
        const char *name, *rname = NULL;
        char zipver[KSYNC_VER_MAX];
        strlist only_zip = { NULL, 0, 0 }, only_amiga = { NULL, 0, 0 };
        size_t k;
        int take = 0, layout = 0;

        o->src = e;
        o->method = e->method;
        o->dtime = e->dtime;
        o->ddate = e->ddate;
        o->crc = e->crc;
        o->csize = e->csize;
        o->usize = e->usize;

        if (is_dir_entry(e))
            continue;
        res->entries++;

        for (k = 0; k < i; k++)
            if (!is_dir_entry(&z.e[k]) && ksync_name_equal(z.e[k].name, e->name))
                warn(opt, "%s and %s differ only in case; AmigaOS treats them as the same file",
                     z.e[k].name, e->name);

        if ((r = zip_extract(e, &data, err, errlen)) != KSYNC_OK)
            goto done;

        memset(&rep, 0, sizeof(rep));
        rep.entry_name = e->name;
        ksync_find_verstring(data, e->usize, zipver, sizeof(zipver));
        ksync_parse_version(zipver, &rep.zip_version);

        name = base_name(e->name);
        rep.is_kicklayout = ksync_name_equal(name, "Kicklayout");
        if (have_list) {
            for (k = 0; k < nremote; k++)
                if (ksync_name_equal(remote[k].name, name)) {
                    rname = remote[k].name;
                    break;
                }
        } else {
            rname = name;
        }

        r = rname ? ksync_info(conn, rname, &info) : KSYNC_NOT_FOUND;
        if (r == KSYNC_NOT_FOUND || r == KSYNC_ERR_SERVER) {
            if (r == KSYNC_ERR_SERVER)
                warn(opt, "%s: %s", e->name, ksync_conn_error(conn));
            r = KSYNC_OK;
            rep.action = KSYNC_ACT_KEPT_NOT_ON_AMIGA;
        } else if (r != KSYNC_OK) {
            set_err(err, errlen, "%s", ksync_conn_error(conn));
            free(data);
            goto done;
        } else {
            int cmp;
            rep.amiga_name = rname;
            rep.renamed = strcmp(rname, name) != 0;
            ksync_parse_version(info.ver, &rep.amiga_version);
            cmp = ksync_compare_version(&rep.amiga_version, &rep.zip_version);

            if (rep.is_kicklayout) {
                layout = 1;          /* never by version: decided below */
            } else if (rep.amiga_version.n == 0) {
                take = 1;
                rep.reason = KSYNC_REASON_NO_AMIGA_VERSION;
            } else if (rep.zip_version.n == 0) {
                take = 1;
                rep.reason = KSYNC_REASON_NO_ZIP_VERSION;
            } else if (cmp > 0) {
                take = 1;
                rep.reason = KSYNC_REASON_NEWER;
            } else if (cmp == 0) {
                rep.action = KSYNC_ACT_SAME;
                rep.size_differs = info.size != e->usize;
            } else {
                rep.action = KSYNC_ACT_KEPT_ZIP_NEWER;
            }
        }

        /* Download the Amiga copy when it goes into the zip, when it may be
           identical to the zip copy (same size), or to compare Kicklayouts. */
        if ((take || layout) && (layout || info.size == e->usize || !opt->dry_run)) {
            char gotver[KSYNC_VER_MAX];
            ksync_version gv;

            r = ksync_get(conn, rname, &blob, &blen);
            if (r != KSYNC_OK) {
                set_err(err, errlen, "downloading %s: %s", rname, ksync_conn_error(conn));
                if (r == KSYNC_NOT_FOUND)
                    r = KSYNC_ERR_SERVER;
                free(data);
                goto done;
            }
            ksync_find_verstring(blob, blen, gotver, sizeof(gotver));
            ksync_parse_version(gotver, &gv);
            if (ksync_compare_version(&gv, &rep.amiga_version) != 0) {
                set_err(err, errlen, "%s changed on the Amiga while downloading", rname);
                r = KSYNC_ERR_PROTOCOL;
                free(blob);
                free(data);
                goto done;
            }
            if (blen == e->usize && memcmp(blob, data, blen) == 0) {
                take = layout = 0;
                rep.action = KSYNC_ACT_SAME;
                rep.reason = KSYNC_REASON_NONE;
                rep.identical = 1;
            } else if (layout) {
                r = layout_diff(data, e->usize, blob, blen, &only_zip, &only_amiga);
                if (r != KSYNC_OK) {
                    set_err(err, errlen, "out of memory");
                    free(blob);
                    free(data);
                    strlist_free(&only_zip);
                    strlist_free(&only_amiga);
                    goto done;
                }
                rep.layout_only_zip = (const char *const *)only_zip.s;
                rep.layout_only_zip_count = only_zip.n;
                rep.layout_only_amiga = (const char *const *)only_amiga.s;
                rep.layout_only_amiga_count = only_amiga.n;
                if (opt->take_kicklayout) {
                    take = 1;
                    rep.reason = KSYNC_REASON_TAKE_KICKLAYOUT;
                } else {
                    rep.action = KSYNC_ACT_KEPT_KICKLAYOUT;
                }
            }
        }
        free(data);

        if (take) {
            rep.action = KSYNC_ACT_UPDATED;
            res->updated++;
            if (!opt->dry_run) {
                r = compress_entry(o, blob, blen);
                if (r != KSYNC_OK) {
                    set_err(err, errlen, "compressing %s failed", e->name);
                    free(blob);
                    strlist_free(&only_zip);
                    strlist_free(&only_amiga);
                    goto done;
                }
                o->replaced = 1;     /* name stays e->name: the zip's spelling */
                amiga_to_dos(info.date, &o->dtime, &o->ddate);
            }
        }
        free(blob);
        if (opt->on_entry)
            opt->on_entry(&rep, opt->user);
        strlist_free(&only_zip);
        strlist_free(&only_amiga);
    }

    if (opt->dry_run)
        goto done;

    if (z.n > 0xFFFF) {
        set_err(err, errlen, "too many entries");
        r = KSYNC_ERR_ZIP;
        goto done;
    }
    snprintf(tmp, sizeof(tmp), "%s.kstmp", res->output_path);
    if ((r = zip_write(tmp, &z, out, z.n, err, errlen)) != KSYNC_OK)
        goto done;
    if ((r = zip_verify(tmp, z.n, err, errlen)) != KSYNC_OK)
        goto done;
    if (replace_file(tmp, res->output_path) != 0) {
        set_err(err, errlen, "cannot create %s: %s", res->output_path, strerror(errno));
        r = KSYNC_ERR_IO;
        goto done;
    }
    tmp[0] = '\0';

done:
    if (tmp[0])
        remove(tmp);
    if (out) {
        for (i = 0; i < z.n; i++)
            free(out[i].cdata);
        free(out);
    }
    free(remote);
    if (conn && conn != opt->conn)
        ksync_close(conn);
    zip_free(&z);
    return r;
}
