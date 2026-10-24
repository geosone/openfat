#!/bin/sh
# The exFAT check (build/host/test_exchk) on an image of tests/exfat_image.sh
# and on copies damaged by tests/exchk_corrupt.py: each damage must give its
# message and exit code, in one pass and in passes of 100 clusters.  Then
# the repair (F): afterwards the check and fsck.exfat must find no error
# (cross-linked clusters stay: they are not repaired).  The same on TexFAT
# copies (tests/exfat_texfat.py, both ActiveFat values) with the damage of
# the second FAT and bitmap; fsck.exfat knows no TexFAT and is left out there.
#   tests/exchk_host.sh [DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/exchk-host}
chk=$ROOT/build/host/test_exchk
make -C "$ROOT" -s build/host/test_exchk build/host/test_exfat
sh "$ROOT/tests/exfat_image.sh" "$dir" 64 >/dev/null
fail=0
base=$dir/exfat.img
expect() {   # CASE RC MESSAGE
    cp "$base" "$dir/case.img"
    test "$1" = clean || python3 "$ROOT/tests/exchk_corrupt.py" "$dir/case.img" "$1"
    for window in 0 100; do
        rc=0
        if test "$window" = 0; then "$chk" "$dir/case.img" > "$dir/$1.out" || rc=$?
        else "$chk" "$dir/case.img" 0 "$window" > "$dir/$1.out" || rc=$?; fi
        if test "$rc" != "$2" || ! grep -q "$3" "$dir/$1.out"; then
            echo "FAILED: $1 (window $window, rc $rc, want $2 and \"$3\")"; cat "$dir/$1.out"; fail=1
        fi
    done
    case $base:$1 in *texfat*|*:clean|*:dirty) ;; *)
        if fsck.exfat -n "$dir/case.img" >/dev/null 2>&1; then echo "note: fsck.exfat finds no error in $1"; fi ;;
    esac
    for window in 0 100; do
        cp "$base" "$dir/fix.img"
        test "$1" = clean || python3 "$ROOT/tests/exchk_corrupt.py" "$dir/fix.img" "$1"
        "$chk" "$dir/fix.img" 0 "$window" F > "$dir/$1.fix" || true
        rc=0; "$chk" "$dir/fix.img" > "$dir/$1.after" || rc=$?
        if test "$1" = cross; then
            grep -q 'cross-linked' "$dir/$1.after" || { echo "FAILED: $1 repaired?"; fail=1; }
        elif test "$rc" != 0 || grep -q WARNING "$dir/$1.after" ||
             { case $base in *texfat*) false ;; *) ! fsck.exfat -n "$dir/fix.img" >/dev/null 2>&1 ;; esac; }; then
            echo "FAILED: repair of $1 (window $window, rc $rc)"; cat "$dir/$1.fix" "$dir/$1.after"
            fail=1
        fi
        if test "$1" = fat-active; then
            "$ROOT/build/host/test_exfat" "$dir/fix.img" 0 FRAG.BIN > "$dir/frag.out" 2>/dev/null || true
            cmp -s "$dir/frag.out" "$dir/files/frag.bin" || { echo "FAILED: frag.bin not restored from the other FAT"; fail=1; }
        fi
    done
}
cases() {
expect clean 0 '0 errors, 0 warnings'
expect bitmap-clear 1 'is used, but free in the allocation bitmap'
expect bitmap-set 1 'is allocated, but not used (lost)'
expect checksum 1 'Klein.txt: entry set checksum is wrong'
expect hash 1 'klein.txt: name hash is wrong'
expect cross 1 'used twice (cross-linked)'
case $base in   # TexFAT: the other FAT still has the chain
    *texfat*) expect chain 1 'frag.bin: the FATs differ' ;;
    *) expect chain 1 'frag.bin: cluster chain has' ;;
esac
expect boot 1 'Boot region checksum is wrong'
expect dirty 0 'marked dirty'
expect vdl 1 'sparse.dat: valid data length is larger than the size'
}
cases
for active in 0 1; do
    base=$dir/texfat$active.img
    cp "$dir/exfat.img" "$base"
    python3 "$ROOT/tests/exfat_texfat.py" "$base" "$active"
    cases
    expect fat-active 1 'frag.bin: the FATs differ'
    expect fat-other 1 'frag.bin: the FATs differ'
    expect bitmap2 1 'second allocation bitmap differs'
done
test "$fail" = 0 && echo "exFAT check host tests passed"
