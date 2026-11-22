#!/bin/sh
# Runs the KickSync test suite (host/test.sh) against the Hollywood version,
# translated to Lua. Needs python3, lua5.4, lua-socket, 7z, zip and unzip.
# The default output name is expected next to the input zip (DEFNAME_DIR).
HERE=$(cd "$(dirname "$0")" && pwd)
DEFNAME_DIR=.. PORT=${PORT:-6898} exec "$HERE/../../host/test.sh" "$HERE/ks-hw"
