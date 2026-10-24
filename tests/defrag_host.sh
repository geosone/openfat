#!/bin/sh
# DEFRAG (build/host/test_defrag) on FAT12, FAT16 and FAT32 images of
# tests/defrag_image.sh: FULL and FILES must leave no fragmented file (FULL
# also no gap below the last cluster in use), the check (test_fatchk) and
# fsck.fat no error, and every file as before (read by mtools).  Power loss:
# FULL stopped after N sectors written, for N spread over the whole run;
# afterwards test_fatchk F must make the volume clean again (fsck.fat
# agrees) and no file may have changed.
#   tests/defrag_host.sh [DIR [STEPS]]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/defrag-host}
steps=${2:-40}
dfg=$ROOT/build/host/test_defrag
chk=$ROOT/build/host/test_fatchk
make -C "$ROOT" -s build/host/test_defrag build/host/test_fatchk
fail=0
same() {   # IMAGE WHAT: every file as in files/
    rm -rf "$dir/out"; mkdir "$dir/out"
    mcopy -s -n -i "$1" '::*' "$dir/out/" 2>/dev/null
    diff -r "$dir/out" "$base/files" > "$dir/diff" 2>&1 || { echo "FAILED: FAT$bits $2: files differ"; head "$dir/diff"; fail=1; }
}
clean() {   # IMAGE WHAT
    "$chk" "$1" > "$dir/chk" 2>&1 || { echo "FAILED: FAT$bits $2: check"; cat "$dir/chk"; fail=1; }
    grep -q '0 errors, 0 warnings' "$dir/chk" || { echo "FAILED: FAT$bits $2: warnings"; cat "$dir/chk"; fail=1; }
    fsck.fat -n "$1" > "$dir/fsck" 2>&1 || { echo "FAILED: FAT$bits $2: fsck.fat"; cat "$dir/fsck"; fail=1; }
}
for bits in 12 16 32; do
    case $bits in 12) set -- 4 4 ;; 16) set -- 32 1 ;; 32) set -- 64 1 ;; esac
    base=$dir/fat$bits
    sh "$ROOT/tests/defrag_image.sh" "$base" "$1" "$bits" "$2" >/dev/null
    "$dfg" "$base/fat.img" A > "$dir/a.out"
    grep -q 'fragmented [1-9]' "$dir/a.out" || { echo "FAILED: FAT$bits image not fragmented"; cat "$dir/a.out"; fail=1; }
    for mode in F U; do
        cp "$base/fat.img" "$dir/run.img"
        "$dfg" "$dir/run.img" "$mode" > "$dir/$mode.out" || { echo "FAILED: FAT$bits $mode"; cat "$dir/$mode.out"; fail=1; }
        grep -q 'after 0;' "$dir/$mode.out" || { echo "FAILED: FAT$bits $mode: still fragmented"; cat "$dir/$mode.out"; fail=1; }
        if test "$mode" = F; then
            grep -q 'gaps 0$' "$dir/F.out" || { echo "FAILED: FAT$bits F: gaps left"; cat "$dir/F.out"; fail=1; }
        fi
        clean "$dir/run.img" "$mode"
        same "$dir/run.img" "$mode"
    done
    total=$(sed -n 's/.*sectors written \([0-9]*\);.*/\1/p' "$dir/F.out")
    n=1
    while test "$n" -lt "$total"; do
        cp "$base/fat.img" "$dir/cut.img"
        rc=0; "$dfg" "$dir/cut.img" F "$n" > "$dir/cut.out" || rc=$?
        test "$rc" = 3 || { echo "FAILED: FAT$bits cut at $n: rc $rc"; cat "$dir/cut.out"; fail=1; }
        "$chk" "$dir/cut.img" 0 0 F > "$dir/cut.fix" || true
        clean "$dir/cut.img" "cut at $n"
        same "$dir/cut.img" "cut at $n"
        n=$((n + total / steps + 1))
    done
    echo "FAT$bits: $(tail -1 "$dir/F.out")"
done
test "$fail" = 0 && echo "DEFRAG host tests passed"
