#!/usr/bin/env python3
"""Damages an exFAT image made by tests/exfat_image.sh for the EXCHK tests.
   exchk_corrupt.py IMAGE CASE
CASE: bitmap-clear bitmap-set checksum hash cross chain boot dirty vdl"""
import struct
import sys

img, case = sys.argv[1], sys.argv[2]
f = open(img, 'r+b')


def rd(off, n):
    f.seek(off)
    return f.read(n)


def wr(off, data):
    f.seek(off)
    f.write(data)


boot = rd(0, 512)
sshift, cshift = boot[108], boot[109]
ss = 1 << sshift
cs = ss << cshift
fat_off = struct.unpack_from('<I', boot, 80)[0] * ss
heap = struct.unpack_from('<I', boot, 88)[0] * ss
nclu = struct.unpack_from('<I', boot, 92)[0]
root = struct.unpack_from('<I', boot, 96)[0]


def fat(c):
    return struct.unpack('<I', rd(fat_off + 4 * c, 4))[0]


def set_fat(c, v):
    wr(fat_off + 4 * c, struct.pack('<I', v))


def clu_off(c):
    return heap + (c - 2) * cs


def chain(c):
    out = [c]
    while fat(out[-1]) != 0xFFFFFFFF:
        out.append(fat(out[-1]))
    return out


def root_entries():
    """byte offsets of all root directory entries"""
    for c in chain(root):
        for i in range(cs // 32):
            yield clu_off(c) + 32 * i


def entry_sets():
    """name -> (offsets of the set, bitmap cluster) in the root directory"""
    offs = list(root_entries())
    sets, bitmap = {}, None
    i = 0
    while i < len(offs):
        e = rd(offs[i], 32)
        if e[0] == 0:
            break
        if e[0] == 0x81:
            bitmap = struct.unpack_from('<I', e, 20)[0]
        if e[0] == 0x85:
            n = e[1]
            set_offs = offs[i:i + n + 1]
            name = b''.join(rd(o, 32)[2:32] for o in set_offs[2:])
            length = rd(set_offs[1], 32)[3]
            sets[name[:2 * length].decode('utf-16-le')] = set_offs
            i += n
        i += 1
    return sets, bitmap


def fix_checksum(set_offs):
    data = bytearray(b''.join(rd(o, 32) for o in set_offs))
    s = 0
    for i, b in enumerate(data):
        if i in (2, 3):
            continue
        s = ((s >> 1) | ((s & 1) << 15)) + b & 0xFFFF
    wr(set_offs[0] + 2, struct.pack('<H', s))


def stream(set_offs):
    return bytearray(rd(set_offs[1], 32))


sets, bitmap = entry_sets()


def first_cluster(name):
    return struct.unpack_from('<I', stream(sets[name]), 20)[0]


def bitmap_bit(c, value):
    off = clu_off(bitmap) + (c - 2) // 8
    b = rd(off, 1)[0]
    b = b | 1 << ((c - 2) % 8) if value else b & ~(1 << ((c - 2) % 8))
    wr(off, bytes([b]))


if case == 'bitmap-clear':
    bitmap_bit(first_cluster('README.TXT'), 0)
elif case == 'bitmap-set':
    bitmap_bit(nclu + 1, 1)
elif case == 'checksum':
    o = sets['klein.txt'][2]
    wr(o + 2, b'K')
elif case == 'hash':
    s = stream(sets['klein.txt'])
    s[4] ^= 0x55
    wr(sets['klein.txt'][1], bytes(s))
    fix_checksum(sets['klein.txt'])
elif case == 'cross':
    s = stream(sets['klein.txt'])
    struct.pack_into('<I', s, 20, first_cluster('README.TXT'))
    wr(sets['klein.txt'][1], bytes(s))
    fix_checksum(sets['klein.txt'])
elif case == 'chain':
    s = stream(sets['frag.bin'])
    assert not s[1] & 2, 'frag.bin is contiguous'
    c = chain(first_cluster('frag.bin'))
    set_fat(c[len(c) // 2], 0xFFFFFFFF)
elif case == 'boot':
    wr(120, bytes([rd(120, 1)[0] ^ 0xFF]))
elif case == 'dirty':
    flags = struct.unpack('<H', rd(106, 2))[0]
    wr(106, struct.pack('<H', flags | 2))
elif case == 'vdl':
    s = stream(sets['sparse.dat'])
    size = struct.unpack_from('<Q', s, 24)[0]
    struct.pack_into('<Q', s, 8, size + 1)
    wr(sets['sparse.dat'][1], bytes(s))
    fix_checksum(sets['sparse.dat'])
else:
    sys.exit('unknown case ' + case)
f.close()
