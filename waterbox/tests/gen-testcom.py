#!/usr/bin/env python3
"""Generates the tiny hand-assembled DOS programs the input gate runs from
the test CD - machine-visible consumers for inputs nothing at the DOS prompt
would otherwise read:

  JOYTEST.COM   busy-loop: IN AL,201h (game port), mask the button bits,
                write them to text video memory at B800:0000. Joystick
                buttons change the screen, so the video digest sees them.
  MOUSETEST.COM INT 33h reset, then busy-loop: INT 33h AX=3 (buttons and
                position), write BL/CL/DL to B800:0000. Mouse movement and
                buttons change the screen.

usage: gen-testcom.py <out-dir>
"""
import os
import sys

outdir = sys.argv[1]

JOYTEST = bytes([
    0xB8, 0x00, 0xB8,        # mov ax, 0xB800
    0x8E, 0xC0,              # mov es, ax
    0x31, 0xFF,              # xor di, di
    0xBA, 0x01, 0x02,        # mov dx, 0x201
    # loop:
    0xEC,                    # in al, dx
    0x24, 0xF0,              # and al, 0xF0      (button bits only)
    0x26, 0x88, 0x05,        # mov es:[di], al
    0xB0, 0x07,              # mov al, 7
    0x26, 0x88, 0x45, 0x01,  # mov es:[di+1], al
    0xEB, 0xF2,              # jmp loop
])

MOUSETEST = bytes([
    0xB8, 0x00, 0x00,        # mov ax, 0        (mouse driver reset/detect)
    0xCD, 0x33,              # int 33h
    0xB8, 0x00, 0xB8,        # mov ax, 0xB800
    0x8E, 0xC0,              # mov es, ax
    # loop:
    0xB8, 0x03, 0x00,        # mov ax, 3        (buttons + position)
    0xCD, 0x33,              # int 33h          -> BX buttons, CX x, DX y
    0x31, 0xFF,              # xor di, di
    0x26, 0x88, 0x1D,        # mov es:[di], bl
    0xB0, 0x07,              # mov al, 7
    0x26, 0x88, 0x45, 0x01,  # mov es:[di+1], al
    0x26, 0x88, 0x4D, 0x02,  # mov es:[di+2], cl
    0x26, 0x88, 0x45, 0x03,  # mov es:[di+3], al
    0x26, 0x88, 0x55, 0x04,  # mov es:[di+4], dl
    0x26, 0x88, 0x45, 0x05,  # mov es:[di+5], al
    0xEB, 0xDE,              # jmp loop
])

os.makedirs(outdir, exist_ok=True)
for name, data in (('JOYTEST.COM', JOYTEST), ('MOUSETEST.COM', MOUSETEST)):
    open(os.path.join(outdir, name), 'wb').write(data)
    print(f'{name}: {len(data)} bytes')
