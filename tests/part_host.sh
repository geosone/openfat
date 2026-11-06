#!/bin/bash
# The partition tables of src/part/part.c on the host (build/host/test_part):
# MBR disks with primary and logical partitions, GPT disks with names, with
# 512- and 4096-byte sectors, GPT with a damaged primary header or entry
# array (read through the backup), FAT and exFAT superfloppies and blank
# disks (no table), a damaged EBR chain (error).  Each list must match
# what sfdisk reads.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/part-host}
make -C "$ROOT" -s build/host/test_part
t=$ROOT/build/host/test_part
rm -rf "$OUT"; mkdir -p "$OUT"

# same IMAGE SECTOR [BACKUP]: test_part against sfdisk --json
same() {
    "$t" "$1" "$2" > "$1.out" || { cat "$1.out"; echo "$1: test_part failed"; exit 1; }
    sfdisk --sector-size "$2" -J "$1" > "$1.json" 2>/dev/null
    python3 - "$1.json" "$1.out" "${3:-0}" <<'PY'
import json, re, sys
t = json.load(open(sys.argv[1]))['partitiontable']
lines = open(sys.argv[2]).read().splitlines()
head = lines[0].split()
assert head[1] == {'dos': 'mbr', 'gpt': 'gpt'}[t['label']], head
assert head[3] == sys.argv[3], ('backup', head)
want = []
for p in t['partitions']:
    idx = int(re.search(r'(\d+)$', p['node']).group(1))
    if t['label'] == 'dos':
        if int(p['type'], 16) in (5, 0xF, 0x85): continue
        typ = '%02X' % int(p['type'], 16)
        act = '128' if p.get('bootable') else '0'
        name = ''
    else:
        typ, act, name = p['type'].upper(), '0', p.get('name', '')
    want.append('%d %d %d %s %s "%s"' % (idx, p['start'], p['size'], typ, act, name))
got = lines[1:]
if t['label'] == 'gpt':
    got = [re.sub(r' (\d+) "', ' 0 "', g) for g in got]
assert got == want, (got, want)
PY
    echo "$(basename "$1") passed"
}

# MBR: two primaries (one active), an extended one, a primary, two logicals
img=$OUT/mbr.img; truncate -s 64M "$img"
sfdisk -q "$img" <<'S'
label: dos
label-id: 0x12345678
start=2048, size=10M, type=c, bootable
size=8M, type=6
size=30M, type=f
size=4M, type=83
start=+, size=4M, type=7
size=4M, type=e
S
same "$img" 512
grep -q 'signature 12345678' "$img.out"

# GPT with names, also 4K sectors
for ss in 512 4096; do
    img=$OUT/gpt$ss.img; truncate -s 64M "$img"
    sfdisk -q --sector-size $ss "$img" <<'S'
label: gpt
size=8M, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI system partition"
size=16M, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="GonWare C"
size=4M, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
S
    same "$img" $ss
done

# the primary header damaged, then the primary entry array
cp "$OUT/gpt512.img" "$OUT/gpt-header.img"
printf 'X' | dd of="$OUT/gpt-header.img" bs=1 seek=$((512 + 40)) conv=notrunc status=none
"$t" "$OUT/gpt-header.img" > "$OUT/gpt-header.out"
grep -q 'scheme gpt backup 1' "$OUT/gpt-header.out" || { cat "$OUT/gpt-header.out"; exit 1; }
diff <(tail -n +2 "$OUT/gpt512.img.out") <(tail -n +2 "$OUT/gpt-header.out")
cp "$OUT/gpt512.img" "$OUT/gpt-entries.img"
printf 'X' | dd of="$OUT/gpt-entries.img" bs=1 seek=$((1024 + 200)) conv=notrunc status=none
"$t" "$OUT/gpt-entries.img" > "$OUT/gpt-entries.out"
grep -q 'scheme gpt backup 1' "$OUT/gpt-entries.out" || { cat "$OUT/gpt-entries.out"; exit 1; }
diff <(tail -n +2 "$OUT/gpt512.img.out") <(tail -n +2 "$OUT/gpt-entries.out")
echo "gpt backup passed"

# no table: FAT12/16/32 and exFAT superfloppies, a blank disk
for bits in 12 16 32; do
    img=$OUT/fat$bits.img; rm -f "$img"; truncate -s 40M "$img"
    if test $bits = 12; then rm "$img"; mkfs.fat -C -F 12 "$img" 1440 >/dev/null; else mkfs.fat -F $bits "$img" >/dev/null; fi
    "$t" "$img" | grep -q 'scheme none' || { echo "fat$bits: a table found"; exit 1; }
done
img=$OUT/exfat.img; truncate -s 40M "$img"; mkfs.exfat -q "$img"
"$t" "$img" | grep -q 'scheme none'
truncate -s 1M "$OUT/blank.img"
"$t" "$OUT/blank.img" | grep -q 'scheme none'
echo "superfloppy passed"

# the EBR chain broken: the second EBR without its signature
cp "$OUT/mbr.img" "$OUT/ebr.img"
ebr2=$(python3 - "$OUT/mbr.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb'); m = f.read(512)
ext = [struct.unpack_from('<I', m, 446 + 16 * i + 8)[0] for i in range(4) if m[446 + 16 * i + 4] == 0xF][0]
f.seek(ext * 512); e = f.read(512)
print(ext + struct.unpack_from('<I', e, 462 + 8)[0])
PY
)
printf '\0\0' | dd of="$OUT/ebr.img" bs=1 seek=$((ebr2 * 512 + 510)) conv=notrunc status=none
if "$t" "$OUT/ebr.img" > "$OUT/ebr.out"; then cat "$OUT/ebr.out"; echo "ebr: no error"; exit 1; fi
grep -q 'error -2' "$OUT/ebr.out"
echo "ebr damage passed"
echo "partition host tests passed"
