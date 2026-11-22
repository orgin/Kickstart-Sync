# Kickstart Sync

KickServ (AmigaOS 4 commodity) and KickSync (host program).

Update a `Kickstart.zip` (as used to boot AmigaOS 4 under QEMU, e.g. with
BBoot) with the newest Kickstart modules from the running AmigaOS 4 guest.

```
 Host                                     AmigaOS 4 (QEMU guest)
 KickSync     ── TCP ──────────────────▶ KickServ commodity
   reads Kickstart.zip                       serves SYS:Kickstart/
   asks INFO <name> for every entry          reports size, date, $VER
   GETs the ones that are newer              sends file contents
   writes Kickstart_<date>.zip
```

## AmigaOS side: `amiga/KickServ`

Build (needs the OS4 cross compiler, or Docker):

    cd amiga
    make            # with ppc-amigaos-gcc in PATH
    make docker     # or: uses walkero/amigagccondocker:os4-gcc11

Copy `KickServ` to e.g. `SYS:Utilities/Commodities/` and give it a tool icon
(copy any other commodity's icon to `KickServ.info`). To start it at boot,
put it in `SYS:WBStartup/` (add the `DONOTWAIT` tool type).

| Tool type / CLI argument | Default         | Meaning |
|--------------------------|-----------------|---------|
| `PORT=<n>`               | `6800`          | TCP port to listen on |
| `DIR=<path>`             | `SYS:Kickstart` | Directory whose files are served |
| `ALLOW=<list>`           | `ANY`           | Comma separated IPs/networks allowed to connect, e.g. `10.0.2.2` or `192.168.1.0/24` |
| `CX_PRIORITY=<n>`        | `0`             | Commodity priority |
| `QUIET`                  | off             | No log output when run from a Shell |

From a Shell the same keywords work as arguments (`KickServ PORT=7000`) and
override the icon's tool types. It appears in Exchange: *Disable* stops
listening (frees the port), *Enable* resumes, *Remove* quits. Ctrl-C quits too.

Only plain file names inside `DIR` are served; names with `:`, leading `/`
or `//` are rejected so nothing outside that directory can be read. The
server has no authentication, so use `ALLOW` if the guest is reachable from
a network you don't trust.

## Host side: `host/` (C)

| File          | Purpose |
|---------------|---------|
| `kicksync.h` / `kicksync.c` | Library: KickServ client, version logic, zip reading/writing. Add both to any program. |
| `main.c`      | The `KickSync` command: argument parsing and output. |
| `test.sh`     | Tests against `kickserv_mock.py` (`make test`). |

Build (needs a C99 compiler and zlib):

    cd host
    make                     # Linux/macOS/BSD
    make CC=x86_64-w64-mingw32-gcc EXE=.exe LDLIBS="-lz -lws2_32"   # Windows

Usage:

    KickSync [options] HOST[:PORT] Kickstart.zip [New_Kickstart.zip]
    KickSync --list HOST[:PORT]

* Without an output path, `Kickstart_<YYYY-MM-DD>.zip` is created in the
  current directory. An existing output file is not overwritten unless you
  pass `--force`; the input zip is never overwritten. The new zip is written
  to a temporary file, verified (CRC of every entry), then renamed.
* Each zip entry is looked up on the Amiga by file name (any directory part
  inside the zip, like `Kickstart/`, is ignored when looking it up and kept in
  the new zip).
* Names are matched without regard to upper/lower case, as on AmigaOS
  (`Kernel` in the zip matches `kernel` on the Amiga). The new zip always
  uses the spelling from the original zip; a downloaded file with different
  case is renamed and reported as `(Amiga: <name>, renamed)`.
* The Amiga copy is taken **unless** both copies have a `$VER:` version and
  the Amiga one is not newer. So a file without `$VER:` on either side is
  taken from the Amiga, unless it is byte for byte the same as the zip copy:
  then it is reported as `same (identical)` and not counted as updated
  (when the sizes match, KickSync downloads the file to compare, also with
  `--dry-run`).
* `Kicklayout` is **never replaced**, whatever its `$VER:` says. It lists
  the modules for one particular machine: the one in a `Kickstart.zip` for
  QEMU loads `siliconmotion502.chip` for the emulated SM501 graphics, for
  example, while `SYS:Kickstart/Kicklayout` on the Amiga may be meant for
  other hardware. If the two copies differ, KickSync keeps the zip's copy
  and lists the `MODULE` lines found in only one of them (ignoring
  upper/lower case and `;` comment lines), so you can merge by hand.
  `-k/--take-kicklayout` takes the Amiga's copy instead.
* Unchanged entries are copied byte for byte, in the original order.
* Modules that exist only on the Amiga are not added. If an update installed
  new modules, the Kicklayout notes show the new `MODULE` lines; add the
  modules to the zip and to its Kicklayout yourself.

Options: `-n/--dry-run` (just show the table), `-l/--list` (list what the
server offers), `-f/--force`, `-k/--take-kicklayout`, `-t/--timeout SECONDS`.

Example output:

```
Entry                        Zip        Amiga      Result
Kickstart/Kernel             54.30      54.46      UPDATED (Amiga: kernel, renamed)
Kickstart/EXEC.library.kmod  54.10      54.9       kept (zip is newer)
Kickstart/dos.library.kmod   54.100     54.100     same
Kickstart/only.zip.kmod      1.0        -          kept (not on Amiga)
Kickstart/nover.zip.kmod     ?          2.1        UPDATED (no version in zip)
Kickstart/diskboot.kmod      ?          ?          same (identical)
Kickstart/Kicklayout         53.3       53.3       kept (Kicklayout differs, see below)

Kickstart/Kicklayout: the Amiga's copy (Kicklayout) differs; the zip's copy was kept.
  only in the zip's copy:    MODULE Kickstart/siliconmotion502.chip
  Merge by hand if needed, or use --take-kicklayout to take the Amiga's copy.

2 module(s) updated, written to Kickstart_2026-11-22.zip
```

### Using the library in your own program

```c
#include "kicksync.h"

ksync_options opt;
ksync_result  res;

ksync_options_init(&opt);
opt.host      = "localhost";
opt.port      = 6800;
opt.input_zip = "Kickstart.zip";
opt.output_zip = "New.zip";          /* NULL: Kickstart_<date>.zip */
opt.on_entry  = my_callback;         /* optional: per-entry report */
if (ksync_update_zip(&opt, &res) != KSYNC_OK)
    fprintf(stderr, "%s\n", res.error);
```

Compile `kicksync.c` with your sources and link `-lz` (plus `-lws2_32` on
Windows). The lower-level calls (`ksync_connect`, `ksync_list`, `ksync_info`,
`ksync_get`, `ksync_close`) and the version helpers are also public; see the
comments in `kicksync.h`. The library has no global state, so separate
connections can be used from separate threads.

### Reaching the guest from the host

With QEMU user networking, forward a host port into the guest:

    qemu-system-ppc ... -nic user,model=rtl8139,hostfwd=tcp:127.0.0.1:6800-:6800

and run `KickSync localhost:6800 Kickstart.zip`. Connections then come
from `10.0.2.2` inside the guest (use `ALLOW=10.0.2.2` to restrict access).
With tap/bridged networking, use the guest's IP address directly.

`host/kickserv_mock.py DIR [PORT]` serves a local directory using the same
protocol, so you can try KickSync without an Amiga (`make test` uses it).

## Protocol

Plain text lines, fields separated by TAB, lines end with LF.

| Client sends  | Server replies |
|---------------|----------------|
| *(connect)*   | `KICKSERV<TAB>1` (or `ERR<TAB>access denied` / `ERR<TAB>busy`) |
| `INFO <name>` | `OK<TAB><size><TAB><date><TAB><$VER string>` or `ERR<TAB><msg>` |
| `GET <name>`  | `OK<TAB><size>` followed by exactly `<size>` raw bytes, or `ERR<TAB><msg>` |
| `LIST`        | `OK`, one `<size><TAB><date><TAB><name><TAB><$VER>` line per file, then `.` |
| `PING`        | `OK` |
| `QUIT`        | `BYE`, then the server closes the connection |

`<date>` is seconds since 1978-01-01 00:00 in Amiga local time.

## License

Kickstart Sync is free and unencumbered software released into the public
domain ([Unlicense](https://unlicense.org/)); see [LICENSE](LICENSE).
