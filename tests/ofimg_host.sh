#!/bin/bash
# tools/ofimg.c on the host: an exFAT volume in a partition of a GPT
# image, formatted, with directories made on the way, files put (one into
# a directory by its own name, one with a non-ASCII name, one replaced);
# fsck.exfat checks the volume and the Linux exFAT driver must read the
# same tree as the mirror.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/ofimg}
make -C "$ROOT" build/host/ofimg >/dev/null
rm -rf "$OUT"
mkdir -p "$OUT/mirror/GONWARE/NLS" "$OUT/mirror/EMPTY/SUB"
img=$OUT/disk.img part=$OUT/part.img
truncate -s 80M "$img"
printf 'label: gpt\nstart=4096, size=60M\n' | sfdisk -q "$img"
head -c 300000 /dev/urandom >"$OUT/big.bin"
cp "$ROOT/README.md" "$OUT/mirror/GONWARE/"
cp "$ROOT/README.md" "$OUT/mirror/GONWARE/NLS/Grüße.md"
cp "$OUT/big.bin" "$OUT/mirror/GONWARE/DATA.BIN"
"$ROOT/build/host/ofimg" "$img" 4096 format $((60 * 2048)) TEST mkdir EMPTY/SUB mkdir GONWARE/NLS \
	put "$ROOT/README.md" 'GONWARE\' put "$ROOT/README.md" /gonware/nls/Grüße.md \
	put "$ROOT/LICENSE" GONWARE/DATA.BIN put "$OUT/big.bin" GONWARE/data.bin
dd if="$img" of="$part" bs=512 skip=4096 count=$((60 * 2048)) status=none
fsck.exfat -n "$part" >"$OUT/fsck.txt" 2>&1 || { cat "$OUT/fsck.txt"; echo "ofimg: fsck.exfat finds errors"; exit 1; }
loop=$(udisksctl loop-setup --no-user-interaction -r -f "$part" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
udisksctl mount --no-user-interaction -b "$loop" >/dev/null
mnt=$(lsblk -no MOUNTPOINT "$loop")
rc=0
diff -r "$OUT/mirror" "$mnt" || rc=1
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
[ $rc = 0 ] || { echo "ofimg: the Linux driver reads another tree"; exit 1; }
echo "ofimg: ok"
