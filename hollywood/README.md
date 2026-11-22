# KickSync for Hollywood

`kicksync.hws` is the KickSync library rewritten in Hollywood. It updates a
`Kickstart.zip` (the one BBoot loads when AmigaOS 4 boots under QEMU) with
newer Kickstart modules from the running AmigaOS 4 guest. Include it in a
Hollywood program with `@INCLUDE "kicksync.hws"`. It was written so that
Kyvos (the QEMU front end for AmigaOS 4 and MorphOS) can offer "update Kickstart.zip from the
running system".

It does what the C `KickSync` tool does, with the same KickServ protocol,
the same rules and the same report:

* Each zip entry is looked up on the Amiga by its file name, ignoring case
  (`Kickstart/Kernel` in the zip matches `kernel` on the Amiga).
* The Amiga copy is taken unless both copies have a `$VER:` version and the
  Amiga one is not newer. When a file has no `$VER:`, the Amiga copy is
  taken, unless it is byte for byte the same as the zip copy.
* `Kicklayout` is never replaced, because it lists the modules for one
  particular machine. If the two copies differ, the report lists the
  `MODULE` lines that are only in one of them. `TakeKicklayout = True`
  takes the Amiga copy anyway.
* The new zip keeps the original entry names, case and order. Unchanged
  entries are copied byte for byte. The input zip is never changed.

| File | Purpose |
|------|---------|
| `kicksync.hws` | The library: include it. Everything is in the `KickSync` table; helpers are named `p_KS_*`. |
| `KickSyncCLI.hws` | Example front end that prints the same table as the C tool. |
| `test/` | Tests that run without Hollywood (see [Testing](#testing)). |

## Requirements

* **Hollywood 9.0 or newer.** It uses only documented functions of
  Hollywood 9; Kyvos 1.6.0 is built with Hollywood 10.
* **7-Zip** (`7z`, or `7z.exe` on Windows). Hollywood can't read or write
  raw deflate data, so 7-Zip unpacks the zip entries and packs the updated
  ones. Kyvos already asks for the 7-Zip path; pass it as `SevenZip`.
* **KickServ** running in the AmigaOS 4 guest. It's a commodity that serves
  `SYS:Kickstart` on TCP port 6800. Get it from `KickServ.lha` in the
  Kickstart Sync releases; its installer can add it to WBStartup.
* **A way to reach the guest.** With QEMU user networking, forward a host
  port to the guest's port 6800:

      -nic user,model=rtl8139,hostfwd=tcp:127.0.0.1:6800-:6800

  (or add `hostfwd=tcp:127.0.0.1:6800-:6800` to an existing
  `-netdev user,...`). Then connect to `127.0.0.1`, port 6800. With
  tap/bridged networking, use the guest's IP address.

## Using it

```
@INCLUDE "kicksync.hws"

Function p_ShowProgress(msg$, done, total, ud)
    ; e.g. update a status line or progress bar
EndFunction

Local r, res = KickSync.UpdateZip({
    Host = "127.0.0.1", Port = 6800,
    InputZip = kickstartzip$,             ; the Kickstart.zip Kyvos boots with
    OutputZip = kickstartzip$ .. ".new",  ; default: Kickstart_<date>.zip next to it
    SevenZip = sevenzip$,                 ; full path of 7z / 7z.exe
    OnProgress = p_ShowProgress})

If r = #KSYNC_OK
    ; res.updated modules were updated; res.reports[0 ... res.entries-1]
    ; describe every entry; the new zip is res.output
Else
    ; res.error says what went wrong
EndIf
```

A typical flow in a GUI:

1. Call `UpdateZip` with `DryRun = True` and show `res.reports` in a list.
   Use the columns `entryname`, `zipver`, `amigaver` and `result`, plus any
   `notes` (the Kicklayout differences). A dry run writes nothing.
2. If the user agrees, run it again without `DryRun`. Then rename the old
   `Kickstart.zip` to a backup and the new zip to `Kickstart.zip`. BBoot
   reads the zip only when QEMU starts, so this can be done while the guest
   is running; the new modules are used from the next start on.

### Options

Pass them as table fields with the dot or constructor syntax, in any case:
`{InputZip = ...}` or `opt.InputZip = ...`, but not `opt["InputZip"]`.

| Field | Default | Meaning |
|-------|---------|---------|
| `Host` | (required) | Name or IP address of the guest |
| `Port` | 6800 | KickServ port |
| `Timeout` | 15000 | Network timeout in milliseconds |
| `Conn` | | An open connection from `KickSync.Connect()` to use instead |
| `InputZip` | (required) | The Kickstart.zip to update (never changed) |
| `OutputZip` | `Kickstart_<YYYY-MM-DD>.zip` next to `InputZip` | The new zip |
| `DryRun` | False | Only build the report, write nothing |
| `Force` | False | Overwrite an existing `OutputZip` |
| `TakeKicklayout` | False | Replace Kicklayout with the Amiga copy if it differs |
| `SevenZip` | `"7z"` | The 7-Zip program |
| `Run7z` | | Your own `function(exe$, args$, dir$)` that runs 7-Zip and returns 0 (see below) |
| `OnEntry` | | `function(report, userdata)`, called once per zip entry |
| `OnWarning` | | `function(message$, userdata)` |
| `OnProgress` | | `function(message$, done, total, userdata)` |
| `UserData` | | Passed to the callbacks |

Results: the status (`#KSYNC_OK` or a negative `#KSYNC_ERR_*` code) and a
table with `entries`, `updated`, `output`, `error` and `reports`.

Fields of each report:

| Field | Meaning |
|-------|---------|
| `entryname` | Name in the zip (kept in the new zip) |
| `amiganame` | Name on the Amiga, `""` if not there |
| `zipver`, `amigaver` | Versions as text: `54.30`, `?` (no `$VER:`), `-` (not on the Amiga) |
| `zipversion`, `amigaversion` | The same as tables, for `KickSync.CompareVersion()` |
| `action` | `#KSYNC_ACT_UPDATED`, `_SAME`, `_KEPT_ZIP_NEWER`, `_KEPT_NOT_ON_AMIGA`, `_KEPT_KICKLAYOUT` |
| `reason` | Why it was updated: `#KSYNC_REASON_NEWER`, `_NO_AMIGA_VERSION`, `_NO_ZIP_VERSION`, `_TAKE_KICKLAYOUT` |
| `renamed`, `identical`, `sizediffers`, `iskicklayout` | Flags |
| `layoutonlyzip`, `layoutonlyamiga` | `MODULE` paths in only one Kicklayout (`.n`, `.s[0 ... n-1]`) |
| `result`, `notes` | The English text of the C tool's table and Kicklayout notes |

Use `action` and `reason` to show the result in Kyvos' own languages.

The lower-level functions are public too: `KickSync.Connect`, `Close`,
`Info`, `Get`, `List`, `ConnError`, `FindVerString`, `ParseVersion`,
`CompareVersion`, `FormatVersion`, `ParseHostPort`, `AmigaDate`,
`NameEqual` and `DefaultOutputName`. Each one is described by a comment in
`kicksync.hws`.

### Things to know

* **Text encoding.** Names and `$VER:` strings are the raw bytes the Amiga
  sent (ISO-8859-1). The library passes `#ENCODING_RAW` to every string
  function that handles them, so the program's default encoding doesn't
  matter. Convert with `ConvertStr()` before showing non-ASCII text in a
  UTF-8 GUI.
* **The GUI waits.** `UpdateZip` blocks until it is done: about 2 seconds
  for a 64-module zip on the local machine, plus network time. Use
  `OnProgress` to show where it is.
* **7-Zip and console windows.** 7-Zip is started up to three times (unpack,
  pack, verify) with `Execute()`. Hollywood 9/10 don't return the exit code,
  so every result is checked instead: the CRC of every unpacked file, and
  the structure and CRCs of the new zip. On Windows each call may flash a
  console window. If Kyvos already has its own way of running 7-Zip, pass
  it as `Run7z`.
* **Differences from the C tool.** The default output file goes next to the
  input zip rather than into the current directory. Updated modules are
  deflated by 7-Zip rather than zlib, so the new zip can be a little
  smaller. Both are valid for BBoot.

## Testing

Hollywood itself was not available, so the tests translate the scripts to
Lua: `test/hw2lua.py` handles the subset of Hollywood these scripts use, and
`test/hwshim.lua` implements the Hollywood functions they call, following
the manual. The shim is strict where Hollywood is: reading a table field
that was never set is an error, and so are out-of-range `ByteAsc()` and
`ByteChr()` values. Then the C tool's test suite runs against the result:

    test/run_tests.sh      # needs python3, lua5.4, lua-socket, 7z, zip, unzip

All 37 checks pass. Results with CRCs returned as signed numbers
(`HW_SIGNED_CRC=1`) pass too. On the real 64-module QEMU `Kickstart.zip`,
the Hollywood version printed the same report as the C tool. With no
updates it wrote a zip byte-identical to the input. With updates, every
entry had the same contents as in the C tool's output.

What the translation can't prove is how Hollywood itself behaves. These are
the documented behaviours the library relies on, worth a quick check in a
real Hollywood:

* `ReceiveData(id, #RECEIVEALL, False)` returns the data available now
  (count 0 when there is none) without waiting for more.
* `err, ... = ?Func(...)` returns the error code first, then the results.
* `Execute(file$, args$, {Directory = ..., ForceExe = True})` runs 7-Zip in
  that directory.
* `MidStr()`, `FindStr()` and `LowerStr()` work on binary data when given
  `#ENCODING_RAW` / `#ENCODING_ISO8859_1`; `Concat(t, "", 0, n - 1)` joins
  a list.
* `Rename(old$, newname$)` takes a plain name as the second argument.

## License

Public domain ([Unlicense](https://unlicense.org/)), like the rest of
[Kickstart Sync](https://github.com/orgin/Kickstart-Sync).
