#!/bin/sh
# The FAT check of ASPICHK (build/host/test_fatchk) on FAT12, FAT16 and
# FAT32 images of tests/fat_image.sh and on copies damaged by
# tests/fatchk_corrupt.py: each damage must give its message and exit
# code, in one pass and in passes of 100 clusters.  Then the repair (F):
# afterwards the check and fsck.fat must find no error.  A chain broken in
# the first FAT (fat1) must come back whole from the second: FRAG.BIN read
# by mtools equal to the original.
#   tests/aspichk_host.sh [DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/aspichk-host}
chk=$ROOT/build/host/test_fatchk
make -C "$ROOT" -s build/host/test_fatchk
fail=0
expect() {   # CASE RC MESSAGE
    cp "$base" "$dir/case.img"
    test "$1" = clean || python3 "$ROOT/tests/fatchk_corrupt.py" "$dir/case.img" "$1"
    for window in 0 100; do
        rc=0
        "$chk" "$dir/case.img" 0 "$window" > "$dir/$1.out" || rc=$?
        if test "$rc" != "$2" || ! grep -q "$3" "$dir/$1.out"; then
            echo "FAILED: FAT$bits $1 (window $window, rc $rc, want $2 and \"$3\")"; cat "$dir/$1.out"; fail=1
        fi
    done
    for window in 0 100; do
        cp "$base" "$dir/fix.img"
        test "$1" = clean || python3 "$ROOT/tests/fatchk_corrupt.py" "$dir/fix.img" "$1"
        "$chk" "$dir/fix.img" 0 "$window" F > "$dir/$1.fix" || true
        rc=0; "$chk" "$dir/fix.img" > "$dir/$1.after" || rc=$?
        if test "$rc" != 0 || grep -q WARNING "$dir/$1.after" || ! fsck.fat -n "$dir/fix.img" > "$dir/$1.fsck" 2>&1; then
            echo "FAILED: repair of FAT$bits $1 (window $window, rc $rc)"; cat "$dir/$1.fix" "$dir/$1.after" "$dir/$1.fsck"
            fail=1
        fi
        if test "$1" = fat1 || test "$1" = clean; then
            mcopy -n -i "$dir/fix.img" ::FRAG.BIN "$dir/frag.out"
            cmp -s "$dir/frag.out" "$dir/fat$bits/files/frag.bin" || { echo "FAILED: FAT$bits $1: FRAG.BIN not whole"; fail=1; }
        fi
    done
}
for bits in 12 16 32; do
    case $bits in 12) size=4 ;; 16) size=32 ;; 32) size=64 ;; esac
    sh "$ROOT/tests/fat_image.sh" "$dir/fat$bits" "$size" "$bits" >/dev/null
    base=$dir/fat$bits/fat.img
    expect clean 0 '0 errors, 0 warnings'
    expect lost 1 'is allocated, but not used (lost)'
    expect cross 1 'klein.txt: cluster [0-9]* is used twice (cross-linked)'
    expect chain 1 'frag.bin: cluster chain has'
    expect free 1 'frag.bin: cluster chain runs into a free cluster'
    expect loop 1 'frag.bin: cluster chain runs in a loop'
    expect long 1 'README.TXT: cluster chain has 1 of 3 clusters'
    expect short 1 'frag.bin: cluster chain is longer than the size'
    expect fat2 1 'The FATs differ in 1 sectors'
    expect fat1 1 'frag.bin: the FATs differ at cluster'
    expect lfn 1 'long name entries'
    expect dot 1 'SUB: ".." entry points to cluster'
    expect dirsize 1 'SUB: directory with size 4096'
    expect empty 1 'LEER.TXT: empty, but with first cluster'
    if test "$bits" != 12; then expect dirty 0 'marked dirty'; fi
    if test "$bits" = 32; then
        expect backup 1 'Backup boot sector differs'
        expect fsinfo 0 'FSInfo free count is 12345'
        expect boot 1 'using the backup in sector 6'
    fi
done
test "$fail" = 0 && echo "FAT check host tests passed"
