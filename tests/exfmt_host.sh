#!/bin/sh
# The exFAT formatting of EXFMT on the host (build/host/test_exfmt): sizes
# from 1 MB to 40 GB (sparse files), all default cluster sizes and some
# chosen ones, TexFAT, bad clusters (also where the system clusters would
# go).  Each image is checked by build/host/test_exchk, and by fsck.exfat
# when it is no TexFAT.  Then an image formatted this way is filled through
# the Linux exFAT driver (tests/exfat_image.sh FORMAT=exfmt) and checked.
#   tests/exfmt_host.sh [DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/exfmt-host}
fmt=$ROOT/build/host/test_exfmt
chk=$ROOT/build/host/test_exchk
make -C "$ROOT" -s build/host/test_exfmt build/host/test_exchk
mkdir -p "$dir"
fail=0
case_() {   # SIZE_MB CLUSTER NFATS BAD... ; expected cluster size in $want
    img=$dir/t.img
    rm -f "$img"
    truncate -s "$1M" "$img"
    size=$1; shift
    "$fmt" "$img" "$@" > "$dir/fmt.out" || { echo "FAILED: format $size MB $*"; cat "$dir/fmt.out"; fail=1; return; }
    if test -n "$want" && ! grep -q "clusters of $want bytes" "$dir/fmt.out"; then
        echo "FAILED: $size MB: not $want-byte clusters"; cat "$dir/fmt.out"; fail=1
    fi
    nbad=0; test $# -le 3 || nbad=$(($# - 3))
    if ! "$chk" "$img" > "$dir/chk.out" || ! grep -q "bad $nbad\$" "$dir/chk.out"; then
        echo "FAILED: check of $size MB $*"; cat "$dir/fmt.out" "$dir/chk.out"; fail=1
    fi
    if test "$2" = 1 && ! fsck.exfat -n "$img" > "$dir/fsck.out" 2>&1; then
        echo "FAILED: fsck.exfat on $size MB $*"; cat "$dir/fsck.out"; fail=1
    fi
}
want=4096;   case_ 1 0 1 ''
want=4096;   case_ 2 4096 1 ''
want=4096;   case_ 100 0 1 OPENASPI
want=4096;   case_ 256 0 2 TEXFAT
want=32768;  case_ 257 0 1 ''
want=32768;  case_ 2000 0 2 ''
want=131072; case_ 40000 0 1 GROSS
want=65536;  case_ 1000 65536 1 ''
want=512;    case_ 64 512 2 KLEIN
want=4096;   case_ 64 0 1 BAD 2 3 5 1000 16000
want=4096;   case_ 64 0 2 BAD2 2 4 6 7 16350
# a cluster size the volume has no room for, and a volume below 1 MB
truncate -s 1M "$dir/t.img"
if "$fmt" "$dir/t.img" 33554432 1 > /dev/null; then echo "FAILED: 32 MB clusters on 1 MB"; fail=1; fi
rm -f "$dir/t.img"; truncate -s 1000K "$dir/t.img"
if "$fmt" "$dir/t.img" 0 1 > /dev/null; then echo "FAILED: below 1 MB"; fail=1; fi
# filled by Linux
FORMAT=exfmt sh "$ROOT/tests/exfat_image.sh" "$dir/x" 64 >/dev/null || { echo "FAILED: Linux fill"; fail=1; }
"$chk" "$dir/x/exfat.img" > "$dir/chk.out" || { echo "FAILED: check after the Linux fill"; cat "$dir/chk.out"; fail=1; }
grep -q '^21 files, 3 directories' "$dir/chk.out" || { echo "FAILED: files after the Linux fill"; cat "$dir/chk.out"; fail=1; }
test "$fail" = 0 && echo "exFAT format host tests passed"
