#!/usr/bin/env python3
"""Generates a minimal ISO9660 image - free, machine-generated CD content for
the gate (no iso tool needed, and the bytes are a pure function of the
inputs).

usage: gen-testiso.py <out.iso> NAME=@inline-text [NAME=path]...
Each argument adds one root-level file: '@' prefixed values are literal text,
anything else is read as a (binary) file path.
"""
import struct
import sys

SECTOR = 2048
out = sys.argv[1]
files = []  # (NAME, bytes)
for arg in sys.argv[2:]:
    name, _, val = arg.partition('=')
    data = val[1:].encode() if val.startswith('@') else open(val, 'rb').read()
    files.append((name.upper(), data))

def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)

def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)

# a fixed timestamp (the sandbox epoch's date), never the build machine's
DATE7 = bytes([117, 5, 27, 12, 44, 28, 0])

def dirrec(extent, size, flags, ident):
    rec = bytearray()
    rec += b'\x00'                      # [1] extended attr len
    rec += both32(extent)               # [2-9]
    rec += both32(size)                 # [10-17]
    rec += DATE7                        # [18-24]
    rec += bytes([flags, 0, 0])         # [25] flags, unit, gap
    rec += both16(1)                    # [28-31] volume sequence number
    rec += bytes([len(ident)]) + ident  # [32] name len + name
    if len(ident) % 2 == 0:
        rec += b'\x00'                  # pad: total record length must be even
    return bytes([len(rec) + 1]) + rec

LBA_PATH = 18
LBA_ROOT = 19
LBA_FIRST_FILE = 20

# lay the files out on consecutive sector runs after the root directory
extents = []
lba = LBA_FIRST_FILE
for name, data in files:
    sectors = max(1, (len(data) + SECTOR - 1) // SECTOR)
    extents.append(lba)
    lba += sectors
TOTAL = lba

rootsec = dirrec(LBA_ROOT, SECTOR, 2, b'\x00') + dirrec(LBA_ROOT, SECTOR, 2, b'\x01')
for (name, data), ext in zip(files, extents):
    rootsec += dirrec(ext, len(data), 0, name.encode() + b';1')
assert len(rootsec) <= SECTOR, 'too many files for the one-sector root'
rootsec = rootsec.ljust(SECTOR, b'\x00')

pathrec = bytes([1, 0]) + struct.pack('<I', LBA_ROOT) + struct.pack('<H', 1) + b'\x00\x00'
pathsec = pathrec.ljust(SECTOR, b'\x00')

pvd = bytearray(SECTOR)
pvd[0] = 1
pvd[1:6] = b'CD001'
pvd[6] = 1
pvd[8:40] = b'CHIMERA'.ljust(32)
pvd[40:72] = b'TESTCD'.ljust(32)
pvd[80:88] = both32(TOTAL)
pvd[120:124] = both16(1)
pvd[124:128] = both16(1)
pvd[128:132] = both16(SECTOR)
pvd[132:140] = both32(len(pathrec))
pvd[140:144] = struct.pack('<I', LBA_PATH)
root_rec = dirrec(LBA_ROOT, SECTOR, 2, b'\x00')
pvd[156:156 + len(root_rec)] = root_rec

term = bytearray(SECTOR)
term[0] = 255
term[1:6] = b'CD001'
term[6] = 1

image = bytearray(SECTOR * 16)          # system area
image += pvd
image += term
image += pathsec
image += rootsec
for name, data in files:
    sectors = max(1, (len(data) + SECTOR - 1) // SECTOR)
    image += data.ljust(sectors * SECTOR, b'\x00')
assert len(image) == TOTAL * SECTOR
open(out, 'wb').write(image)
print(f'{out}: {TOTAL} sectors, ' + ', '.join(f'/{n};1 = {len(d)} bytes' for n, d in files))
