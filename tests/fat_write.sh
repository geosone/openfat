#!/bin/bash
# FAT12/16/32 reading and writing of src/fat/fat.c on the host:
# tests/fat_write_test.c reads an image of tests/fat_image.sh (filled by
# the Linux vfat driver), changes it and the same files in a mirror
# directory; fsck.fat and test_fatchk check the image and the Linux vfat
# driver must read the same tree as the mirror.  For FAT12, FAT16 and
# FAT32, with several cluster sizes, a volume written until it is full and
# a fixed root directory written until it is full.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/fat-write}
make -C "$ROOT" -s build/host/test_fat_write build/host/test_fatchk
rm -rf "$OUT"
mkdir -p "$OUT"

# compare IMAGE MIRROR: the tree through the Linux driver against the mirror
compare() {
    local loop mnt rc=0
    loop=$(udisksctl loop-setup --no-user-interaction -r -f "$1" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
    udisksctl mount --no-user-interaction -b "$loop" -o utf8 >/dev/null
    mnt=$(lsblk -no MOUNTPOINT "$loop")
    diff -r "$2" "$mnt" || rc=1
    udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
    udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
    return $rc
}

run() {   # name size-MB bits [mkfs-options [FULL|ROOTFULL]]
    local dir=$OUT/$1
    MKFS_OPTS="${4:-}" sh "$ROOT/tests/fat_image.sh" "$dir" "$2" "$3" >/dev/null
    cp -r "$dir/files" "$dir/mirror"
    "$ROOT/build/host/test_fat_write" "$dir/fat.img" "$dir/mirror" ${5:-} > "$dir/test.txt" ||
        { cat "$dir/test.txt"; echo "$1: test_fat_write failed"; exit 1; }
    fsck.fat -n "$dir/fat.img" > "$dir/fsck.txt" 2>&1 || { cat "$dir/fsck.txt"; echo "$1: fsck.fat finds errors"; exit 1; }
    # (the host program of the FAT check reads 512-byte sectors only)
    case "${4:-}" in *"-S 4096"*) ;; *)
        "$ROOT/build/host/test_fatchk" "$dir/fat.img" > "$dir/fatchk.txt" || { cat "$dir/fatchk.txt"; echo "$1: test_fatchk finds errors"; exit 1; } ;;
    esac
    compare "$dir/fat.img" "$dir/mirror" || { echo "$1: the Linux driver reads another tree"; exit 1; }
    echo "$1 passed: $(tail -1 "$dir/test.txt")"
}
run fat12 4 12
run fat12-1k 3 12 "-s 2"
run fat16 32 16
run fat16-32k 128 16 "-s 64"
run fat32 300 32
run fat32-small 40 32 "-s 1"
run fat32-4k-sectors 300 32 "-S 4096 -s 1"
run fat12-full 2 12 "" FULL
run fat16-rootfull 16 16 "-r 112" ROOTFULL
echo "FAT write tests passed"
