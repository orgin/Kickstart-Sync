#!/bin/bash
# Builds a fully static KickSync for Linux x86_64 (musl + zlib), which runs on
# any distribution:  dist/KickSync-<version>-linux-x86_64
#
# Needs musl-gcc (Debian/Ubuntu: apt install musl-tools) and curl. zlib is
# downloaded once into build/ (checked against its SHA-256) and its C files
# are compiled directly; nothing from the download is run.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZVER=1.3.1
ZSHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
ZURL="https://github.com/madler/zlib/releases/download/v$ZVER/zlib-$ZVER.tar.gz"
B="$ROOT/build/linux-static"
Z="$B/zlib-$ZVER"

ver=$(sed -n 's/^#define KSYNC_LIB_VERSION[[:space:]]*"\(.*\)"/\1/p' "$ROOT/host/kicksync.h")
[ -n "$ver" ] || { echo "no KSYNC_LIB_VERSION in host/kicksync.h" >&2; exit 1; }
OUT="$ROOT/dist/KickSync-$ver-linux-x86_64"

mkdir -p "$B" "$ROOT/dist"
if [ ! -f "$Z/zlib.h" ]; then
	curl -fsSL -o "$B/zlib-$ZVER.tar.gz" "$ZURL"
	echo "$ZSHA  $B/zlib-$ZVER.tar.gz" | sha256sum -c --quiet
	tar -xzf "$B/zlib-$ZVER.tar.gz" -C "$B"
fi

rm -rf "${B:?}/obj"
mkdir -p "$B/obj"
for f in adler32 crc32 deflate inflate inffast inftrees trees zutil; do
	musl-gcc -O2 -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN -I"$Z" -c "$Z/$f.c" -o "$B/obj/z_$f.o"
done
musl-gcc -O2 -Wall -Wextra -std=c99 -static -I"$Z" -o "$OUT" \
	"$ROOT/host/main.c" "$ROOT/host/kicksync.c" "$B"/obj/z_*.o
strip "$OUT"

file "$OUT"
"$OUT" --version
echo "-> $OUT"
