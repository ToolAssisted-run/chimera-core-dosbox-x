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
  POSTEST.COM   INT 33h fn 3 like MOUSETEST, but stores the WHOLE position
                (CX, DX) in the IACA at 0000:04F0, so a run can be asked where
                the DOS cursor is as a number.
  VMWTEST.COM   asks the VMware absolute pointer (port 5658h) for absolute
                mode, then busy-loops reading the cursor position off it and
                stores x, y and the buttons in the IACA at 0000:04F0 - the
                16 bytes IBM reserved for programs to talk to each other and
                DOS never touches - so a run can be asked what position the
                GUEST saw, not only whether the screen changed. It writes the
                low bytes to B800:0000 as well, so the video digest moves too.

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

# The VMware backdoor: EAX carries the magic, CX the command, DX the port, and
# the answer comes back in EAX/EBX/ECX (buttons, x, y). Absolute mode has to be
# asked for first - until a guest asks, the emulator keeps feeding the PS/2
# stream and this reads whatever the port happens to hold.
VMWTEST = bytes([
    0x66, 0xB8, 0x68, 0x58, 0x4D, 0x56,  # mov eax, 564D5868h   (VMWARE_MAGIC)
    0x66, 0xBB, 0x52, 0x41, 0x42, 0x53,  # mov ebx, 53424152h   (ABSPOINTER_ABSOLUTE)
    0x66, 0xB9, 0x29, 0x00, 0x00, 0x00,  # mov ecx, 41          (ABSPOINTER_COMMAND)
    0xBA, 0x58, 0x56,                    # mov dx, 5658h
    0x66, 0xED,                          # in eax, dx
    0x31, 0xC0,                          # xor ax, ax
    0x8E, 0xC0,                          # mov es, ax           (ES = 0, for the IACA)
    0xB8, 0x00, 0xB8,                    # mov ax, 0B800h
    0x8E, 0xD8,                          # mov ds, ax           (DS = text screen)
    0xBF, 0xF0, 0x04,                    # mov di, 04F0h        (the IACA)
    0x31, 0xF6,                          # xor si, si
    # loop:
    0x66, 0xB8, 0x68, 0x58, 0x4D, 0x56,  # mov eax, 564D5868h
    0x66, 0xB9, 0x27, 0x00, 0x00, 0x00,  # mov ecx, 39          (ABSPOINTER_DATA)
    0xBA, 0x58, 0x56,                    # mov dx, 5658h
    0x66, 0xED,                          # in eax, dx           -> AL buttons, BX x, CX y
    0x26, 0x88, 0x45, 0x04,              # mov es:[di+4], al
    0x26, 0x89, 0x1D,                    # mov es:[di], bx
    0x26, 0x89, 0x4D, 0x02,              # mov es:[di+2], cx
    0x88, 0x1C,                          # mov [si], bl         (and onto the screen)
    0x88, 0x4C, 0x02,                    # mov [si+2], cl
    0xB0, 0x07,                          # mov al, 7
    0x88, 0x44, 0x01,                    # mov [si+1], al
    0x88, 0x44, 0x03,                    # mov [si+3], al
    0xEB, 0xD5,                          # jmp loop
])

# The INT 33h cursor as a NUMBER, not as pixels on a screen: the same fn 03h
# MOUSETEST polls, but the full CX/DX stored in the IACA, so a run can be asked
# where the DOS cursor actually is and the answer checked against a position
# worked out independently of the core.
POSTEST = bytes([
    0xB8, 0x00, 0x00,        # mov ax, 0            (reset/detect)
    0xCD, 0x33,              # int 33h
    0x31, 0xC0,              # xor ax, ax
    0x8E, 0xC0,              # mov es, ax           (ES = 0, for the IACA)
    0xBF, 0xF0, 0x04,        # mov di, 04F0h
    # loop:
    0xB8, 0x03, 0x00,        # mov ax, 3            (buttons + position)
    0xCD, 0x33,              # int 33h              -> BX buttons, CX x, DX y
    0x26, 0x89, 0x0D,        # mov es:[di], cx
    0x26, 0x89, 0x55, 0x02,  # mov es:[di+2], dx
    0x26, 0x88, 0x5D, 0x04,  # mov es:[di+4], bl
    0xEB, 0xEE,              # jmp loop
])

# POSTEST, but it switches to 40-column text first (INT 10h AX=0001), which
# halves the mouse's screen range from 640 to 320. The same absolute position
# must then land on the same FRACTION of the screen and therefore half the
# pixel - which is the whole point of a position that is not a fixed number.
MODETEST = bytes([
    0xB8, 0x01, 0x00,        # mov ax, 0001h        (40x25 text)
    0xCD, 0x10,              # int 10h
    0xB8, 0x00, 0x00,        # mov ax, 0
    0xCD, 0x33,              # int 33h              (reset, after the mode set)
    0x31, 0xC0,              # xor ax, ax
    0x8E, 0xC0,              # mov es, ax
    0xBF, 0xF0, 0x04,        # mov di, 04F0h
    # loop:
    0xB8, 0x03, 0x00,        # mov ax, 3
    0xCD, 0x33,              # int 33h
    0x26, 0x89, 0x0D,        # mov es:[di], cx
    0x26, 0x89, 0x55, 0x02,  # mov es:[di+2], dx
    0x26, 0x88, 0x5D, 0x04,  # mov es:[di+4], bl
    0xEB, 0xEE,              # jmp loop
])

# The MICKEYS, accumulated: INT 33h fn 0Bh returns the motion counters and
# clears them, so a polling loop that adds them up in the IACA reports the
# total relative movement the machine was told about. That is the number a
# position and a speed must AGREE on - a position moved ten pixels and a speed
# of ten pixels are the same movement, and if the position path differenced its
# wire instead of its pixels they would differ by about a hundredfold.
MICKTEST = bytes([
    0xB8, 0x00, 0x00,              # mov ax, 0
    0xCD, 0x33,                    # int 33h
    0x31, 0xC0,                    # xor ax, ax
    0x8E, 0xC0,                    # mov es, ax
    0xBF, 0xF0, 0x04,              # mov di, 04F0h
    0x26, 0xC7, 0x05, 0x00, 0x00,  # mov word es:[di], 0
    0x26, 0xC7, 0x45, 0x02, 0x00, 0x00,  # mov word es:[di+2], 0
    # loop:
    0xB8, 0x0B, 0x00,              # mov ax, 0Bh      (read motion counters)
    0xCD, 0x33,                    # int 33h          -> CX dx, DX dy
    0x26, 0x01, 0x0D,              # add es:[di], cx
    0x26, 0x01, 0x55, 0x02,        # add es:[di+2], dx
    0xEB, 0xF2,                    # jmp loop
])

os.makedirs(outdir, exist_ok=True)
for name, data in (('JOYTEST.COM', JOYTEST), ('MOUSETEST.COM', MOUSETEST),
                   ('VMWTEST.COM', VMWTEST), ('POSTEST.COM', POSTEST),
                   ('MODETEST.COM', MODETEST), ('MICKTEST.COM', MICKTEST)):
    open(os.path.join(outdir, name), 'wb').write(data)
    print(f'{name}: {len(data)} bytes')
