#!/bin/bash
# exFAT past 32 bits (src/exfat/exfat.c with 64-bit sector numbers and
# sizes): a sparse 3 TiB volume, with 512-byte sectors more than 2^32
# sectors, and the same with 4 KiB sectors.
# tests/exfat_big_test.c writes a file into its last clusters (past 2 TiB)
# and a file larger than 4 GiB and reads both back; fsck.exfat checks the
# volume and both files must read the same through the Linux exFAT driver.
# Needs about 4.1 GiB of disk space for the large file.
# (The exFAT check of src/exfat/exfat_check.c still reaches 2^32 sectors
# only, so it does not run here.)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/exfat-big}
make -C "$ROOT" -s build/host/test_exfat_big
t=$ROOT/build/host/test_exfat_big
rm -rf "$OUT"; mkdir -p "$OUT"
img=$OUT/big.img
trap 'rm -f "$img"' EXIT
for ss in 512 4096; do
    rm -f "$img"; truncate -s 3T "$img"
    mkfs.exfat -q -s "$ss" -c 1M -L BIG "$img" 2>/dev/null || mkfs.exfat -q -c 1M -L BIG "$img"
    "$t" "$img" > "$OUT/test$ss.txt" || { cat "$OUT/test$ss.txt"; echo "$ss: test_exfat_big failed"; exit 1; }
    fsck.exfat -n "$img" > "$OUT/fsck$ss.txt" 2>&1 || { cat "$OUT/fsck$ss.txt"; echo "$ss: fsck.exfat finds errors"; exit 1; }
    loop=$(udisksctl loop-setup --no-user-interaction -r -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
    udisksctl mount --no-user-interaction -b "$loop" >/dev/null
    mnt=$(lsblk -no MOUNTPOINT "$loop")
    rc=0
    "$t" -v "$mnt/HIGH.BIN" 1 && "$t" -v "$mnt/BIG.BIN" 2 || rc=1
    udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
    udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
    test $rc = 0 || { echo "$ss: the Linux driver reads other data"; exit 1; }
    echo "$ss-byte sectors passed: $(tail -1 "$OUT/test$ss.txt")"
done
echo "exFAT big volume tests passed"
