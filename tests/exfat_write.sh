#!/bin/bash
# The exFAT writing of common/exfat/exfat.c on the host: tests/exfat_write_test.c
# changes an image of tests/exfat_image.sh and the same files in a mirror
# directory; fsck.exfat checks the image and the Linux exFAT driver must
# read the same tree as the mirror.  Then the same on TexFAT images (both
# FATs and bitmaps must stay equal; checked by test_exchk), and on a small
# image that is written until it is full.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/exfat-write}
make -C "$ROOT" build/host/test_exfat_write build/host/test_exchk >/dev/null
rm -rf "$OUT"
mkdir -p "$OUT"

# compare IMAGE MIRROR: the tree through the Linux driver against the mirror
compare() {
    local loop mnt rc=0
    loop=$(udisksctl loop-setup --no-user-interaction -r -f "$1" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
    udisksctl mount --no-user-interaction -b "$loop" >/dev/null
    mnt=$(lsblk -no MOUNTPOINT "$loop")
    diff -r "$2" "$mnt" || rc=1
    udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
    udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
    return $rc
}

run() {   # name cluster [texfat-active] [full]
    local dir=$OUT/$1
    sh "$ROOT/tests/exfat_image.sh" "$dir" "${5:-64}" "$2" >/dev/null
    if test -n "${3:-}"; then python3 "$ROOT/tests/exfat_texfat.py" "$dir/exfat.img" "$3"; fi
    cp -r "$dir/files" "$dir/mirror"
    "$ROOT/build/host/test_exfat_write" "$dir/exfat.img" "$dir/mirror" ${4:-}
    if test -z "${3:-}"; then
        fsck.exfat -n "$dir/exfat.img" > "$dir/fsck.txt" 2>&1 || { cat "$dir/fsck.txt"; echo "$1: fsck.exfat finds errors"; exit 1; }
        compare "$dir/exfat.img" "$dir/mirror" || { echo "$1: the Linux driver reads another tree"; exit 1; }
    else
        python3 - "$dir/exfat.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb'); b = f.read(512)
ss = 1 << b[108]; fat, fatlen = struct.unpack_from('<II', b, 80)
f.seek(fat * ss); a = f.read(fatlen * ss); c = f.read(fatlen * ss)
assert a == c, 'the two FATs differ'
PY
    fi
    "$ROOT/build/host/test_exchk" "$dir/exfat.img" > "$dir/exchk.txt" || { cat "$dir/exchk.txt"; echo "$1: test_exchk finds errors"; exit 1; }
    echo "$1 passed"
}
run plain 4K
run big-clusters 32K
run small-clusters 512
run texfat 4K 0
run texfat-second 4K 1
run full 4K "" full 8
echo "exFAT write tests passed"
