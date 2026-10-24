#!/bin/sh
# Makes FAT test images with mkfs.fat and fills them through the Linux vfat
# driver (udisksctl, no root needed): long and German names, subdirectories,
# a fragmented file (written in turns with another file that is deleted),
# small and empty files.  The files are also kept in DIR/files.
#   tests/fat_image.sh DIR [SIZE_MB [BITS]]   -> DIR/fat.img   (BITS 12, 16, 32)
set -eu
dir=$1; size=${2:-32}; bits=${3:-16}
img=$dir/fat.img; files=$dir/files
mkdir -p "$dir"
rm -rf "$files" "$img"
mkdir -p "$files/Unterordner mit langem Namen/Tiefer" "$files/SUB"
printf 'hallo\r\n' > "$files/README.TXT"
printf 'kleingeschrieben\r\n' > "$files/klein.txt"
printf 'Umlaute\r\n' > "$files/Ärger über Größe.txt"
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    printf 'Datei %s\r\n' "$i" > "$files/Ein langer Name $i.txt"
done
printf 'tief\r\n' > "$files/Unterordner mit langem Namen/Tiefer/Datei.dat"
printf 'sub\r\n' > "$files/SUB/A.B"
: > "$files/LEER.TXT"
head -c 100000 /dev/urandom > "$files/zufall.bin"

truncate -s "${size}M" "$img"
mkfs.fat -F "$bits" -n FATTEST "$img" >/dev/null
loop=$(udisksctl loop-setup --no-user-interaction -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
trap 'udisksctl unmount --no-user-interaction -b "$loop" >/dev/null 2>&1 || true; udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true' EXIT
udisksctl mount --no-user-interaction -b "$loop" -o utf8 >/dev/null
mnt=$(lsblk -no MOUNTPOINT "$loop")
cp -r "$files/." "$mnt/"
for i in $(seq 1 30); do
    head -c 3000 /dev/urandom >> "$mnt/frag.bin"
    head -c 3000 /dev/urandom >> "$mnt/luecke.tmp"
    sync "$mnt/frag.bin"
done
rm "$mnt/luecke.tmp"
cp "$mnt/frag.bin" "$files/frag.bin"
sync
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
trap - EXIT
fsck.fat -n "$img" >/dev/null
echo "$img"
