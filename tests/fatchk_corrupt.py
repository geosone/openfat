#!/usr/bin/env python3
"""Damages a FAT image made by tests/fat_image.sh for the ASPICHK tests.
   fatchk_corrupt.py IMAGE CASE
CASE: lost cross chain free loop long short fat2 fat1 lfn dot dirsize empty
dirty; FAT32 only: backup fsinfo boot"""
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


b = rd(0, 512)
ss, spc, res, nfats, rootent = struct.unpack_from('<HBHBH', b, 11)
fatlen = struct.unpack_from('<H', b, 22)[0] or struct.unpack_from('<I', b, 36)[0]
total = struct.unpack_from('<H', b, 19)[0] or struct.unpack_from('<I', b, 32)[0]
rootsecs = (rootent * 32 + ss - 1) // ss
heap = res + nfats * fatlen + rootsecs
nclu = (total - heap) // spc
bits = 32 if not struct.unpack_from('<H', b, 22)[0] else 12 if nclu < 4085 else 16
cs = ss * spc
rootclu = struct.unpack_from('<I', b, 44)[0] if bits == 32 else 0
EOC = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[bits]


def fat(c, copy=0):
    base = (res + copy * fatlen) * ss
    if bits == 12:
        w = struct.unpack('<H', rd(base + c + c // 2, 2))[0]
        return w >> 4 if c & 1 else w & 0xFFF
    if bits == 16:
        return struct.unpack('<H', rd(base + 2 * c, 2))[0]
    return struct.unpack('<I', rd(base + 4 * c, 4))[0] & 0x0FFFFFFF


def set_fat(c, v, copies=None):
    for copy in copies if copies is not None else range(nfats):
        base = (res + copy * fatlen) * ss
        if bits == 12:
            off = base + c + c // 2
            w = struct.unpack('<H', rd(off, 2))[0]
            w = (w & 0x000F) | (v << 4) if c & 1 else (w & 0xF000) | v
            wr(off, struct.pack('<H', w))
        elif bits == 16:
            wr(base + 2 * c, struct.pack('<H', v))
        else:
            wr(base + 4 * c, struct.pack('<I', v))


def clu_off(c):
    return (heap + (c - 2) * spc) * ss


def chain(c):
    out = [c]
    while fat(out[-1]) < EOC - 7:
        out.append(fat(out[-1]))
    return out


def dir_offsets(first):
    """byte offsets of the entries of a directory (0: the root)"""
    if not first and bits != 32:
        start = (res + nfats * fatlen) * ss
        return [start + 32 * i for i in range(rootent)]
    return [clu_off(c) + 32 * i for c in chain(first or rootclu) for i in range(cs // 32)]


def first_of(e):
    return struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16 if bits == 32 else 0)


def find(name, first=0):
    """the offset of the 8.3 entry NAME (11 bytes, blank padded)"""
    for o in dir_offsets(first):
        e = rd(o, 32)
        if e[0] == 0:
            break
        if e[:11] == name.encode():
            return o
    raise KeyError(name)


def start(name, first=0):
    return first_of(rd(find(name, first), 32))


if case == 'lost':
    c = next(c for c in range(nclu + 1, 2, -1) if fat(c) == 0)
    set_fat(c, EOC)
elif case == 'cross':
    o = find('KLEIN   TXT')
    wr(o + 26, struct.pack('<H', start('README  TXT') & 0xFFFF))
    if bits == 32:
        wr(o + 20, struct.pack('<H', start('README  TXT') >> 16))
elif case in ('chain', 'free', 'loop'):
    c = chain(start('FRAG    BIN'))
    assert len(c) > 4
    set_fat(c[len(c) // 2], {'chain': EOC, 'free': 0, 'loop': c[1]}[case])
elif case == 'long':
    o = find('README  TXT')
    wr(o + 28, struct.pack('<I', cs * 3))
elif case == 'short':
    o = find('FRAG    BIN')
    wr(o + 28, struct.pack('<I', 1))
elif case == 'fat2':
    c = chain(start('FRAG    BIN'))
    set_fat(c[2], EOC, [1])
elif case == 'fat1':
    c = chain(start('FRAG    BIN'))
    set_fat(c[len(c) // 2], 0, [0])
elif case == 'lfn':
    o = find('EINLAN~1TXT')
    e = rd(o - 32, 32)
    wr(o - 32 + 13, bytes([e[13] ^ 0x55]))
elif case == 'dot':
    o = find('..         ', start('SUB        '))
    wr(o + 26, struct.pack('<H', 0x1234 % nclu + 2))
elif case == 'dirsize':
    wr(find('SUB        ') + 28, struct.pack('<I', 4096))
elif case == 'empty':
    c = next(c for c in range(nclu + 1, 2, -1) if fat(c) == 0)
    set_fat(c, EOC)
    wr(find('LEER    TXT') + 26, struct.pack('<H', c & 0xFFFF))
elif case == 'dirty':
    set_fat(1, EOC & ~(0x8000 if bits == 16 else 0x08000000))
elif case == 'backup':
    wr(6 * ss + 71, b'X')
elif case == 'fsinfo':
    fs = struct.unpack_from('<H', b, 48)[0]
    wr(fs * ss + 488, struct.pack('<I', 12345))
elif case == 'boot':
    wr(11, b'\0\0')
else:
    sys.exit('unknown case ' + case)
f.close()
