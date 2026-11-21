#!/bin/sh
# Tests KickSync against kickserv_mock.py.  usage: ./test.sh [path/to/KickSync]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
KF=${1:-$HERE/KickSync}
PORT=${PORT:-6899}
T=$(mktemp -d)
trap 'kill $MOCK 2>/dev/null; rm -rf "$T"' EXIT
fail=0
check() {   # check "description" command...
    d=$1; shift
    if "$@"; then echo "ok   - $d"; else echo "FAIL - $d"; fail=1; fi
}
mk() { printf 'BIN\000\001$VER: %s\000tail' "$2" > "$1"; }

mkdir -p "$T/zip/Kickstart" "$T/amiga"
cd "$T"
mk zip/Kickstart/Kernel             "kernel 54.30 (1.1.2020)"         # newer on Amiga, case differs
mk zip/Kickstart/EXEC.library.kmod  "exec.library 54.10 (1.1.2020)"   # older on Amiga
mk zip/Kickstart/dos.library.kmod   "dos.library 54.100 (1.1.2020)"   # same
mk zip/Kickstart/only.zip.kmod      "only.zip 1.0 (1.1.2020)"         # not on Amiga
mk zip/Kickstart/nover.amiga.kmod   "nover 1.0 (1.1.2020)"            # Amiga copy has no $VER
printf 'plain\000' > zip/Kickstart/nover.zip.kmod                     # zip copy has no $VER
echo "MODULE Kickstart/kernel" > zip/Kickstart/Kicklayout             # no $VER either side
(cd zip && zip -qr ../Kickstart.zip Kickstart)

mk amiga/kernel             "kernel 54.46 (1.1.2024)"
mk amiga/exec.library.kmod  "exec.library 54.9 (1.1.2019)"
mk amiga/DOS.library.kmod   "dos.library 54.100 (1.1.2020)"
printf 'NEW-NO-VERSION\000' > amiga/nover.amiga.kmod
mk amiga/nover.zip.kmod     "nover.zip 2.1 (1.1.2024)"
echo "MODULE Kickstart/kernel NEW" > amiga/Kicklayout

python3 "$HERE/kickserv_mock.py" amiga "$PORT" >/dev/null 2>&1 &
MOCK=$!
sleep 1

"$KF" "localhost:$PORT" Kickstart.zip new.zip > out.txt 2>&1
check "run succeeds" test $? -eq 0
cat out.txt
row() { grep "^Kickstart/$1 " out.txt | grep -q "$2"; }
check "newer Amiga version is taken"        row Kernel "UPDATED (Amiga: kernel, renamed)"
check "older Amiga version is kept"         row EXEC.library.kmod "kept (zip is newer)"
check "same version is kept"                row dos.library.kmod "same"
check "missing on Amiga is kept"            row only.zip.kmod "kept (not on Amiga)"
check "no \$VER on Amiga -> taken"          row nover.amiga.kmod "UPDATED (no version on Amiga)"
check "no \$VER in zip -> taken"            row nover.zip.kmod "UPDATED (no version in zip)"
check "no \$VER anywhere -> taken"          row Kicklayout "UPDATED (no version on Amiga)"

check "zip is valid"                        unzip -tq new.zip
check "zip keeps original case"             sh -c 'unzip -Z1 new.zip | grep -qx Kickstart/Kernel'
check "same entry order"                    sh -c '[ "$(unzip -Z1 new.zip)" = "$(unzip -Z1 Kickstart.zip)" ]'
check "Kernel content is Amiga's"           sh -c 'unzip -p new.zip Kickstart/Kernel | grep -aq "54.46"'
check "Kicklayout content is Amiga's"       sh -c 'unzip -p new.zip Kickstart/Kicklayout | grep -q NEW'
check "kept entry copied unchanged"         sh -c 'unzip -p new.zip Kickstart/EXEC.library.kmod | cmp -s - zip/Kickstart/EXEC.library.kmod'

mkdir run && cd run
"$KF" "127.0.0.1:$PORT" ../Kickstart.zip > /dev/null 2>&1
check "default name Kickstart_<date>.zip"   test -f "Kickstart_$(date +%Y-%m-%d).zip"
"$KF" "127.0.0.1:$PORT" ../Kickstart.zip > /dev/null 2>&1
check "refuses to overwrite"                test $? -eq 1
"$KF" -f "127.0.0.1:$PORT" ../Kickstart.zip > /dev/null 2>&1
check "--force overwrites"                  test $? -eq 0
"$KF" -n "127.0.0.1:$PORT" ../Kickstart.zip dry.zip > /dev/null 2>&1
check "--dry-run writes nothing"            test ! -e dry.zip
"$KF" "127.0.0.1:$PORT" ../Kickstart.zip ../Kickstart.zip -f > /dev/null 2>&1
check "never overwrites input"              test $? -eq 1
cd ..
check "--list works"                        sh -c "'$KF' --list localhost:$PORT | grep -q 'kernel .*54.46'"
"$KF" "localhost:1" Kickstart.zip x.zip > /dev/null 2>&1
check "connection refused -> exit 1"        test $? -eq 1
"$KF" "localhost:$PORT" nonexist.zip x.zip > /dev/null 2>&1
check "missing input -> exit 1"             test $? -eq 1
"$KF" "localhost:99999" Kickstart.zip > /dev/null 2>&1
check "bad port -> exit 2"                  test $? -eq 2

[ $fail -eq 0 ] && echo "all tests passed"
exit $fail
