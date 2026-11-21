/*
 * KickServ - AmigaOS 4 commodity that serves Kickstart modules over TCP/IP.
 *
 * The host-side tool "KickSync" connects to this commodity, asks for the
 * version of every module found in a Kickstart.zip and downloads the ones
 * that are newer on the Amiga side.
 *
 * Tool types (icon) / CLI arguments:
 *   PORT=<n>          TCP port to listen on            (default 6800)
 *   DIR=<path>        directory to serve               (default SYS:Kickstart)
 *   ALLOW=<list>      comma separated IPv4 addresses or networks
 *                     (a.b.c.d or a.b.c.d/bits) that may connect,
 *                     ANY or * allows everybody         (default ANY)
 *   CX_PRIORITY=<n>   commodity priority               (default 0)
 *   QUIET             no log output when started from a Shell
 *
 * Protocol (line based, fields separated by TAB, lines end with LF):
 *   server greeting:   KICKSERV <TAB> 1
 *   LIST            -> OK, then "<size>\t<date>\t<name>\t<$VER>" lines, then "."
 *   INFO <name>     -> OK\t<size>\t<date>\t<$VER>        or ERR\t<message>
 *   GET <name>      -> OK\t<size>\n followed by <size> raw bytes, or ERR\t<msg>
 *   QUIT            -> BYE (connection closed)
 *   <date> is seconds since 1978-01-01 00:00:00 Amiga local time.
 *
 * Build: ppc-amigaos-gcc -O2 -Wall -D__USE_INLINE__ -o KickServ kickserv.c -lauto
 */

#include <exec/types.h>
#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/exall.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <libraries/commodities.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/commodities.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/bsdsocket.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#if defined(__has_include)
#if __has_include(<sys/select.h>)
#include <sys/select.h>
#endif
#endif

#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#define KS_NAME        "KickServ"
#define KS_VERSION     "1.0"
#define KS_DATE        "21.11.2026"
#define KS_PROTO       1

#define DEFAULT_PORT   6800
#define DEFAULT_DIR    "SYS:Kickstart"

#define MAX_CLIENTS    4
#define MAX_ALLOW      8
#define LINE_LEN       512
#define VER_LEN        200
#define PATH_LEN       1024
#define IO_CHUNK       32768
#define VER_OVERLAP    (VER_LEN + 8)

static const char __attribute__((used)) verstag[] =
    "\0$VER: " KS_NAME " " KS_VERSION " (" KS_DATE ")";

struct Library   *SocketBase = NULL;
struct SocketIFace *ISocket  = NULL;

struct AllowEntry {
    ULONG addr;   /* host byte order */
    ULONG mask;
};

struct Config {
    LONG  port;
    char  dir[PATH_LEN];
    LONG  pri;
    BOOL  quiet;
    int   nallow;          /* 0 = everybody */
    struct AllowEntry allow[MAX_ALLOW];
};

struct Client {
    LONG  sock;
    ULONG addr;            /* host byte order */
    int   len;
    char  buf[LINE_LEN];
};

static struct Config cfg;
static struct Client clients[MAX_CLIENTS];
static LONG   listen_sock = -1;
static BOOL   from_wb     = FALSE;
static UBYTE  iobuf[IO_CHUNK + VER_OVERLAP];

/* ------------------------------------------------------------------------ */
/* logging / error reporting                                                 */

static void log_msg(CONST_STRPTR fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(CONST_STRPTR fmt, ...)
{
    char line[256];
    va_list ap;

    if (from_wb || cfg.quiet)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    PutStr(line);
    PutStr("\n");
}

static void fatal(CONST_STRPTR msg)
{
    if (from_wb) {
        struct EasyStruct es = {
            sizeof(struct EasyStruct), 0,
            KS_NAME, (STRPTR)"%s", (STRPTR)"OK", NULL, NULL
        };
        EasyRequest(NULL, &es, NULL, msg);
    } else {
        PutStr(KS_NAME ": ");
        PutStr(msg);
        PutStr("\n");
    }
}

static void fmt_ip(ULONG a, char *out, int outlen)
{
    snprintf(out, outlen, "%lu.%lu.%lu.%lu",
             (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

/* ------------------------------------------------------------------------ */
/* configuration                                                              */

static BOOL parse_ipv4(const char *s, ULONG *out)
{
    ULONG v = 0;
    int part;

    for (part = 0; part < 4; part++) {
        ULONG n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            if (++digits > 3 || n > 255)
                return FALSE;
        }
        if (digits == 0)
            return FALSE;
        v = (v << 8) | n;
        if (part < 3) {
            if (*s++ != '.')
                return FALSE;
        }
    }
    if (*s != '\0')
        return FALSE;
    *out = v;
    return TRUE;
}

static void parse_allow(const char *list)
{
    char tmp[256];
    char *p, *tok;

    cfg.nallow = 0;
    strncpy(tmp, list, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (p = tmp; p && *p; ) {
        char *slash;
        LONG bits = 32;
        ULONG addr;

        tok = p;
        p = strpbrk(p, ",|");
        if (p)
            *p++ = '\0';
        while (*tok == ' ')
            tok++;
        {
            char *e = tok + strlen(tok);
            while (e > tok && e[-1] == ' ')
                *--e = '\0';
        }
        if (*tok == '\0')
            continue;
        if (Stricmp(tok, "ANY") == 0 || strcmp(tok, "*") == 0) {
            cfg.nallow = 0;
            return;
        }
        slash = strchr(tok, '/');
        if (slash) {
            *slash++ = '\0';
            if (StrToLong(slash, &bits) <= 0 || bits < 0 || bits > 32) {
                log_msg("Ignoring invalid ALLOW entry \"%s\"", tok);
                continue;
            }
        }
        if (!parse_ipv4(tok, &addr)) {
            log_msg("Ignoring invalid ALLOW entry \"%s\"", tok);
            continue;
        }
        if (cfg.nallow < MAX_ALLOW) {
            ULONG mask = bits == 0 ? 0 : (0xFFFFFFFFUL << (32 - bits));
            cfg.allow[cfg.nallow].addr = addr & mask;
            cfg.allow[cfg.nallow].mask = mask;
            cfg.nallow++;
        }
    }
}

static void apply_tooltypes(struct DiskObject *dobj)
{
    STRPTR *tt = (STRPTR *)dobj->do_ToolTypes;
    STRPTR s;
    LONG v;

    if (!tt)
        return;
    if ((s = FindToolType(tt, "PORT")) && StrToLong(s, &v) > 0)
        cfg.port = v;
    if ((s = FindToolType(tt, "DIR")) && *s) {
        strncpy(cfg.dir, s, sizeof(cfg.dir) - 1);
        cfg.dir[sizeof(cfg.dir) - 1] = '\0';
    }
    if ((s = FindToolType(tt, "CX_PRIORITY")) && StrToLong(s, &v) > 0)
        cfg.pri = v;
    if ((s = FindToolType(tt, "ALLOW")))
        parse_allow(s);
    if (FindToolType(tt, "QUIET"))
        cfg.quiet = TRUE;
}

static void read_icon_wb(struct WBStartup *wbs)
{
    struct WBArg *wa = wbs->sm_ArgList;
    BPTR old = SetCurrentDir(wa->wa_Lock);
    struct DiskObject *dobj = GetDiskObject(wa->wa_Name);
    SetCurrentDir(old);

    if (dobj) {
        apply_tooltypes(dobj);
        FreeDiskObject(dobj);
    }
}

/* When started from a Shell the icon's tool types act as defaults. */
static void read_icon_cli(void)
{
    char name[256];
    char path[300];
    struct DiskObject *dobj;

    if (!GetCliProgramName(name, sizeof(name)))
        return;
    snprintf(path, sizeof(path), "PROGDIR:%s", (char *)FilePart(name));
    if ((dobj = GetDiskObject(path))) {
        apply_tooltypes(dobj);
        FreeDiskObject(dobj);
    }
}

static BOOL read_cli_args(void)
{
    enum { A_PORT, A_DIR, A_PRI, A_ALLOW, A_QUIET, A_COUNT };
    int32 args[A_COUNT] = { 0 };
    struct RDArgs *rda;

    rda = ReadArgs("PORT/K/N,DIR/K,CX_PRIORITY/K/N,ALLOW/K,QUIET/S", args, NULL);
    if (!rda) {
        PrintFault(IoErr(), KS_NAME);
        return FALSE;
    }
    if (args[A_PORT])
        cfg.port = *(int32 *)args[A_PORT];
    if (args[A_DIR]) {
        strncpy(cfg.dir, (char *)args[A_DIR], sizeof(cfg.dir) - 1);
        cfg.dir[sizeof(cfg.dir) - 1] = '\0';
    }
    if (args[A_PRI])
        cfg.pri = *(int32 *)args[A_PRI];
    if (args[A_ALLOW])
        parse_allow((char *)args[A_ALLOW]);
    if (args[A_QUIET])
        cfg.quiet = TRUE;
    FreeArgs(rda);
    return TRUE;
}

static BOOL is_allowed(ULONG addr)
{
    int i;
    if (cfg.nallow == 0)
        return TRUE;
    for (i = 0; i < cfg.nallow; i++)
        if ((addr & cfg.allow[i].mask) == cfg.allow[i].addr)
            return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* file helpers                                                               */

/* Reject anything that could escape the served directory. */
static BOOL valid_name(const char *name)
{
    size_t len = strlen(name);
    const char *p;

    if (len == 0 || len > 255)
        return FALSE;
    if (name[0] == '/' || name[len - 1] == '/')
        return FALSE;
    if (strchr(name, ':') || strstr(name, "//"))
        return FALSE;
    for (p = name; *p; p++)
        if ((unsigned char)*p < 32)
            return FALSE;
    return TRUE;
}

static BOOL make_path(const char *name, char *path, int pathlen)
{
    Strlcpy(path, cfg.dir, pathlen);
    return AddPart(path, name, pathlen) ? TRUE : FALSE;
}

static ULONG ds_to_secs(const struct DateStamp *ds)
{
    return (ULONG)ds->ds_Days * 86400UL + (ULONG)ds->ds_Minute * 60UL +
           (ULONG)ds->ds_Tick / TICKS_PER_SECOND;
}

/* Find the first "$VER:" string in a file. Returns "" if none found. */
static void find_version(const char *path, char *out, int outlen)
{
    BPTR fh;
    LONG keep = 0;
    BOOL eof = FALSE;

    out[0] = '\0';
    if (!(fh = Open(path, MODE_OLDFILE)))
        return;

    for (;;) {
        LONG n = 0, total, limit, i;

        if (!eof) {
            n = Read(fh, iobuf + keep, IO_CHUNK);
            if (n < 0)
                break;
            if (n == 0)
                eof = TRUE;
        }
        total = keep + n;
        /* Only start matches where the whole string is guaranteed to be
           in the buffer, unless we are at the end of the file. */
        limit = eof ? total : total - VER_OVERLAP;
        if (limit < 0)
            limit = 0;

        for (i = 0; i < limit && i + 5 <= total; i++) {
            if (iobuf[i] == '$' && memcmp(iobuf + i, "$VER:", 5) == 0) {
                LONG j = i + 5;
                int o = 0;
                while (j < total && iobuf[j] == ' ')
                    j++;
                while (j < total && o < outlen - 1 && o < VER_LEN) {
                    UBYTE c = iobuf[j++];
                    if (c == 0 || c == '\n' || c == '\r')
                        break;
                    out[o++] = (c == '\t' || c < 32) ? ' ' : (char)c;
                }
                out[o] = '\0';
                Close(fh);
                return;
            }
        }
        if (eof)
            break;
        keep = total - limit;
        memmove(iobuf, iobuf + limit, keep);
    }
    Close(fh);
}

/* Returns TRUE if path is a regular file and fills size/date. */
static BOOL stat_file(const char *path, ULONG *size, ULONG *date)
{
    struct ExamineData *ed;
    BOOL ok = FALSE;

    ed = ExamineObjectTags(EX_StringNameInput, path, TAG_END);
    if (ed) {
        if (EXD_IS_FILE(ed)) {
            *size = (ULONG)ed->FileSize;
            *date = ds_to_secs(&ed->Date);
            ok = TRUE;
        }
        FreeDosObject(DOS_EXAMINEDATA, ed);
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* networking                                                                 */

static BOOL send_all(LONG s, const void *data, LONG len)
{
    const char *p = data;
    while (len > 0) {
        LONG n = send(s, (APTR)p, len, 0);
        if (n <= 0)
            return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

static BOOL send_line(LONG s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static BOOL send_line(LONG s, const char *fmt, ...)
{
    char line[LINE_LEN + VER_LEN + 64];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return FALSE;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    return send_all(s, line, n);
}

static LONG open_listener(LONG port)
{
    struct sockaddr_in sa;
    LONG s, one = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(s, 4) < 0) {
        CloseSocket(s);
        return -1;
    }
    return s;
}

static void close_client(struct Client *c)
{
    if (c->sock >= 0) {
        char ip[20];
        fmt_ip(c->addr, ip, sizeof(ip));
        log_msg("Disconnected %s", ip);
        CloseSocket(c->sock);
    }
    c->sock = -1;
    c->len = 0;
}

static void accept_client(void)
{
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    LONG s;
    ULONG addr;
    char ip[20];
    int i;

    s = accept(listen_sock, (struct sockaddr *)&sa, &salen);
    if (s < 0)
        return;
    addr = ntohl(sa.sin_addr.s_addr);
    fmt_ip(addr, ip, sizeof(ip));

    if (!is_allowed(addr)) {
        log_msg("Rejected connection from %s (not in ALLOW)", ip);
        send_line(s, "ERR\taccess denied");
        CloseSocket(s);
        return;
    }
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].sock < 0)
            break;
    if (i == MAX_CLIENTS) {
        log_msg("Rejected connection from %s (too many clients)", ip);
        send_line(s, "ERR\tbusy");
        CloseSocket(s);
        return;
    }

    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    clients[i].sock = s;
    clients[i].addr = addr;
    clients[i].len = 0;
    log_msg("Connection from %s", ip);
    if (!send_line(s, "KICKSERV\t%d", KS_PROTO))
        close_client(&clients[i]);
}

/* ------------------------------------------------------------------------ */
/* protocol                                                                   */

static BOOL cmd_list(LONG s)
{
    APTR ctx;
    struct ExamineData *ed;
    char path[PATH_LEN];
    char ver[VER_LEN + 1];
    BOOL ok = TRUE;

    ctx = ObtainDirContextTags(EX_StringNameInput, cfg.dir,
                               EX_DataFields, EXF_NAME | EXF_SIZE | EXF_DATE | EXF_TYPE,
                               TAG_END);
    if (!ctx)
        return send_line(s, "ERR\tcannot read directory %s", cfg.dir);

    if (!send_line(s, "OK")) {
        ReleaseDirContext(ctx);
        return FALSE;
    }
    while (ok && (ed = ExamineDir(ctx))) {
        if (!EXD_IS_FILE(ed) || !valid_name(ed->Name))
            continue;
        if (!make_path(ed->Name, path, sizeof(path)))
            continue;
        find_version(path, ver, sizeof(ver));
        ok = send_line(s, "%lu\t%lu\t%s\t%s", (ULONG)ed->FileSize,
                       ds_to_secs(&ed->Date), ed->Name, ver);
    }
    ReleaseDirContext(ctx);
    return ok && send_line(s, ".");
}

static BOOL cmd_info(LONG s, const char *name)
{
    char path[PATH_LEN];
    char ver[VER_LEN + 1];
    ULONG size, date;

    if (!valid_name(name) || !make_path(name, path, sizeof(path)))
        return send_line(s, "ERR\tinvalid name");
    if (!stat_file(path, &size, &date))
        return send_line(s, "ERR\tnot found");
    find_version(path, ver, sizeof(ver));
    return send_line(s, "OK\t%lu\t%lu\t%s", size, date, ver);
}

static BOOL cmd_get(LONG s, const char *name)
{
    char path[PATH_LEN];
    ULONG size, date, left;
    BPTR fh;

    if (!valid_name(name) || !make_path(name, path, sizeof(path)))
        return send_line(s, "ERR\tinvalid name");
    if (!stat_file(path, &size, &date) || !(fh = Open(path, MODE_OLDFILE)))
        return send_line(s, "ERR\tnot found");

    if (!send_line(s, "OK\t%lu", size)) {
        Close(fh);
        return FALSE;
    }
    for (left = size; left > 0; ) {
        LONG want = left > IO_CHUNK ? IO_CHUNK : (LONG)left;
        LONG n = Read(fh, iobuf, want);
        if (n <= 0 || !send_all(s, iobuf, n)) {
            /* We promised <size> bytes; the only safe thing is to hang up. */
            Close(fh);
            return FALSE;
        }
        left -= n;
    }
    Close(fh);
    log_msg("Sent %s (%lu bytes)", name, size);
    return TRUE;
}

/* Returns FALSE if the connection should be closed. */
static BOOL handle_line(struct Client *c, char *line)
{
    char *arg = strchr(line, ' ');

    if (arg)
        *arg++ = '\0';
    else
        arg = "";

    if (Stricmp(line, "LIST") == 0)
        return cmd_list(c->sock);
    if (Stricmp(line, "INFO") == 0)
        return cmd_info(c->sock, arg);
    if (Stricmp(line, "GET") == 0)
        return cmd_get(c->sock, arg);
    if (Stricmp(line, "PING") == 0)
        return send_line(c->sock, "OK");
    if (Stricmp(line, "QUIT") == 0) {
        send_line(c->sock, "BYE");
        return FALSE;
    }
    if (line[0] == '\0')
        return TRUE;
    return send_line(c->sock, "ERR\tunknown command");
}

static void read_client(struct Client *c)
{
    LONG n;
    char *nl;

    n = recv(c->sock, c->buf + c->len, sizeof(c->buf) - 1 - c->len, 0);
    if (n <= 0) {
        close_client(c);
        return;
    }
    c->len += n;
    c->buf[c->len] = '\0';

    while (c->sock >= 0 && (nl = memchr(c->buf, '\n', c->len))) {
        int linelen = nl - c->buf;
        *nl = '\0';
        if (linelen > 0 && c->buf[linelen - 1] == '\r')
            c->buf[linelen - 1] = '\0';
        if (!handle_line(c, c->buf)) {
            close_client(c);
            return;
        }
        c->len -= linelen + 1;
        memmove(c->buf, nl + 1, c->len);
        c->buf[c->len] = '\0';
    }
    if (c->sock >= 0 && c->len >= (int)sizeof(c->buf) - 1) {
        send_line(c->sock, "ERR\tline too long");
        close_client(c);
    }
}

/* ------------------------------------------------------------------------ */
/* main                                                                       */

int main(int argc, char **argv)
{
    struct MsgPort *cxport = NULL;
    CxObj *broker = NULL;
    BOOL running = TRUE;
    BOOL enabled = TRUE;
    int rc = RETURN_FAIL;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.port = DEFAULT_PORT;
    strcpy(cfg.dir, DEFAULT_DIR);
    for (i = 0; i < MAX_CLIENTS; i++)
        clients[i].sock = -1;

    if (argc == 0) {
        from_wb = TRUE;
        read_icon_wb((struct WBStartup *)argv);
    } else {
        read_icon_cli();
        if (!read_cli_args())
            return RETURN_FAIL;
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        fatal("Invalid PORT");
        return RETURN_FAIL;
    }

    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (SocketBase)
        ISocket = (struct SocketIFace *)GetInterface(SocketBase, "main", 1, NULL);
    if (!ISocket) {
        fatal("Cannot open bsdsocket.library (is the TCP/IP stack running?)");
        goto out;
    }

    cxport = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!cxport) {
        fatal("Out of memory");
        goto out;
    }

    {
        struct NewBroker nb;
        LONG err = 0;

        memset(&nb, 0, sizeof(nb));
        nb.nb_Version = NB_VERSION;
        nb.nb_Name    = (STRPTR)KS_NAME;
        nb.nb_Title   = (STRPTR)KS_NAME " " KS_VERSION;
        nb.nb_Descr   = (STRPTR)"Serves Kickstart modules via TCP/IP";
        nb.nb_Unique  = NBU_UNIQUE | NBU_NOTIFY;
        nb.nb_Flags   = 0;
        nb.nb_Pri     = (BYTE)cfg.pri;
        nb.nb_Port    = cxport;

        broker = CxBroker(&nb, &err);
        if (!broker) {
            if (err != CBERR_DUP)   /* already running: exit quietly */
                fatal("Cannot create commodity broker");
            else
                rc = RETURN_OK;
            goto out;
        }
    }

    listen_sock = open_listener(cfg.port);
    if (listen_sock < 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Cannot listen on TCP port %ld", (long)cfg.port);
        fatal(msg);
        goto out;
    }

    ActivateCxObj(broker, TRUE);
    log_msg(KS_NAME " " KS_VERSION ": serving %s on port %ld%s", cfg.dir,
            (long)cfg.port, cfg.nallow ? " (restricted by ALLOW)" : "");

    while (running) {
        fd_set rfds;
        LONG maxfd = -1, n;
        ULONG cxsig = 1UL << cxport->mp_SigBit;
        ULONG sigs = cxsig | SIGBREAKF_CTRL_C;
        CxMsg *msg;

        FD_ZERO(&rfds);
        if (listen_sock >= 0) {
            FD_SET(listen_sock, &rfds);
            maxfd = listen_sock;
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].sock >= 0) {
                FD_SET(clients[i].sock, &rfds);
                if (clients[i].sock > maxfd)
                    maxfd = clients[i].sock;
            }
        }

        n = WaitSelect(maxfd + 1, &rfds, NULL, NULL, NULL, &sigs);
        if (n < 0) {
            fatal("WaitSelect() failed");
            break;
        }

        if (sigs & SIGBREAKF_CTRL_C)
            running = FALSE;

        if (sigs & cxsig) {
            while ((msg = (CxMsg *)GetMsg(cxport))) {
                ULONG id = CxMsgID(msg);
                ULONG type = CxMsgType(msg);
                ReplyMsg((struct Message *)msg);
                if (type != CXM_COMMAND)
                    continue;
                switch (id) {
                case CXCMD_DISABLE:
                    if (enabled) {
                        ActivateCxObj(broker, FALSE);
                        if (listen_sock >= 0) {
                            CloseSocket(listen_sock);
                            listen_sock = -1;
                        }
                        enabled = FALSE;
                        log_msg("Disabled");
                    }
                    break;
                case CXCMD_ENABLE:
                    if (!enabled) {
                        listen_sock = open_listener(cfg.port);
                        if (listen_sock < 0) {
                            fatal("Cannot listen on TCP port, staying disabled");
                        } else {
                            ActivateCxObj(broker, TRUE);
                            enabled = TRUE;
                            log_msg("Enabled");
                        }
                    }
                    break;
                case CXCMD_KILL:
                    running = FALSE;
                    break;
                default:   /* CXCMD_UNIQUE, APPEAR, DISAPPEAR: no GUI */
                    break;
                }
            }
        }

        if (n > 0 && running) {
            if (listen_sock >= 0 && FD_ISSET(listen_sock, &rfds))
                accept_client();
            for (i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].sock >= 0 && FD_ISSET(clients[i].sock, &rfds))
                    read_client(&clients[i]);
        }
    }

    log_msg(KS_NAME " exiting");
    rc = RETURN_OK;

out:
    for (i = 0; i < MAX_CLIENTS; i++)
        close_client(&clients[i]);
    if (listen_sock >= 0)
        CloseSocket(listen_sock);
    if (broker)
        DeleteCxObjAll(broker);
    if (cxport) {
        struct Message *m;
        while ((m = GetMsg(cxport)))
            ReplyMsg(m);
        FreeSysObject(ASOT_PORT, cxport);
    }
    if (ISocket)
        DropInterface((struct Interface *)ISocket);
    if (SocketBase)
        CloseLibrary(SocketBase);
    return rc;
}
