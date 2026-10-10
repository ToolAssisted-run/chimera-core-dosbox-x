#!/usr/bin/env python3
"""Generates waterbox.config and default_keybinds.json.

The surface is IMPORTED from the author's BizHawk DOSBox-X integration
(BizHawk src/BizHawk.Emulation.Cores/Computers/DOS + Assets/defctrl.json)
so the two look identical and a finished BizHawk movie converts 1:1:
the same controller name, the same button names in the same order, the
same axes, the same sync settings with the same defaults, and the same
default host bindings.

The button order IS the wire format: joysticks, mouse buttons, disk-swap
controls, then the 102-key keyboard (config index 21+i maps to KBD value
i+1 in the guest's SetButton export). The keyboard list mirrors BizHawk's
DOSBoxKeyboard enum, which is KBD_KEYS order - verified below against the
real enum in extern/dosbox-x/include/keyboard.h. Regenerate only with a
matching guest change; run from waterbox/: python3 gen-config.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- keyboard: BizHawk's DOSBoxKeyboard enum (KBD_KEYS values 1..102) ------
# (kbd token, button name, default host bind - "" ships unbound)
KEYS = []
def K(token, name, bind):
    KEYS.append((token, name, bind))

for d in '1234567890':
    K(f'KBD_{d}', f'Key {d}', f'Number{d}')
for c in 'qwertyuiop' + 'asdfghjkl' + 'zxcvbnm':
    K(f'KBD_{c}', f'Key {c.upper()}', c.upper())
for i in range(1, 13):
    K(f'KBD_f{i}', f'Key F{i}', f'F{i}')
K('KBD_esc', 'Key Escape', 'Escape')
K('KBD_tab', 'Key Tab', 'Tab')
K('KBD_backspace', 'Key Backspace', 'Backspace')
K('KBD_enter', 'Key Enter', 'Enter')
K('KBD_space', 'Key Space', 'Space')
K('KBD_leftalt', 'Key LeftAlt', 'Alt, LeftAlt')
K('KBD_rightalt', 'Key RightAlt', 'RightAlt')
K('KBD_leftctrl', 'Key LeftCtrl', 'Ctrl, LeftCtrl')
K('KBD_rightctrl', 'Key RightCtrl', 'RightCtrl')
K('KBD_leftshift', 'Key LeftShift', 'Shift, LeftShift')
K('KBD_rightshift', 'Key RightShift', 'RightShift')
K('KBD_capslock', 'Key CapsLock', 'CapsLock')
K('KBD_scrolllock', 'Key ScrollLock', 'ScrollLock')
K('KBD_numlock', 'Key NumLock', 'NumLock')
K('KBD_grave', 'Key Grave', 'Backtick')
K('KBD_minus', 'Key Minus', 'Minus')
K('KBD_equals', 'Key Equals', 'Equals')
K('KBD_backslash', 'Key Backslash', 'Backslash')
K('KBD_leftbracket', 'Key LeftBracket', 'LeftBracket')
K('KBD_rightbracket', 'Key RightBracket', 'RightBracket')
K('KBD_semicolon', 'Key Semicolon', 'Semicolon')
K('KBD_quote', 'Key Quote', 'Apostrophe')
K('KBD_period', 'Key Period', 'Period')
K('KBD_comma', 'Key Comma', 'Comma')
K('KBD_slash', 'Key Slash', 'Slash')
K('KBD_extra_lt_gt', 'Key ExtraLtGt', '')
K('KBD_printscreen', 'Key PrintScreen', 'PrintScreen')
K('KBD_pause', 'Key Pause', 'Pause')
K('KBD_insert', 'Key Insert', 'Insert')
K('KBD_home', 'Key Home', 'Home')
K('KBD_pageup', 'Key Pageup', 'PageUp')
K('KBD_delete', 'Key Delete', 'Delete')
K('KBD_end', 'Key End', 'End')
K('KBD_pagedown', 'Key Pagedown', 'PageDown')
K('KBD_left', 'Key Left', 'Left')
K('KBD_up', 'Key Up', 'Up')
K('KBD_down', 'Key Down', 'Down')
K('KBD_right', 'Key Right', 'Right')
for d in '1234567890':
    K(f'KBD_kp{d}', f'Key KeyPad{d}', f'Keypad{d}')
K('KBD_kpdivide', 'Key KeyPadDivide', 'KeypadDivide')
K('KBD_kpmultiply', 'Key KeyPadMultiply', 'KeypadMultiply')
K('KBD_kpminus', 'Key KeyPadMinus', 'KeypadSubtract')
K('KBD_kpplus', 'Key KeyPadPlus', 'KeypadAdd')
K('KBD_kpenter', 'Key KeyPadEnter', 'KeypadEnter')
K('KBD_kpperiod', 'Key KeyPadPeriod', 'KeypadDecimal')
assert len(KEYS) == 102, len(KEYS)

# verify against the real enum: token i must have value i+1
enum = []
src = open(os.path.join(HERE, '../extern/dosbox-x/include/keyboard.h')).read()
body = src.split('enum KBD_KEYS {', 1)[1].split('};', 1)[0]
for line in body.split('\n'):
    line = line.split('/*')[0].split('//')[0]
    for tok in line.replace(',', ' ').split():
        if tok.startswith('KBD_'):
            enum.append(tok)
assert enum[0] == 'KBD_NONE'
for i, (tok, _, _) in enumerate(KEYS):
    assert enum[i + 1] == tok, f'order mismatch at {i}: {enum[i+1]} != {tok}'
print('keyboard order verified against KBD_KEYS')

# ---- the non-keyboard blocks, BizHawk's controller-definition order --------
JOY = [(f'P{p} Joystick {b}', bind)
       for p, pad in ((1, 'J1'), (2, 'J2'))
       for b, bind in (('Up', f'Up, {pad} POV1U, X1 DpadUp, X1 LStickUp' if p == 1 else ''),
                       ('Down', f'Down, {pad} POV1D, X1 DpadDown, X1 LStickDown' if p == 1 else ''),
                       ('Left', f'Left, {pad} POV1L, X1 DpadLeft, X1 LStickLeft' if p == 1 else ''),
                       ('Right', f'Right, {pad} POV1R, X1 DpadRight, X1 LStickRight' if p == 1 else ''),
                       ('Button 1', 'Z, J1 B1, X1 X' if p == 1 else ''),
                       ('Button 2', 'X, J1 B2, X1 A' if p == 1 else ''))]
# Mouse Set Position is what makes Mouse Position X/Y apply: held, the pointer
# is put there and the speeds are ignored; not held, the position is ignored
# and the pointer stays where it is (user-decided, 2026-10-08; chimera#210,
# chimera#211). It ships unbound, like the middle and right buttons.
MOUSE_BTNS = [('Mouse Left Button', 'WMouse L'), ('Mouse Middle Button', ''),
              ('Mouse Right Button', ''), ('Mouse Set Position', '')]
SWAP = [('Previous Floppy Disk', ''), ('Next Floppy Disk', ''), ('Swap Floppy Disk', ''),
        ('Previous CDROM', ''), ('Next CDROM', ''), ('Swap CDROM', '')]

buttons = [n for n, _ in JOY] + [n for n, _ in MOUSE_BTNS] + [n for n, _ in SWAP] \
    + [name for _, name, _ in KEYS]
assert len(buttons) == 124, len(buttons)

# An absolute position on the guest's screen is 0..65535 with neutral 32768 on
# every Chimera core (chimera docs/porting-a-core.md, "A point on the screen").
# A PC changes video mode whenever it likes, so no declared range can be the
# screen; the wire carries a FRACTION of whatever is being drawn and the driver
# converts it against the live mode. Speed stays relative, in pixels.
#
# This replaced BizHawk's MouseAbsoluteScreenWidth/Height plane (2560x2048),
# which the driver then divided by a different number again (800x600), so the
# right-hand two thirds of the window could not be reached at all.
axes = [
    {"name": "Mouse Position X", "min": 0, "max": 65535, "neutral": 32768},
    {"name": "Mouse Position Y", "min": 0, "max": 65535, "neutral": 32768},
    {"name": "Mouse Speed X", "min": -180, "max": 180, "neutral": 0},
    {"name": "Mouse Speed Y", "min": -180, "max": 180, "neutral": 0},
]

# ---- the ten machines, as PRESETS -----------------------------------------
# They used to be a "Configuration Preset" SETTING that the core resolved for
# itself by appending waterbox/conf/dosbox-x.<year>.<model>.conf AFTER
# everything the user had chosen, so the preset silently outranked the grid and
# seven settings had to describe themselves as "auto uses the preset's
# default". They are now a declared preset list: the wizard's Apply WRITES
# these values into the settings and the preset is finished with (chimera
# docs/project.md, "Configuration presets").
#
# MACHINE_NEUTRAL is what a machine key means when that machine's .conf does
# not mention it: base.conf's own value, so a preset that leaves it alone
# describes the same machine the old preset blob did. Every preset carries the
# WHOLE machine, not a layer - applying 1981 after 1997 gives an XT, not an XT
# with an Aptiva's video memory left behind - so these fill in the rest.
# tools/check-preset-machines.py holds every preset to the .conf it came from.
#
# cpuType, cpuCycles, cpuCore, soundBlasterModel, videoCardType and memsizeMB
# are deliberately NOT here: base.conf answers "auto" for the first three and
# every one of the ten machines states its own, so there is no honest neutral
# and a preset that forgot one must fail the assert below rather than inherit a
# made-up number.
MACHINE_NEUTRAL = {
    "memsizeKB": 0,
    "videoMemoryMB": -1, "vesaModelistWidthLimit": 1280, "vesaModelistHeightLimit": 1024,
    "dosVersion": "auto", "hardDriveDataRateLimit": -1, "floppyDriveDataRateLimit": -1,
    "int13FakeIo": False, "cdromInsertionDelayMs": 0,
}
MACHINE_KEYS = sorted(set(MACHINE_NEUTRAL) | {
    "videoCardType", "memsizeMB", "cpuType", "cpuCycles", "cpuCore", "soundBlasterModel"})

# What a 1990s PC needs before a Windows 9x install will drive its disks and
# notice a disc going in: the block the 1997 and 1999 .confs carry, which is
# also the whole of conf/dosbox-x.osconfig.windows95|98.conf (see docs/PLAN.md).
WIN9X = {
    "videoMemoryMB": 8, "vesaModelistWidthLimit": 0, "vesaModelistHeightLimit": 0,
    "dosVersion": "7.1", "hardDriveDataRateLimit": 0, "floppyDriveDataRateLimit": 0,
    "int13FakeIo": True, "cdromInsertionDelayMs": 4000,
}

def machine(id, label, description, **values):
    v = dict(MACHINE_NEUTRAL)
    v["cpuCore"] = "normal"  # every one of the ten .confs says core=normal
    v.update(values)
    missing = [k for k in MACHINE_KEYS if k not in v]
    assert not missing, f'preset {id} does not say what its {missing} is'
    return {"id": id, "label": label, "description": description, "values": v}

PRESETS = [
    machine("1981_ibm_xt5150", "1981 IBM XT 5150",
            "An 8086 processor at 4.77 MHz, 256 KB of memory, a monochrome "
            "MDA video card and only the PC speaker for sound.",
            cpuType="8086", cpuCycles=315, soundBlasterModel="none",
            videoCardType="mda", memsizeMB=0, memsizeKB=256),
    machine("1983_ibm_xt5160", "1983 IBM XT 5160",
            "An 8086 processor at 4.77 MHz, the full 640 KB of memory, a CGA"
            " video card and only the PC speaker.",
            cpuType="8086", cpuCycles=315, soundBlasterModel="none",
            videoCardType="cga", memsizeMB=0, memsizeKB=640),
    machine("1986_ibm_xt5162", "1986 IBM XT 286 5162",
            "A 286-class XT at 6 MHz, 1 MB of memory, an EGA video card and "
            "only the PC speaker.",
            cpuType="8086", cpuCycles=700, soundBlasterModel="none",
            videoCardType="ega", memsizeMB=1, memsizeKB=0),
    machine("1987_ibm_ps2_25", "1987 IBM PS/2 25",
            "An 80186 processor at 8 MHz, 640 KB of memory, MCGA video and a"
            " Creative Game Blaster sound card.",
            cpuType="80186", cpuCycles=1400, soundBlasterModel="gb",
            videoCardType="mcga", memsizeMB=0, memsizeKB=640),
    machine("1990_ibm_ps2_25_286", "1990 IBM PS/2 25 286",
            "A 286 at 10 MHz, 4 MB of memory, VGA video (emulated as an S3 "
            "card) and a Sound Blaster 1.0.",
            cpuType="286", cpuCycles=2300, soundBlasterModel="sb1",
            videoCardType="svga_s3", memsizeMB=4),
    machine("1991_ibm_ps2_25_386", "1991 IBM PS/2 25 386",
            "A 386 at 25 MHz, 6 MB of memory, VGA video (emulated as an S3 "
            "card) and a Sound Blaster 2.0.",
            cpuType="386", cpuCycles=6000, soundBlasterModel="sb2",
            videoCardType="svga_s3", memsizeMB=6),
    machine("1993_ibm_ps2_53_slc2_486", "1993 IBM PS/2 53 SLC2 486",
            "A 486 at 50 MHz, 32 MB of memory, SVGA video and a Sound "
            "Blaster Pro 2. This is the default machine. It is fast enough "
            "for most DOS games and still typical of their time.",
            cpuType="486", cpuCycles=22000, soundBlasterModel="sbpro2",
            videoCardType="svga_s3", memsizeMB=32),
    machine("1994_ibm_ps2_76i_slc2_486", "1994 IBM PS/2 76i SLC2 486",
            "A 486 at 100 MHz, 64 MB of memory, SVGA video and a Sound "
            "Blaster 16.",
            cpuType="486", cpuCycles=77000, soundBlasterModel="sb16",
            videoCardType="svga_s3", memsizeMB=64),
    machine("1997_ibm_aptiva_2140", "1997 IBM Aptiva 2140",
            "A Pentium II at 233 MHz, 96 MB of memory, SVGA video with 8 MB "
            "of video memory and a Sound Blaster 16 ViBRA. The disk and CD-"
            "ROM settings are the ones an installation of Windows 95 or 98 "
            "expects.",
            cpuType="pentium_ii", cpuCycles=200000, soundBlasterModel="sb16vibra",
            videoCardType="svga_s3", memsizeMB=96, **WIN9X),
    machine("1999_ibm_thinkpad_240", "1999 IBM Thinkpad 240",
            "A Pentium III at 300 MHz, 128 MB of memory, an S3 Trio64 video "
            "card with 16 MB of video memory and a Sound Blaster 16 ViBRA. "
            "The disk and CD-ROM settings are the ones an installation of "
            "Windows 95 or 98 expects.",
            cpuType="pentium_iii", cpuCycles=200000, soundBlasterModel="sb16vibra",
            videoCardType="svga_s3trio64", memsizeMB=128, **{**WIN9X, "videoMemoryMB": 16}),
]

CPU_TYPES = [
    "auto", "8086", "8086_prefetch", "80186", "80186_prefetch",
    "286", "286_prefetch", "386", "386_prefetch",
    "486old", "486old_prefetch", "486", "486_prefetch",
    "pentium", "pentium_mmx", "ppro_slow", "pentium_ii", "pentium_iii",
]

VIDEO_CARDS = [
    "mda", "cga", "cga_mono", "cga_rgb", "cga_composite", "cga_composite2",
    "hercules", "hercules_plus", "hercules_incolor", "hercules_color",
    "tandy", "pcjr", "pcjr_composite", "pcjr_composite2", "amstrad",
    "ega", "ega200", "jega", "mcga", "vgaonly",
    "svga_s3", "svga_s386c928", "svga_s3vision864", "svga_s3vision868",
    "svga_s3vision964", "svga_s3vision968", "svga_s3trio32", "svga_s3trio64",
    "svga_s3trio64v+", "svga_s3virge", "svga_s3virgevx",
    "svga_et3000", "svga_et4000", "svga_paradise",
    "vesa_nolfb", "vesa_oldvbe", "vesa_oldvbe10",
    "pc98", "pc9801", "pc9821",
    "svga_ati_egavgawonder", "svga_ati_vgawonder", "svga_ati_vgawonderplus",
    "svga_ati_vgawonderxl", "svga_ati_vgawonderxl24",
    "svga_ati_mach8", "svga_ati_mach32", "svga_ati_mach64", "fm_towns",
]

SB_MODELS = ["none", "sb1", "sb2", "sbpro1", "sbpro2", "sb16",
             "sb16vibra", "gb", "ess688", "reveal_sc400"]

# DOSBox-X's own cores minus the JIT ones: dynamic_x86 is a recompiler and, like
# PPSSPP's JIT, cannot be cross-build deterministic, so it is not compiled in
# (docs/PLAN.md section 10). "auto" would only ever resolve to normal here, and
# a no-op option that looks like a choice is worse than no option.
CPU_CORES = ["normal", "full", "simple"]

# [dos] ver, as base.conf documents it. "auto" is DOSBox-X's own word for
# "pick a DOS kernel version", and it is what an unset ver means.
DOS_VERSIONS = ["auto", "3.3", "5.0", "6.22", "7.0", "7.1"]

# ---- firmware: every file a ROM-backed device opens -----------------------
# The hashes are munt's own (extern/dosbox-x/src/libs/mt32/ROMInfo.cpp): it
# refuses a ROM it does not know, so a wrong dump is said here, not there.
def FW(id, display, description, size, name, sha1, label, when):
    e = {"id": id, "display": display, "description": description, "size": size,
         "name": name, "label": label, "requiredWhen": when}
    if sha1: e["sha1"] = sha1
    return e

def midi_is(v): return {"setting": "midiDevice", "is": v}
def flag(n): return {"setting": n, "is": True}
ROLAND = "Copied from a Roland unit you own. "
FIRMWARE = [
    FW("MT32_CONTROL.ROM", "Roland MT-32 control ROM", ROLAND + "First generation, version 1.07.",
       65536, "MT32_CONTROL.ROM", "B083518FFFB7F66B03C23B7EB4F868E62DC5A987", "MT-32 v1.07", midi_is("mt32_old")),
    FW("MT32_CONTROL.ROM", "Roland MT-32 control ROM", ROLAND + "Second generation, version 2.04.",
       131072, "MT32_CONTROL.ROM", "2C16432B6C73DD2A3947CBA950A0F4C19D6180EB", "MT-32 v2.04", midi_is("mt32_new")),
    FW("MT32_PCM.ROM", "Roland MT-32 PCM ROM", ROLAND + "Every MT-32 has this same PCM ROM.",
       524288, "MT32_PCM.ROM", "F6B1EEBC4B2D200EC6D3D21D51325D5B48C60252", "MT-32 PCM",
       {"setting": "midiDevice", "in": ["mt32_old", "mt32_new"]}),
    FW("CM32L_CONTROL.ROM", "Roland CM-32L control ROM", ROLAND + "CM-32L or LAPC-I, version 1.02.",
       65536, "CM32L_CONTROL.ROM", "A439FBB390DA38CADA95A7CBB1D6CA199CD66EF8", "CM-32L v1.02", midi_is("cm32l")),
    FW("CM32L_PCM.ROM", "Roland CM-32L PCM ROM", ROLAND + "CM-32L, CM-64 or LAPC-I.",
       1048576, "CM32L_PCM.ROM", "289CC298AD532B702461BFC738009D9EBE8025EA", "CM-32L PCM", midi_is("cm32l")),
    FW("FONT.ROM", "PC-98 font ROM", "The character ROM of an NEC PC-98 you own. It holds the 8x8 and "
        "8x16 characters and the 16x16 kanji.",
       288768, "FONT.ROM", "78BA9960F135372825AB7244B5E4E73A810002FF", "NEC PC-98 FONT.ROM", flag("pc98FontRom")),
    FW("SOUND.ROM", "PC-98 sound BIOS", "The BIOS (16 KiB) of a PC-9801-26K or PC-9801-86 sound board you "
        "own.",
       16384, "SOUND.ROM", "D5DBC4FEA3B8367024D363F5351BAECD6ADCD8EF", "NEC PC-9801-26K/86 SOUND.ROM", flag("pc98SoundBios")),
] + [
    FW(f"2608_{n}.wav", f"PC-98 rhythm sample ({what})", "One of the six drum samples inside the sound chip (YM2608) of a "
        "PC-9801-86 sound board, as the WAV files that PC-98 emulators "
        "share. Without them the board's drum channel is silent.",
       size, f"2608_{n}.wav", sha, f"YM2608 {what}", flag("pc98RhythmSamples"))
    for n, what, size, sha in [
        ("bd", "bass drum", 19192, "0A56C142EF40CEC50F3EE56A6E42D0029C9E2818"),
        ("sd", "snare drum", 15558, "3C79663EF74C0B0439D13351326EB1C52A657008"),
        ("top", "top cymbal", 57016, "AA4A8F766A86B830687D5083FD3B9DB0652F46FC"),
        ("hh", "hi-hat", 36722, "12F676CEF249B82480B6F19C454E234B435CA7B6"),
        ("tom", "tom-tom", 23092, "9513FB4A3F41E75A972A273A5104CBD834C1E2C5"),
        ("rim", "rim shot", 5288, "C65592330C9DD84011151DAED52F9AEC926B7E56"),
    ]
] + [
    FW("IBMBASIC.ROM", "IBM ROM BASIC", "The BASIC ROMs (32 KiB) of an IBM 5150 you own, as one image for "
        "address F6000h. The file is often named "
        "IBMROMBASIC-F6000h-1982-10-27.ROM.",
       32768, "IBMROMBASIC-F6000h-1982-10-27.ROM", "07449EBCA18F979B9AB748582B736E402F2BF940", "IBM BASIC C1.10", flag("ibmRomBasic")),
    FW("VGABIOS.BIN", "Video BIOS", "The video BIOS of the card chosen in Video Card Type (1 to 64 KiB),"
        " copied from a card you own. DOSBox-X recognises et4000.bin for "
        "svga_et4000 and the S3 Trio64 BIOS version 1.5-07 for svga_s3.",
       0, "et4000.bin", None, "Video BIOS", flag("vgaBiosRom")),  # size 0: any, a video BIOS is 1 to 64 KiB
]


# ---- what the controls and the system are called ----
# The frontend keeps no table of these: a core says what its own are called.
# MNEMONICS is the letter each button writes into a movie's text and heads its
# input column with, by the button's name - whole, or without its player ("P2
# Up" is found under "Up"), so one line serves every pad. AXIS_HEADERS is the
# short header of each axis's column. (An entry is read by position: a letter
# may change and no movie made before it is harmed.)
MNEMONICS = {
    "Joystick Up": "U", "Joystick Down": "D", "Joystick Left": "L", "Joystick Right": "R",
    "Joystick Button 1": "1", "Joystick Button 2": "2", "Mouse Left Button": "l",
    "Mouse Middle Button": "m", "Mouse Right Button": "r", "Mouse Set Position": "@",
    "Previous Floppy Disk": "<",
    "Next Floppy Disk": ">", "Swap Floppy Disk": "v", "Previous CDROM": "{", "Next CDROM": "}",
    "Swap CDROM": "w", "Key 1": "1", "Key 2": "2", "Key 3": "3", "Key 4": "4", "Key 5": "5",
    "Key 6": "6", "Key 7": "7", "Key 8": "8", "Key 9": "9", "Key 0": "0", "Key Q": "Q",
    "Key W": "W", "Key E": "E", "Key R": "R", "Key T": "T", "Key Y": "Y", "Key U": "U",
    "Key I": "I", "Key O": "O", "Key P": "P", "Key A": "A", "Key S": "S", "Key D": "D",
    "Key F": "F", "Key G": "G", "Key H": "H", "Key J": "J", "Key K": "K", "Key L": "L",
    "Key Z": "Z", "Key X": "X", "Key C": "C", "Key V": "V", "Key B": "B", "Key N": "N",
    "Key M": "M", "Key F1": "1", "Key F2": "2", "Key F3": "3", "Key F4": "4", "Key F5": "5",
    "Key F6": "6", "Key F7": "7", "Key F8": "8", "Key F9": "9", "Key F10": "0", "Key F11": "1",
    "Key F12": "2", "Key Escape": "e", "Key Tab": "t", "Key Backspace": "b", "Key Enter": "e",
    "Key Space": "s", "Key LeftAlt": "a", "Key RightAlt": "a", "Key LeftCtrl": "c",
    "Key RightCtrl": "c", "Key LeftShift": "s", "Key RightShift": "s", "Key CapsLock": "C",
    "Key ScrollLock": "S", "Key NumLock": "N", "Key Grave": "`", "Key Minus": "-",
    "Key Equals": "=", "Key Backslash": "b", "Key LeftBracket": "[", "Key RightBracket": "]",
    "Key Semicolon": ";", "Key Quote": "'", "Key Period": "p", "Key Comma": ",", "Key Slash": "/",
    "Key ExtraLtGt": ">", "Key PrintScreen": "p", "Key Pause": "P", "Key Insert": "i",
    "Key Home": "h", "Key Pageup": "p", "Key Delete": "d", "Key End": "e", "Key Pagedown": "p",
    "Key Left": "<", "Key Up": "^", "Key Down": "v", "Key Right": ">", "Key KeyPad1": "1",
    "Key KeyPad2": "2", "Key KeyPad3": "3", "Key KeyPad4": "4", "Key KeyPad5": "5",
    "Key KeyPad6": "6", "Key KeyPad7": "7", "Key KeyPad8": "8", "Key KeyPad9": "9",
    "Key KeyPad0": "0", "Key KeyPadDivide": "/", "Key KeyPadMultiply": "*", "Key KeyPadMinus": "-",
    "Key KeyPadPlus": "+", "Key KeyPadEnter": "e", "Key KeyPadPeriod": "p",
}
AXIS_HEADERS = {
    "Mouse Position X": "mpX", "Mouse Position Y": "mpY", "Mouse Speed X": "msX",
    "Mouse Speed Y": "msY",
}
SYSTEM_NAMES = {
    "DOS": "MS-DOS",
}


def _bare(name):
    """A control's name without its player: "P2 Up" -> "Up"."""
    head, _, rest = name.partition(" ")
    return rest if rest and head[:1] == "P" and head[1:].isdigit() else name


def mnemonics_for(buttons):
    """The "mnemonics" of an input declaration: a letter for every one of its
    buttons, and for nothing else. A button nobody gave a letter stops the
    build - the engine would give it its rule's guess, and two columns of one
    pad would share a letter with nobody having decided it."""
    out = {}
    for b in buttons:
        key = b if b in MNEMONICS else _bare(b)
        if key not in MNEMONICS:
            raise SystemExit("no mnemonic for the button %r (MNEMONICS in %s)" % (b, __file__))
        out[key] = MNEMONICS[key]
    return out


def with_headers(axes):
    """The axes with their column headers; an axis nobody named stops the build."""
    missing = [a["name"] for a in axes if a["name"] not in AXIS_HEADERS]
    if missing:
        raise SystemExit("no header for the axes %s (AXIS_HEADERS in %s)" % (missing, __file__))
    return [dict(a, header=AXIS_HEADERS[a["name"]]) for a in axes]


config = {
    "coreName": "DOSBox-X",
    "systemId": "DOS",
    "systemNames": SYSTEM_NAMES,
    "author": "DOSBox-X team; chimera port by Sergio Martin",
    "url": "https://github.com/ToolAssisted-run/chimera-core-dosbox-x",
    "romFile": "rom",
    "deterministic": True,
    "memoryLayoutMiB": [256, 16, 16, 64, 1024],
    "video": {
        "_comment": "the BUFFER capacity (BizHawk's SVGA_MAX plane); the live size comes from GetVideoWidth/Height per frame",
        "width": 2560, "height": 2048,
        "virtualWidth": 1024, "virtualHeight": 768,
        "vsyncNumerator": 3146888, "vsyncDenominator": 44900,
        "getBgra": "GetVideoBgra"
    },
    "audio": {"samplesPerFrame": 8192, "channels": 2, "get": "GetAudio"},
    "input": {
        "name": "DOSBox Controller",
        "_comment": "index order is the wire format, imported from BizHawk's controller definition: joysticks, mouse buttons, disk-swap controls, then the 102-key keyboard (KBD_KEYS 1..102, see gen-config.py). Wider than 64, so everything rides the SetButton channel.",
        "buttons": buttons,
        "mnemonics": mnemonics_for(buttons),
        "axes": with_headers(axes),
        "_axes_note": "Mouse position is an absolute point on the guest screen, 0..65535 across whatever the machine is drawing right now - a DOS box changes video mode whenever it likes, so the wire carries a fraction and the driver converts it against the live mode. It applies only while the Mouse Set Position button is held: then the pointer is put there and the speeds are ignored. While the button is not held the position is ignored, the pointer stays where it is, and Mouse Speed - the per-frame relative movement, in guest pixels - moves it. The frontend feeds these through SetAxis before every frame."
    },
    "extensions": {
        ".ima": "DOS", ".img": "DOS", ".xdf": "DOS", ".fdi": "DOS",
        ".hdd": "DOS", ".conf": "DOS"
    },
    "presets": PRESETS,
    "settings": [
        {
            "name": "joystick1Enabled", "display": "Enable Joystick 1",
            "description": "Whether a joystick is plugged into game port 1.",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "joystick2Enabled", "display": "Enable Joystick 2",
            "description": "Whether a joystick is plugged into game port 2.",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "mouseEnabled", "display": "Enable Mouse",
            "description": "Whether a mouse is plugged in.",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "mouseSensitivity", "display": "Mouse Relative Sensitivity",
            "description": "Every relative mouse movement is multiplied by this number "
                "before the machine receives it. It applies to Mouse Speed "
                "X/Y and to the movement that Mouse Set Position causes. It "
                "changes how far the mouse has to travel, not where the "
                "pointer ends: a position set with Mouse Set Position still "
                "lands where it says. The value used to be 3.0 (the value in"
                " the BizHawk version of this core), which moved the DOS "
                "pointer about three times as far as asked.",
            "type": "float", "default": 0.5, "sync": True
        },
        {
            "name": "chimeraMouseDriver", "display": "Use Chimera Mouse Driver",
            "description": "Lets Mouse Position (with Mouse Set Position held) place "
                "the pointer exactly in Windows 3.1, 95 and 98. Without it "
                "the pointer is pushed through Windows' own mouse "
                "acceleration and does not land exactly. With this on, drive"
                " B: holds Chimera's mouse drivers and their installer. "
                "Before anything else starts, the installer puts the right "
                "driver into the Windows found on drive C: (CHIMABS.EXE, "
                "started from WIN.INI, for Windows 95 and 98, and VBADOS' "
                "VBMOUSE.DRV for Windows 3.1). It writes to C: only when the"
                " driver is not there yet, and it leaves a disk without "
                "Windows alone. Turning this off removes drive B: but does "
                "not remove an installed driver. It is not available on "
                "PC-98 machines, which these drivers are not made for. "
                "B:\\README.TXT says more, and B:\\INSTALL installs the driver"
                " by hand.",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "formattedHardDisk", "display": "Mount Formatted Hard Disk Drive",
            "description": "Whether an empty, formatted, writable hard disk is put in "
                "as drive C:. The disk is kept completely in memory, so make"
                " sure your computer has enough memory. Its contents are "
                "this core's save data (Emulator > Export Save Data). This "
                "setting is ignored when a hard disk image is given.",
            "type": "enum",
            "options": ["none", "21mb", "41mb", "241mb", "504mb", "2014mb"],
            "default": "none", "sync": True
        },
        {
            "name": "forceFPSNumerator", "display": "Force FPS Numerator",
            "description": "The top number of a forced frame rate (frames per second of"
                " emulated time, as a fraction). Leave it unchanged to "
                "follow the machine's own video refresh rate, which is "
                "recommended. To force a rate, set both this and Force FPS "
                "Denominator.",
            "type": "int", "default": 0, "sync": True
        },
        {
            "name": "forceFPSDenominator", "display": "Force FPS Denominator",
            "description": "The bottom number of a forced frame rate. Leave it "
                "unchanged to follow the machine's own video refresh rate, "
                "which is recommended. To force a rate, set both this and "
                "Force FPS Numerator.",
            "type": "int", "default": 0, "sync": True
        },
        {
            "name": "cpuCycles", "display": "CPU Cycles",
            "description": "How many processor cycles the machine executes in each "
                "emulated millisecond. DOSBox measures processor speed this "
                "way and not in MHz. Rough values are 315 for a 4.77 MHz "
                "8086, 2300 for a 10 MHz 286, 22000 for a 50 MHz 486 and "
                "200000 for a late Pentium. Lower it for a game that runs "
                "too fast and raise it for one that stutters. It is always a"
                " fixed number. DOSBox-X's 'auto' and 'max' follow the speed"
                " of your computer, which a movie could not reproduce.",
            "type": "int", "default": 22000, "min": 1, "max": 10000000, "sync": True
        },
        {
            "name": "cpuType", "display": "CPU Type",
            "description": "Which x86 processor the machine has. It decides which "
                "instructions a program can use, not how fast it runs (that "
                "is CPU Cycles). 'auto' lets DOSBox-X choose the type that "
                "fits the rest of the machine. The choices ending in "
                "'_prefetch' also imitate the real chip's instruction queue,"
                " which a few programs that depend on exact timing need.",
            "type": "enum", "options": CPU_TYPES, "default": "486", "sync": True
        },
        {
            "name": "cpuCore", "display": "CPU Core",
            "description": "Which of DOSBox-X's interpreters runs the x86 code. "
                "'normal' is the accurate one and is used by every machine "
                "preset here. 'simple' is faster and only for real-mode "
                "software. 'full' is the slowest and the most exact. "
                "DOSBox-X's recompiling ('dynamic') cores are not included, "
                "because they cannot be made to give the same result on "
                "every computer, and a movie needs that.",
            "type": "enum", "options": CPU_CORES, "default": "normal", "sync": True
        },
        {
            "name": "videoCardType", "display": "Video Card Type",
            "description": "Which video card the machine has. It decides which video "
                "modes a game can use. 'mda' is monochrome text only, 'cga' "
                "has four colours and 'ega' sixteen. 'mcga' and 'vgaonly' "
                "are the plain PS/2 and VGA cards. The svga_ choices are the"
                " faster cards of the 1990s, and an ordinary VGA game works "
                "well on svga_s3. 'pc98', 'pc9801' and 'pc9821' turn the "
                "machine into an NEC PC-98 instead of an IBM PC.",
            "type": "enum", "options": VIDEO_CARDS, "default": "svga_s3", "sync": True
        },
        {
            "name": "memsizeMB", "display": "RAM Size (MB)",
            "description": "The memory size in whole megabytes. Computers of the time "
                "had very little: 0 for a PC from before 1987 (RAM Size (KB)"
                " then gives the real amount), 1 to 8 for the 286 and 386 "
                "years, and 32 to 128 for a late Pentium. More memory is not"
                " always better, because some DOS games refuse to start when"
                " they find more than they expect.",
            "type": "int", "default": 32, "min": 0, "max": 256, "sync": True
        },
        {
            "name": "memsizeKB", "display": "RAM Size (KB)",
            "description": "Kilobytes of memory ADDED to RAM Size (MB), for machines "
                "with less than a megabyte: 256 for a 1981 PC and 640 for an"
                " XT or a PS/2 25. Leave it at 0 for anything from 1986 on, "
                "where RAM Size (MB) is enough.",
            "type": "int", "default": 0, "min": 0, "max": 262144, "sync": True
        },
        {
            "name": "pcSpeaker", "display": "PC Speaker",
            "description": "Whether the machine's built-in speaker is connected. Every "
                "machine here had one, and DOS games from before sound cards"
                " play their music on it. Turning it off silences that music"
                " and changes nothing else.",
            "type": "enum", "options": ["disabled", "enabled"],
            "default": "enabled", "sync": True
        },
        {
            "name": "soundBlasterModel", "display": "Sound Blaster Model",
            "description": "Which sound card the machine has. 'none' means no card, as "
                "in PCs before 1987. 'gb' is Creative's Game Blaster. 'sb1' "
                "and 'sb2' are the 8-bit Sound Blasters, 'sbpro1' and "
                "'sbpro2' the stereo Sound Blaster Pro cards, and 'sb16' and"
                " 'sb16vibra' the 16-bit ones. Choose the card the game was "
                "written for. A game from 1990 does not use the extra "
                "features of an sb16, and a game from 1995 may refuse an "
                "sb1.",
            "type": "enum", "options": SB_MODELS, "default": "sbpro2", "sync": True
        },
        {
            "name": "soundBlasterIRQ", "display": "Sound Blaster IRQ",
            "description": "The interrupt line (IRQ) the Sound Blaster uses. -1 uses "
                "the card model's factory default (7 for the early cards, 5 "
                "for a Sound Blaster 16), which is what a game's setup "
                "program expects.",
            "type": "int", "default": -1, "min": -1, "max": 15, "sync": True
        },
        {
            "name": "videoMemoryMB", "display": "Video Memory (MB)",
            "description": "How many megabytes of memory the video card has. This "
                "limits the resolution and number of colours an SVGA card "
                "offers: 1 MB allows 1024x768 in 256 colours, 2 MB allows "
                "640x480 in true colour and 8 MB allows 1600x1200 in true "
                "colour. -1 uses the amount the chosen card was sold with. "
                "Cards older than VGA have a fixed amount and ignore this.",
            "type": "int", "default": -1, "min": -1, "max": 64, "sync": True
        },
        {
            "name": "vesaModelistWidthLimit", "display": "VESA Mode List Width Limit",
            "description": "Hides VESA video modes wider than this many pixels from the"
                " list a program gets. Some DOS programs cannot handle a "
                "long list or a very large mode. 1280 is DOSBox-X's own "
                "limit. 0 lists every mode the card has, which a Windows 95 "
                "or 98 display driver needs.",
            "type": "int", "default": 1280, "min": 0, "max": 4096, "sync": True
        },
        {
            "name": "vesaModelistHeightLimit", "display": "VESA Mode List Height Limit",
            "description": "The same limit for the height of VESA video modes. 1024 is "
                "DOSBox-X's own limit, and 0 lists every mode.",
            "type": "int", "default": 1024, "min": 0, "max": 4096, "sync": True
        },
        {
            "name": "dosVersion", "display": "Reported DOS Version",
            "description": "The version number DOSBox-X's built-in DOS reports to "
                "programs. 'auto' lets it choose (currently 5.0, the safest "
                "for DOS games). 6.22 is the last real MS-DOS. 7.0 and 7.1 "
                "are the DOS versions inside Windows 95 and 98, which some "
                "installers and programs that use long file names look for. "
                "It has no effect when the machine starts a DOS of its own "
                "from a disk.",
            "type": "enum", "options": DOS_VERSIONS, "default": "auto", "sync": True
        },
        {
            "name": "hardDriveDataRateLimit", "display": "Hard Disk Data Rate (bytes/s)",
            "description": "Limits the hard disk to this many bytes per second, so that"
                " a game takes as long to read from disk as it did on a real"
                " machine. -1 uses DOSBox-X's own limit for a machine of the"
                " period. 0 removes the limit, which an installation of "
                "Windows 95 or 98 needs and which is what plain DOSBox does.",
            "type": "int", "default": -1, "min": -1, "max": 1000000000, "sync": True
        },
        {
            "name": "floppyDriveDataRateLimit", "display": "Floppy Data Rate (bytes/s)",
            "description": "The same limit for the floppy drives. -1 uses DOSBox-X's "
                "own limit, and 0 makes floppy reads instant.",
            "type": "int", "default": -1, "min": -1, "max": 1000000000, "sync": True
        },
        {
            "name": "int13FakeIo", "display": "Fake INT 13h Disk I/O",
            "description": "Makes the emulated hard disk and floppy controllers respond"
                " to BIOS disk calls the way real hardware does. The 32-bit "
                "disk access of Windows 3.11 and Windows 95 needs this and "
                "cannot use the disks without it. A plain DOS machine does "
                "not need it, and turning it on there does nothing and costs"
                " nothing. In DOSBox-X's terms it sets int13fakeio and "
                "int13fakev86io on both IDE channels and int13fakev86io on "
                "the floppy controller.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "cdromInsertionDelayMs", "display": "CD-ROM Insertion Delay (ms)",
            "description": "How long the CD-ROM drive reports that it is empty after a "
                "disc is changed, in milliseconds. This stands for the time "
                "a person takes to change the disc. 0 uses the drive's "
                "default, which is no delay. Windows 95 and later need about"
                " 4000 to notice a new disc automatically.",
            "type": "int", "default": 0, "min": 0, "max": 60000, "sync": True
        },
        {
            "name": "bootDrive", "display": "Boot From",
            "description": "Starts the machine from a drive instead of showing the DOS "
                "prompt. 'a' starts from a bootable floppy image and 'c' "
                "starts an operating system installed on the hard disk. "
                "'none' keeps DOSBox-X's built-in DOS prompt. This setting "
                "was added for Chimera, and BizHawk movies use 'none'.",
            "type": "enum", "options": ["none", "a", "c"], "default": "none",
            "sync": True
        },
        {
            "name": "initialDrive", "display": "Initial Drive",
            "description": "The drive the DOS prompt starts on. 'auto' uses the first "
                "drive the project put a disk in, looking in the order A: (a"
                " floppy), D: (a CD) and C: (the hard disk). If a drive is "
                "named here and has no disk, the same order is used. Z:, "
                "DOSBox-X's own drive of built-in commands, is never "
                "offered, because a project always has a disk or gets a "
                "formatted one. The one exception is a project made only of "
                "a configuration file, which stays on Z:. The starting drive"
                " changes what the prompt shows, so a movie recorded before "
                "this setting existed plays back on a machine whose prompt "
                "looks different. Set it to match if that matters. It is "
                "ignored when Boot From starts an operating system, because "
                "the prompt is then never shown.",
            "type": "enum", "options": ["auto", "a", "c", "d"], "default": "auto",
            "sync": True
        },
        # ---- devices that are nothing without their ROM (chimera additions;
        # the defaults change nothing, so a BizHawk movie's machine is intact)
        {
            "name": "midiDevice", "display": "MIDI Device",
            "description": "What is connected to the machine's MIDI port (MPU-401). "
                "'none' leaves the port unconnected, as every one of these "
                "machines was sold. 'mt32_old' is a first-generation Roland "
                "MT-32 (control ROM version 1.07), whose peculiarities the "
                "earliest Sierra games rely on. 'mt32_new' is the second "
                "generation (version 2.04). 'cm32l' is a Roland CM-32L or "
                "LAPC-I (version 1.02), an MT-32 with the extra sound "
                "effects that later games use. Each needs its control ROM "
                "and PCM ROM, copied from a unit you own.",
            "type": "enum", "options": ["none", "mt32_old", "mt32_new", "cm32l"],
            "default": "none", "sync": True
        },
        {
            "name": "pc98FontRom", "display": "Use PC-98 Font ROM",
            "description": "Draws PC-98 text with a real NEC character ROM (FONT.ROM) "
                "instead of the built-in free font. It only has an effect "
                "when Video Card Type is one of the pc98 choices.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "pc98SoundBios", "display": "Use PC-98 Sound BIOS",
            "description": "Adds the BIOS of the PC-9801-26K/86 sound board (SOUND.ROM)"
                " to the machine, at address CC000h. The board's FM sound "
                "works without it. Games that call the sound BIOS need it. "
                "It only has an effect when Video Card Type is one of the "
                "pc98 choices.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "pc98RhythmSamples", "display": "Use PC-98 Rhythm Samples",
            "description": "Gives the sound chip of the PC-9801-86 board (YM2608) its "
                "six built-in drum samples (2608_bd, sd, top, hh, tom and "
                "rim .wav). Without them FM music plays with no drums. It "
                "only has an effect when Video Card Type is one of the pc98 "
                "choices.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "ibmRomBasic", "display": "Use IBM ROM BASIC",
            "description": "Loads IBM's Cassette/ROM BASIC into the machine at address "
                "F6000h, below the BIOS, as an IBM 5150 has it. PC-DOS's "
                "BASICA needs it, and so does starting the machine with no "
                "disk.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "vgaBiosRom", "display": "Use Real Video BIOS",
            "description": "Uses a real video BIOS copied from the card chosen in Video"
                " Card Type, instead of the one DOSBox-X makes itself. It is"
                " for software that examines the card's BIOS.",
            "type": "bool", "default": False, "sync": True
        }
    ],
    # Each id is the file name DOSBox-X itself opens, in the work directory.
    "firmware": FIRMWARE
}

with open(os.path.join(HERE, 'waterbox.config'), 'w') as f:
    json.dump(config, f, indent=2)
    f.write('\n')

# ---- default_keybinds.json: BizHawk Assets/defctrl.json, verbatim ----------
binds = {n: b for n, b in JOY}
binds.update({n: b for n, b in MOUSE_BTNS})
binds.update({n: b for n, b in SWAP})
binds.update({name: bind for _, name, bind in KEYS})
analog = {
    "Mouse Position X": {"Value": "WMouse X", "Mult": 1.0, "Deadzone": 0.0},
    "Mouse Position Y": {"Value": "WMouse Y", "Mult": 1.0, "Deadzone": 0.0},
    "Mouse Speed X": {"Value": "RMouse X", "Mult": 1.0, "Deadzone": 0.0},
    "Mouse Speed Y": {"Value": "RMouse Y", "Mult": 1.0, "Deadzone": 0.0},
}
keybinds = {
    "_comment": [
        "Default bindings for the DOSBox machine, imported verbatim from the",
        "author's BizHawk integration (Assets/defctrl.json, DOSBox Controller):",
        "P1 joystick on host arrows/gamepad, left mouse button and mouse axes on",
        "the host mouse, the keyboard 1:1. P2 joystick, extra mouse buttons and",
        "disk-swap controls ship unbound."
    ],
    "AllTrollers": {"DOSBox Controller": binds},
    "AllTrollersAutoFire": {"DOSBox Controller": {}},
    "AllTrollersAnalog": {"DOSBox Controller": analog},
}
with open(os.path.join(HERE, 'default_keybinds.json'), 'w') as f:
    json.dump(keybinds, f, indent=2)
    f.write('\n')

print(f'waterbox.config: {len(buttons)} buttons, {len(axes)} axes')
print('default_keybinds.json written')
