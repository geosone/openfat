#!/bin/sh
# Makes a fragmented FAT image for the DEFRAG tests with mkfs.fat and the
# Linux vfat driver (udisksctl, no root needed): six files written in turns,
# a subdirectory that grows in turns with a file (so the directory itself is
# fragmented) and has subdirectories of its own, deleted files that leave
# gaps, an empty file.  The files are also kept in DIR/files.
#   tests/defrag_image.sh DIR [SIZE_MB [BITS [SECTORS_PER_CLUSTER]]]   -> DIR/fat.img
set -eu
dir=$1; size=${2:-32}; bits=${3:-16}; spc=${4:-1}
img=$dir/fat.img; files=$dir/files
mkdir -p "$dir"
rm -rf "$files" "$img"
truncate -s "${size}M" "$img"
mkfs.fat -F "$bits" -s "$spc" -n DEFRAG "$img" >/dev/null
loop=$(udisksctl loop-setup --no-user-interaction -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
trap 'udisksctl unmount --no-user-interaction -b "$loop" >/dev/null 2>&1 || true; udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true' EXIT
udisksctl mount --no-user-interaction -b "$loop" -o utf8 >/dev/null
mnt=$(lsblk -no MOUNTPOINT "$loop")
for i in $(seq 1 12); do
    for f in A.BIN B.BIN "Long name C.dat" D.BIN E.BIN F.BIN; do
        head -c 1500 /dev/urandom >> "$mnt/$f"
        sync "$mnt/$f"
    done
done
mkdir "$mnt/Folder with files"
for i in $(seq 1 40); do
    printf 'file %s\r\n' "$i" > "$mnt/Folder with files/A long file name number $i.txt"
    sync "$mnt/Folder with files"
    head -c 700 /dev/urandom >> "$mnt/grow.bin"
    sync "$mnt/grow.bin"
    if test "$i" = 10 || test "$i" = 30; then
        mkdir "$mnt/Folder with files/Sub $i"
        printf 'deep\r\n' > "$mnt/Folder with files/Sub $i/deep.txt"
        head -c 3000 /dev/urandom > "$mnt/Folder with files/Sub $i/data.bin"
    fi
done
rm "$mnt/B.BIN" "$mnt/E.BIN"
: > "$mnt/EMPTY.TXT"
head -c 20000 /dev/urandom > "$mnt/last.bin"
sync
rm -rf "$files"; cp -r "$mnt" "$files"
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
trap - EXIT
fsck.fat -n "$img" >/dev/null
echo "$img"
