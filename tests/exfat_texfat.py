#!/usr/bin/env python3
"""Turns an exFAT image of tests/exfat_image.sh into TexFAT: a second FAT
behind the first one, a second allocation bitmap (BitmapFlags bit 0 set),
NumberOfFats 2, and the boot region with its checksum and backup.
   exfat_texfat.py IMAGE [ACTIVE]     ACTIVE 1: the second FAT and bitmap
                                      are the active ones (VolumeFlags)"""
import struct
import sys

img = sys.argv[1]
active = int(sys.argv[2]) if len(sys.argv) > 2 else 0
f = open(img, 'r+b')


def rd(off, n):
    f.seek(off)
    return f.read(n)


def wr(off, data):
    f.seek(off)
    f.write(data)


boot = bytearray(rd(0, 512))
ss = 1 << boot[108]
cs = ss << boot[109]
fat, fatlen, heap, nclu, root = struct.unpack_from('<IIIII', boot, 80)
assert boot[110] == 1 and heap >= fat + 2 * fatlen, 'no room for a second FAT'


def clu_off(c):
    return heap * ss + (c - 2) * cs


# the root directory (one cluster), its bitmap entry and a free slot
rootdir = bytearray(rd(clu_off(root), cs))
slot = bm = None
for i in range(0, cs, 32):
    if rootdir[i] == 0x81:
        bm = i
    if rootdir[i] == 0 and slot is None:
        slot = i
bm_first, bm_len = struct.unpack_from('<IQ', rootdir, bm + 20)
bitmap = bytearray(rd(clu_off(bm_first), bm_len))
assert bm_len <= cs, 'bitmap of more than one cluster'
# a free cluster for the second bitmap
new = next(c for c in range(2, nclu + 2) if not bitmap[(c - 2) // 8] >> ((c - 2) % 8) & 1)
bitmap[(new - 2) // 8] |= 1 << ((new - 2) % 8)
wr(clu_off(bm_first), bytes(bitmap))
wr(clu_off(new), bytes(bitmap) + bytes(cs - bm_len))
wr(fat * ss + 4 * new, struct.pack('<I', 0xFFFFFFFF))
entry = bytearray(32)
entry[0] = 0x81
entry[1] = 1
struct.pack_into('<IQ', entry, 20, new, bm_len)
rootdir[slot:slot + 32] = entry
wr(clu_off(root), bytes(rootdir))
# the second FAT: a copy of the first
wr((fat + fatlen) * ss, rd(fat * ss, fatlen * ss))
# boot region: NumberOfFats, ActiveFat, checksum, backup
boot[110] = 2
flags = struct.unpack_from('<H', boot, 106)[0] & ~1 | active
struct.pack_into('<H', boot, 106, flags)
wr(0, bytes(boot))
s = 0
for sec in range(11):
    data = rd(sec * ss, ss)
    for i, b in enumerate(data):
        if sec == 0 and i in (106, 107, 112):
            continue
        s = ((s >> 1) | ((s & 1) << 31)) + b & 0xFFFFFFFF
wr(11 * ss, struct.pack('<I', s) * (ss // 4))
wr(12 * ss, rd(0, 12 * ss))
f.close()
