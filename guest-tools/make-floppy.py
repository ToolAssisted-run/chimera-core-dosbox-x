#!/usr/bin/env python3
"""Builds chimera-mouse.img, the absolute-pointer install floppy (chimera#135):
a 1.44MB FAT12 image whose root holds the files below, its bytes a pure
function of them (fixed timestamps, fixed volume id), so a rebuild that
changes nothing changes no byte.

usage: make-floppy.py [out.img]   (default: chimera-mouse.img beside this file)
"""
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'chimera-mouse.img')

# (name on the floppy, file in this directory)
FILES = [
    ('README.TXT', 'README.TXT'),
    ('CHIMABS.EXE', 'chimabs/CHIMABS.EXE'),
    ('CHIMABS.C', 'chimabs/chimabs.c'),
    ('VBMOUSE.EXE', 'vbados/VBMOUSE.EXE'),
    ('VBMOUSE.DRV', 'vbados/VBMOUSE.DRV'),
    ('VBMOUSE.EN', 'vbados/VBMOUSE.EN'),
    ('OEMSETUP.INF', 'vbados/OEMSETUP.INF'),
    ('COPYING', 'vbados/COPYING'),
    ('VBADOS.TGZ', 'vbados/vbados-0.67-src.tar.gz'),
]

SECTOR = 512
TOTAL = 2880           # 1.44MB
FAT_SECTORS = 9
ROOT_ENTRIES = 224
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR  # 14
FAT1 = 1
FAT2 = FAT1 + FAT_SECTORS
ROOT = FAT2 + FAT_SECTORS                   # 19
DATA = ROOT + ROOT_SECTORS                  # 33 (cluster 2)
DOS_DATE = ((2026 - 1980) << 9) | (10 << 5) | 2   # 2026-10-02
DOS_TIME = 0

img = bytearray(TOTAL * SECTOR)

# ---- boot sector -----------------------------------------------------------
boot = bytearray(SECTOR)
boot[0:3] = b'\xEB\x3C\x90'
boot[3:11] = b'CHIMERA '
boot[11:13] = struct.pack('<H', SECTOR)
boot[13] = 1                                 # sectors per cluster
boot[14:16] = struct.pack('<H', 1)           # reserved
boot[16] = 2                                 # FATs
boot[17:19] = struct.pack('<H', ROOT_ENTRIES)
boot[19:21] = struct.pack('<H', TOTAL)
boot[21] = 0xF0                              # media descriptor
boot[22:24] = struct.pack('<H', FAT_SECTORS)
boot[24:26] = struct.pack('<H', 18)          # sectors per track
boot[26:28] = struct.pack('<H', 2)           # heads
boot[38] = 0x29                              # extended boot signature
boot[39:43] = struct.pack('<I', 0x1D0BB05F)  # fixed volume id
boot[43:54] = b'CHIMERAMOUS'
boot[54:62] = b'FAT12   '
# not bootable: say so instead of hanging (int 10h teletype, then wait for a key)
msg = b'Not a boot disk: the Chimera mouse install floppy.\r\n\0'
code = bytes([0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0xFB,          # cli; xor ax,ax; mov ds,ax; sti
              0xBE, 0x00, 0x00,                            # mov si, msg (patched)
              0xAC, 0x08, 0xC0, 0x74, 0x06,                # lodsb; or al,al; jz done
              0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5,          # mov ah,0Eh; int 10h; jmp lodsb
              0x30, 0xE4, 0xCD, 0x16, 0xCD, 0x19])         # done: xor ah,ah; int 16h; int 19h
code = bytearray(code)
msg_at = 0x3E + len(code)
struct.pack_into('<H', code, 7, 0x7C00 + msg_at)
boot[0x3E:0x3E + len(code)] = code
boot[msg_at:msg_at + len(msg)] = msg
boot[510:512] = b'\x55\xAA'
img[0:SECTOR] = boot

# ---- files: contiguous chains from cluster 2 ---------------------------------
fat_entries = [0xFF0, 0xFFF]                 # reserved entries 0 and 1
root = bytearray(ROOT_SECTORS * SECTOR)
cluster = 2
for i, (name, path) in enumerate(FILES):
    data = open(os.path.join(here, path), 'rb').read()
    if name.endswith('.TXT'): data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    base, _, ext = name.partition('.')
    assert len(base) <= 8 and len(ext) <= 3 and name == name.upper(), name
    n = (len(data) + SECTOR - 1) // SECTOR
    first = cluster if n else 0
    for c in range(n):
        fat_entries.append(0xFFF if c == n - 1 else cluster + c + 1)
    at = (DATA + cluster - 2) * SECTOR
    img[at:at + len(data)] = data
    cluster += n
    e = bytearray(32)
    e[0:8] = base.ljust(8).encode()
    e[8:11] = ext.ljust(3).encode()
    e[11] = 0x20                                 # archive
    struct.pack_into('<HH', e, 22, DOS_TIME, DOS_DATE)
    struct.pack_into('<H', e, 18, DOS_DATE)      # last access
    struct.pack_into('<HH', e, 14, DOS_TIME, DOS_DATE)  # created
    struct.pack_into('<H', e, 26, first)
    struct.pack_into('<I', e, 28, len(data))
    root[i * 32:(i + 1) * 32] = e
assert DATA + cluster - 2 <= TOTAL, 'the files do not fit on a 1.44MB floppy'

fat = bytearray(FAT_SECTORS * SECTOR)
for idx, val in enumerate(fat_entries):
    off = idx * 3 // 2
    if idx % 2 == 0:
        fat[off] = val & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | (val >> 8)
    else:
        fat[off] = (fat[off] & 0x0F) | ((val & 0xF) << 4)
        fat[off + 1] = val >> 4
img[FAT1 * SECTOR:(FAT1 + FAT_SECTORS) * SECTOR] = fat
img[FAT2 * SECTOR:(FAT2 + FAT_SECTORS) * SECTOR] = fat
img[ROOT * SECTOR:DATA * SECTOR] = root

open(out, 'wb').write(img)
print(out, (cluster - 2) * SECTOR, 'bytes of files,', (TOTAL - DATA - cluster + 2) * SECTOR, 'free')
