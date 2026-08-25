#!/usr/bin/env python3
"""Generates a minimal 1.44MB FAT12 floppy image holding one root file -
free, machine-generated content for the gate, bytes a pure function of the
inputs (fixed timestamps, fixed volume id).

usage: gen-testfloppy.py <out.img> <NAME.EXT> <content-string>
"""
import struct
import sys

out, name, content = sys.argv[1], sys.argv[2], sys.argv[3].encode()
assert len(content) <= 512 * 4, 'keep the test file small'

SECTOR = 512
TOTAL = 2880           # 1.44MB
FAT_SECTORS = 9
ROOT_ENTRIES = 224
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR  # 14
FAT1 = 1
FAT2 = FAT1 + FAT_SECTORS
ROOT = FAT2 + FAT_SECTORS                   # 19
DATA = ROOT + ROOT_SECTORS                  # 33 (cluster 2)

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
boot[39:43] = struct.pack('<I', 0x1D0BB05E)  # fixed volume id
boot[43:54] = b'CHIMERAFD  '
boot[54:62] = b'FAT12   '
boot[510:512] = b'\x55\xAA'

# ---- FAT12: media marker + one file chain from cluster 2 -------------------
clusters = max(1, (len(content) + SECTOR - 1) // SECTOR)
fat_entries = [0xFF0, 0xFFF]                 # reserved entries 0 and 1
for i in range(clusters):
    last = i == clusters - 1
    fat_entries.append(0xFFF if last else 2 + i + 1)

fat = bytearray(FAT_SECTORS * SECTOR)
for idx, val in enumerate(fat_entries):
    off = idx * 3 // 2
    if idx % 2 == 0:
        fat[off] = val & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | (val >> 8)
    else:
        fat[off] = (fat[off] & 0x0F) | ((val & 0xF) << 4)
        fat[off + 1] = val >> 4

# ---- root directory: one entry, fixed date (the sandbox epoch's day) -------
base, _, ext = name.upper().partition('.')
entry = bytearray(32)
entry[0:8] = base.encode().ljust(8)
entry[8:11] = ext.encode().ljust(3)
entry[11] = 0x20                             # archive
dos_time = (12 << 11) | (44 << 5) | (28 // 2)
dos_date = ((2017 - 1980) << 9) | (5 << 5) | 27
entry[22:24] = struct.pack('<H', dos_time)
entry[24:26] = struct.pack('<H', dos_date)
entry[26:28] = struct.pack('<H', 2)          # first cluster
entry[28:32] = struct.pack('<I', len(content))
root = bytes(entry).ljust(ROOT_SECTORS * SECTOR, b'\x00')

image = bytearray(TOTAL * SECTOR)
image[0:SECTOR] = boot
image[FAT1 * SECTOR:FAT1 * SECTOR + len(fat)] = fat
image[FAT2 * SECTOR:FAT2 * SECTOR + len(fat)] = fat
image[ROOT * SECTOR:ROOT * SECTOR + len(root)] = root
image[DATA * SECTOR:DATA * SECTOR + len(content)] = content
open(out, 'wb').write(image)
print(f'{out}: 1.44MB FAT12, /{base}.{ext} = {len(content)} bytes')
