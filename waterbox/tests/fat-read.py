#!/usr/bin/env python3
"""Reads one file out of a FAT12/16 disk image - a partitioned hard disk or a
bare floppy - and writes its bytes to stdout; exit 1 if it is not there.
Enough for the gate to look inside a disk the machine exported, with nothing
installed.

usage: fat-read.py <image> <PATH\\NAME.EXT>
"""
import struct
import sys

img, want = sys.argv[1], sys.argv[2]
f = open(img, 'rb')


def at(offset, n):
    f.seek(offset)
    return f.read(n)


# a partitioned disk: the first FAT partition; a floppy: the boot sector itself
base = 0
mbr = at(0, 512)
if mbr[510:512] == b'\x55\xAA' and mbr[0x1C2] in (0x01, 0x04, 0x06, 0x0E):
    base = struct.unpack('<I', mbr[0x1C6:0x1CA])[0] * 512
bpb = at(base, 512)
bps, spc, reserved, nfats, rootents, total16, _, fatsz = struct.unpack('<HBHBHHBH', bpb[11:24])
total = total16 or struct.unpack('<I', bpb[32:36])[0]
fat_at = base + reserved * bps
root_at = fat_at + nfats * fatsz * bps
data_at = root_at + rootents * 32
clusters = (total - (data_at - base) // bps) // spc
fat16 = clusters >= 4085
fat = at(fat_at, fatsz * bps)


def chain(c):
    while 2 <= c < (0xFFF8 if fat16 else 0xFF8):
        yield c
        if fat16:
            c = struct.unpack('<H', fat[c * 2:c * 2 + 2])[0]
        else:
            v = struct.unpack('<H', fat[c * 3 // 2:c * 3 // 2 + 2])[0]
            c = v >> 4 if c & 1 else v & 0xFFF


def entries(data):
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if e[0] == 0:
            return
        if e[0] == 0xE5 or e[11] & 0x08:
            continue
        name = e[0:8].decode('latin-1').rstrip()
        ext = e[8:11].decode('latin-1').rstrip()
        yield (name + ('.' + ext if ext else '')).upper(), e[11], struct.unpack('<H', e[26:28])[0], struct.unpack('<I', e[28:32])[0]


def read_chain(c, size=None):
    out = b''.join(at(data_at + (n - 2) * spc * bps, spc * bps) for n in chain(c))
    return out if size is None else out[:size]


here = at(root_at, rootents * 32)
parts = want.upper().strip('\\').split('\\')
for i, part in enumerate(parts):
    for name, attr, first, size in entries(here):
        if name == part:
            if i == len(parts) - 1:
                sys.stdout.buffer.write(read_chain(first, size) if not attr & 0x10 else b'')
                sys.exit(0)
            here = read_chain(first)
            break
    else:
        sys.exit(1)
