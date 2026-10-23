#!/bin/sh
# Makes exFAT test images with mkfs.exfat and fills them through the Linux
# exFAT driver (udisksctl, no root needed): long and German names, names
# that collide in 8.3, subdirectories, fragmented and contiguous files, a
# file larger than its valid data length.  The files are also kept in
# DIR/files for comparison.
#   tests/exfat_image.sh DIR [SIZE_MB [CLUSTER]]   -> DIR/exfat.img
set -eu
dir=$1; size=${2:-64}; cluster=${3:-4K}
img=$dir/exfat.img; files=$dir/files
mkdir -p "$dir"
rm -rf "$files" "$img"
mkdir -p "$files/Unterordner mit langem Namen/Tiefer" "$files/SUB"

# the content
printf 'hallo\r\n' > "$files/README.TXT"
printf 'kleingeschrieben\r\n' > "$files/klein.txt"
printf 'Umlaute\r\n' > "$files/Ärger über Größe.txt"
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    printf 'Datei %s\r\n' "$i" > "$files/Ein langer Name $i.txt"
done
printf 'tief\r\n' > "$files/Unterordner mit langem Namen/Tiefer/Datei.dat"
printf 'sub\r\n' > "$files/SUB/A.B"
printf 'zwei Punkte\r\n' > "$files/archiv.tar.gz"
head -c 300000 /dev/urandom > "$files/zufall.bin"

truncate -s "${size}M" "$img"
mkfs.exfat -q -c "$cluster" -L OPENASPI "$img"
loop=$(udisksctl loop-setup --no-user-interaction -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
trap 'udisksctl unmount --no-user-interaction -b "$loop" >/dev/null 2>&1 || true; udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true' EXIT
udisksctl mount --no-user-interaction -b "$loop" >/dev/null
mnt=$(lsblk -no MOUNTPOINT "$loop")
cp -r "$files/." "$mnt/"
# fragmented: two files written in turns, one of them deleted afterwards
for i in $(seq 1 40); do
    head -c 5000 /dev/urandom >> "$mnt/frag.bin"
    head -c 5000 /dev/urandom >> "$mnt/luecke.tmp"
    sync "$mnt/frag.bin"
done
rm "$mnt/luecke.tmp"
cp "$mnt/frag.bin" "$files/frag.bin"
# valid data length below the size: the rest reads as zeros
printf 'Anfang' > "$mnt/sparse.dat"
truncate -s 70000 "$mnt/sparse.dat"
cp "$mnt/sparse.dat" "$files/sparse.dat"
sync
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
trap - EXIT
fsck.exfat -n "$img" >/dev/null
echo "$img"
