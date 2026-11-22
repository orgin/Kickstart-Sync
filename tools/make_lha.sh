#!/bin/bash
# Puts the KickServ release together: dist/KickServ.lha (AmigaOS 4) and
# dist/KickSync-<version>-linux-x86_64 (static, tools/build_linux_static.sh).
#
# Layout of the LHA:
#   KickServ.info              drawer icon
#   KickServ/
#     Install (+ .info)        Installation Utility script
#     Install.wbstartup        wbstartup.prefs entry, joined by Install
#     Install.wbsheader        header of a new wbstartup.prefs
#     KickServ (+ .info)       the commodity
#     KickServ.doc (+ .info)
#
# KickServ is built from amiga/kickserv.c with the OS4 gcc 15 Docker image,
# stripped, and also copied to amiga/KickServ (the binary kept in git).
# Both files are copied to $RELEASES (default /media/sf_claude_folder/releases).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"
RELEASES="${RELEASES:-/media/sf_claude_folder/releases}"
DOCKER="${DOCKER:-sudo docker}"
IMAGE=walkero/amigagccondocker:os4-gcc15-3.7.0
SRC="$ROOT/amiga/kickserv.c"
ICONS="${ICONS:-$ROOT/amiga/package/icons}"
STAGE="$ROOT/build/stage"
OS4="$ROOT/build/os4"
OUT="$ROOT/dist/KickServ.lha"

ver=$(sed -n 's/^#define KS_VERSION[[:space:]]*"\(.*\)"/\1/p' "$SRC")     # 1.0
date=$(sed -n 's/^#define KS_DATE[[:space:]]*"\(.*\)"/\1/p' "$SRC")       # 21.11.2026
[ -n "$ver" ] && [ -n "$date" ] || { echo "no KS_VERSION/KS_DATE in $SRC" >&2; exit 1; }
grep -q "^$ver ($date) " "$ROOT/amiga/package/KickServ.doc" ||
	{ echo "amiga/package/KickServ.doc: History has no \"$ver ($date)\" line" >&2; exit 1; }
for i in KickServ Install KickServ.doc KickServ_KickServ; do
	[ -f "$ICONS/$i.info" ] || { echo "missing $ICONS/$i.info (see amiga/package/icons/README.md)" >&2; exit 1; }
done

# --- KickServ ---------------------------------------------------------------
rm -rf "${OS4:?}" "${STAGE:?}"
mkdir -p "$OS4" "$ROOT/dist"
cp "$SRC" "$OS4/"
$DOCKER run --rm -u "$(id -u):$(id -g)" -v "$OS4:/src" -w /src "$IMAGE" sh -c \
	'ppc-amigaos-gcc -O2 -Wall -W -D__USE_INLINE__ -o KickServ kickserv.c -lauto && ppc-amigaos-strip KickServ' \
	< /dev/null
strings "$OS4/KickServ" | grep -qx "\$VER: KickServ $ver ($date)" ||
	{ echo "build/os4/KickServ has no \$VER: KickServ $ver ($date)" >&2; exit 1; }
cp "$OS4/KickServ" "$ROOT/amiga/KickServ"

# --- stage and pack ---------------------------------------------------------
D="$STAGE/KickServ"
mkdir -p "$D"
cp "$OS4/KickServ" "$ROOT/amiga/package/Install" "$D/"
sed -e "s/@VERSION@/$ver/g" -e "s/@DATE@/$date/g" "$ROOT/amiga/package/KickServ.doc" > "$D/KickServ.doc"
python3 -I - "$D" <<'EOF'
import struct, sys
d = sys.argv[1]
path = b"SYS:Utilities/Commodities/KickServ"
open(d + "/Install.wbstartup", "wb").write(struct.pack(">II", 0x10000000, len(path)) + path)
open(d + "/Install.wbsheader", "wb").write(bytes.fromhex("000c6fbd00000003"))
EOF
cp "$ICONS/KickServ.info" "$STAGE/"
cp "$ICONS/Install.info" "$ICONS/KickServ.doc.info" "$D/"
cp "$ICONS/KickServ_KickServ.info" "$D/KickServ.info"

rm -f "$OUT"
(cd "$STAGE" && "$LHA" aq2o5 "$OUT" KickServ.info KickServ)
"$LHA" tq "$OUT"
"$LHA" l "$OUT"
echo "KickServ $ver ($date) -> $OUT"

# --- KickSync for Linux -----------------------------------------------------
"$ROOT/tools/build_linux_static.sh"
kver=$(sed -n 's/^#define KSYNC_LIB_VERSION[[:space:]]*"\(.*\)"/\1/p' "$ROOT/host/kicksync.h")
LINUX="$ROOT/dist/KickSync-$kver-linux-x86_64"

if [ -d "$RELEASES" ]; then
	cp "$OUT" "$LINUX" "$RELEASES/"
	echo "copied $(basename "$OUT") and $(basename "$LINUX") to $RELEASES"
fi
