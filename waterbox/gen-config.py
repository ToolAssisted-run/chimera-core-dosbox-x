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
MOUSE_BTNS = [('Mouse Left Button', 'WMouse L'), ('Mouse Middle Button', ''),
              ('Mouse Right Button', '')]
SWAP = [('Previous Floppy Disk', ''), ('Next Floppy Disk', ''), ('Swap Floppy Disk', ''),
        ('Previous CDROM', ''), ('Next CDROM', ''), ('Swap CDROM', '')]

buttons = [n for n, _ in JOY] + [n for n, _ in MOUSE_BTNS] + [n for n, _ in SWAP] \
    + [name for _, name, _ in KEYS]
assert len(buttons) == 123, len(buttons)

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
            "An 8086 at 4.77 MHz, 256 KB of RAM, a monochrome MDA card and nothing but the PC speaker.",
            cpuType="8086", cpuCycles=315, soundBlasterModel="none",
            videoCardType="mda", memsizeMB=0, memsizeKB=256),
    machine("1983_ibm_xt5160", "1983 IBM XT 5160",
            "An 8086 at 4.77 MHz, the full 640 KB of RAM, CGA, PC speaker only.",
            cpuType="8086", cpuCycles=315, soundBlasterModel="none",
            videoCardType="cga", memsizeMB=0, memsizeKB=640),
    machine("1986_ibm_xt5162", "1986 IBM XT 286 5162",
            "An 80286-class XT at 6 MHz, 1 MB of RAM, EGA, PC speaker only.",
            cpuType="8086", cpuCycles=700, soundBlasterModel="none",
            videoCardType="ega", memsizeMB=1, memsizeKB=0),
    machine("1987_ibm_ps2_25", "1987 IBM PS/2 25",
            "An 80186 at 8 MHz, 640 KB of RAM, MCGA, and a Creative Game Blaster.",
            cpuType="80186", cpuCycles=1400, soundBlasterModel="gb",
            videoCardType="mcga", memsizeMB=0, memsizeKB=640),
    machine("1990_ibm_ps2_25_286", "1990 IBM PS/2 25 286",
            "A 286 at 10 MHz, 4 MB of RAM, VGA (emulated as an S3), and a Sound Blaster 1.0.",
            cpuType="286", cpuCycles=2300, soundBlasterModel="sb1",
            videoCardType="svga_s3", memsizeMB=4),
    machine("1991_ibm_ps2_25_386", "1991 IBM PS/2 25 386",
            "A 386 at 25 MHz, 6 MB of RAM, VGA (emulated as an S3), and a Sound Blaster 2.0.",
            cpuType="386", cpuCycles=6000, soundBlasterModel="sb2",
            videoCardType="svga_s3", memsizeMB=6),
    machine("1993_ibm_ps2_53_slc2_486", "1993 IBM PS/2 53 SLC2 486",
            "A 486 at 50 MHz, 32 MB of RAM, SVGA, and a Sound Blaster Pro 2. The default machine: fast enough for most DOS games without being an anachronism.",
            cpuType="486", cpuCycles=22000, soundBlasterModel="sbpro2",
            videoCardType="svga_s3", memsizeMB=32),
    machine("1994_ibm_ps2_76i_slc2_486", "1994 IBM PS/2 76i SLC2 486",
            "A 486 at 100 MHz, 64 MB of RAM, SVGA, and a Sound Blaster 16.",
            cpuType="486", cpuCycles=77000, soundBlasterModel="sb16",
            videoCardType="svga_s3", memsizeMB=64),
    machine("1997_ibm_aptiva_2140", "1997 IBM Aptiva 2140",
            "A Pentium II at 233 MHz, 96 MB of RAM, SVGA with 8 MB of video memory, a Sound Blaster 16 ViBRA, and the disk and CD-ROM behaviour a Windows 95 or 98 install expects.",
            cpuType="pentium_ii", cpuCycles=200000, soundBlasterModel="sb16vibra",
            videoCardType="svga_s3", memsizeMB=96, **WIN9X),
    machine("1999_ibm_thinkpad_240", "1999 IBM Thinkpad 240",
            "A Pentium III at 300 MHz, 128 MB of RAM, an S3 Trio64 with 16 MB of video memory, a Sound Blaster 16 ViBRA, and the disk and CD-ROM behaviour a Windows 95 or 98 install expects.",
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
ROLAND = "Dumped from a Roland unit you own. "
FIRMWARE = [
    FW("MT32_CONTROL.ROM", "Roland MT-32 control ROM", ROLAND + "First generation, v1.07.",
       65536, "MT32_CONTROL.ROM", "B083518FFFB7F66B03C23B7EB4F868E62DC5A987", "MT-32 v1.07", midi_is("mt32_old")),
    FW("MT32_CONTROL.ROM", "Roland MT-32 control ROM", ROLAND + "Second generation, v2.04.",
       131072, "MT32_CONTROL.ROM", "2C16432B6C73DD2A3947CBA950A0F4C19D6180EB", "MT-32 v2.04", midi_is("mt32_new")),
    FW("MT32_PCM.ROM", "Roland MT-32 PCM ROM", ROLAND + "The one PCM ROM every MT-32 has.",
       524288, "MT32_PCM.ROM", "F6B1EEBC4B2D200EC6D3D21D51325D5B48C60252", "MT-32 PCM",
       {"setting": "midiDevice", "in": ["mt32_old", "mt32_new"]}),
    FW("CM32L_CONTROL.ROM", "Roland CM-32L control ROM", ROLAND + "CM-32L / LAPC-I, v1.02.",
       65536, "CM32L_CONTROL.ROM", "A439FBB390DA38CADA95A7CBB1D6CA199CD66EF8", "CM-32L v1.02", midi_is("cm32l")),
    FW("CM32L_PCM.ROM", "Roland CM-32L PCM ROM", ROLAND + "CM-32L / CM-64 / LAPC-I.",
       1048576, "CM32L_PCM.ROM", "289CC298AD532B702461BFC738009D9EBE8025EA", "CM-32L PCM", midi_is("cm32l")),
    FW("FONT.ROM", "PC-98 font ROM", "The character ROM of an NEC PC-98 you own: 8x8, 8x16 and the 16x16 kanji.",
       288768, "FONT.ROM", "78BA9960F135372825AB7244B5E4E73A810002FF", "NEC PC-98 FONT.ROM", flag("pc98FontRom")),
    FW("SOUND.ROM", "PC-98 sound BIOS", "The 16 KiB BIOS of a PC-9801-26K or -86 sound board you own.",
       16384, "SOUND.ROM", "D5DBC4FEA3B8367024D363F5351BAECD6ADCD8EF", "NEC PC-9801-26K/86 SOUND.ROM", flag("pc98SoundBios")),
] + [
    FW(f"2608_{n}.wav", f"PC-98 rhythm sample ({what})", "One of the six drum samples inside the YM2608 (OPNA) of a PC-9801-86 sound board, as the WAV files PC-98 emulators share. Without them the board plays with its rhythm channel silent.",
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
    FW("IBMBASIC.ROM", "IBM ROM BASIC", "The 32 KiB BASIC ROM set of an IBM 5150 you own, as one image for F6000h (often named IBMROMBASIC-F6000h-1982-10-27.ROM).",
       32768, "IBMROMBASIC-F6000h-1982-10-27.ROM", "07449EBCA18F979B9AB748582B736E402F2BF940", "IBM BASIC C1.10", flag("ibmRomBasic")),
    FW("VGABIOS.BIN", "Video BIOS", "The video BIOS of the card chosen in Video Card Type, 1 to 64 KiB, dumped from a card you own. DOSBox-X knows et4000.bin for svga_et4000 and the S3 Trio64 v1.5-07 BIOS for svga_s3.",
       0, "et4000.bin", None, "Video BIOS", flag("vgaBiosRom")),  # size 0: any, a video BIOS is 1 to 64 KiB
]


config = {
    "coreName": "DOSBox-X",
    "systemId": "DOS",
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
        "axes": axes,
        "_axes_note": "Mouse position is an absolute point on the guest screen, 0..65535 across whatever the machine is drawing right now - a DOS box changes video mode whenever it likes, so the wire carries a fraction and the driver converts it against the live mode. Speed is the per-frame relative movement, in guest pixels. The frontend feeds these through SetAxis before every frame."
    },
    "extensions": {
        ".ima": "DOS", ".img": "DOS", ".xdf": "DOS", ".fdi": "DOS",
        ".hdd": "DOS", ".conf": "DOS"
    },
    "presets": PRESETS,
    "settings": [
        {
            "name": "joystick1Enabled", "display": "Enable Joystick 1",
            "description": "Determines whether a joystick will be plugged in the IBM PC Gameport 1",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "joystick2Enabled", "display": "Enable Joystick 2",
            "description": "Determines whether a joystick will be plugged in the IBM PC Gameport 2",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "mouseEnabled", "display": "Enable Mouse",
            "description": "Determines whether a mouse will be plugged in",
            "type": "bool", "default": True, "sync": True
        },
        {
            "name": "mouseSensitivity", "display": "Mouse Relative Sensitivity",
            "description": "Multiplies every relative mouse movement before the machine sees it, in mickeys. It applies to Mouse Speed X/Y and to the movement Mouse Position X/Y implies, so it scales the pointer's whole travel, not its destination: an absolute position still lands where it says, but it takes this many times as many mickeys to get there. Was 3.0, which is the BizHawk integration's value and moved the DOS cursor about three times as far as the host pointer asked for.",
            "type": "float", "default": 0.5, "sync": True
        },
        {
            "name": "formattedHardDisk", "display": "Mount Formatted Hard Disk Drive",
            "description": "Determines whether to mount an empty writable formatted hard disk in drive C:. The hard disk will be fully located in memory so make sure you have enough RAM available. Its contents are this core's save data (Emulator > Export Save Data). This value will be ignored if a hard disk image (.hdd) is provided.",
            "type": "enum",
            "options": ["none", "21mb", "41mb", "241mb", "504mb", "2014mb"],
            "default": "none", "sync": True
        },
        {
            "name": "forceFPSNumerator", "display": "Force FPS Numerator",
            "description": "Forces a numerator for FPS: how many frontend frames to run per second of emulation. We recommend leaving this value unmodified, to follow the core's own video refresh rate. Set both numerator and denominator to force.",
            "type": "int", "default": 0, "sync": True
        },
        {
            "name": "forceFPSDenominator", "display": "Force FPS Denominator",
            "description": "Forces a denominator for FPS: how many frontend frames to run per second of emulation. We recommend leaving this value unmodified, to follow the core's own video refresh rate. Set both numerator and denominator to force.",
            "type": "int", "default": 0, "sync": True
        },
        {
            "name": "cpuCycles", "display": "CPU Cycles",
            "description": "How many CPU cycles the machine executes per emulated millisecond - this, not a clock speed, is how DOSBox measures a CPU. Roughly 315 for a 4.77 MHz 8086, 2300 for a 10 MHz 286, 22000 for a 50 MHz 486, 200000 for a late Pentium. A game that runs too fast wants fewer; one that stutters wants more. Always a fixed count: DOSBox-X's 'auto' and 'max' settings chase the host's own speed, which a movie cannot reproduce.",
            "type": "int", "default": 22000, "min": 1, "max": 10000000, "sync": True
        },
        {
            "name": "cpuType", "display": "CPU Type",
            "description": "Which x86 the machine is. This decides the instruction set a program may use, not how fast it runs (that is CPU Cycles). 'auto' lets DOSBox-X pick the type that suits the rest of the machine. The '_prefetch' variants add the real chip's prefetch queue, which a few timing-sensitive titles need.",
            "type": "enum", "options": CPU_TYPES, "default": "486", "sync": True
        },
        {
            "name": "cpuCore", "display": "CPU Core",
            "description": "The interpreter that executes the x86. 'normal' is the accurate one and what every machine here uses; 'simple' is a faster one for real-mode-only software; 'full' is the slowest and most literal. DOSBox-X's recompiling ('dynamic') cores are not built into this core: a JIT cannot be made to produce the same result on every machine, and a movie needs it to.",
            "type": "enum", "options": CPU_CORES, "default": "normal", "sync": True
        },
        {
            "name": "videoCardType", "display": "Video Card Type",
            "description": "Which display adapter is fitted. This is the machine's video hardware, so it decides what modes a game can find: 'mda' is monochrome text only, 'cga' four colours, 'ega' sixteen, 'mcga'/'vgaonly' the plain PS/2 and VGA cards, and the svga_* entries are the accelerated 1990s cards (an ordinary VGA-era game is happy on svga_s3). 'pc98'/'pc9801'/'pc9821' make the machine an NEC PC-98 instead of an IBM PC.",
            "type": "enum", "options": VIDEO_CARDS, "default": "svga_s3", "sync": True
        },
        {
            "name": "memsizeMB", "display": "RAM Size (MB)",
            "description": "Whole megabytes of RAM. Period machines had very little: 0 (with RAM Size (KB) supplying the real figure) for a pre-1987 PC, 1 to 8 for the 286/386 years, 32 to 128 for a late Pentium. More RAM is not always better - some DOS games refuse to start when they find more than they expect.",
            "type": "int", "default": 32, "min": 0, "max": 256, "sync": True
        },
        {
            "name": "memsizeKB", "display": "RAM Size (KB)",
            "description": "Kilobytes of RAM ADDED to RAM Size (MB), for the machines that had less than a megabyte: 256 for a 1981 PC, 640 for an XT or a PS/2 25. Leave at 0 on anything from 1986 on, where RAM Size (MB) says it all.",
            "type": "int", "default": 0, "min": 0, "max": 262144, "sync": True
        },
        {
            "name": "pcSpeaker", "display": "PC Speaker",
            "description": "Whether the machine has its internal beeper wired up. Every machine here had one, and DOS games that predate sound cards play their music on it; disabling it silences that music without changing anything else.",
            "type": "enum", "options": ["disabled", "enabled"],
            "default": "enabled", "sync": True
        },
        {
            "name": "soundBlasterModel", "display": "Sound Blaster Model",
            "description": "Which sound card is fitted. 'none' is a machine with no card at all (pre-1987 PCs had none); 'gb' is Creative's Game Blaster; sb1/sb2 are the 8-bit Sound Blasters, sbpro1/sbpro2 the stereo Pros, sb16/sb16vibra the 16-bit ones. Pick the card the game was written for - a 1990 game will not find an sb16's extras and a 1995 game may refuse an sb1.",
            "type": "enum", "options": SB_MODELS, "default": "sbpro2", "sync": True
        },
        {
            "name": "soundBlasterIRQ", "display": "Sound Blaster IRQ",
            "description": "The interrupt line the Sound Blaster answers on. -1 leaves DOSBox-X to use the model's own factory default (7 for the early cards, 5 for a Sound Blaster 16), which is what a game's own setup program expects to find.",
            "type": "int", "default": -1, "min": -1, "max": 15, "sync": True
        },
        {
            "name": "videoMemoryMB", "display": "Video Memory (MB)",
            "description": "Megabytes on the video card, which is what caps the resolution and colour depth an SVGA card can offer: 1 reaches 1024x768 in 256 colours, 2 reaches 640x480 in true colour, 8 reaches 1600x1200 in true colour. -1 lets DOSBox-X fit the amount the chosen card shipped with. Ignored by the pre-VGA cards, which have a fixed amount.",
            "type": "int", "default": -1, "min": -1, "max": 64, "sync": True
        },
        {
            "name": "vesaModelistWidthLimit", "display": "VESA Mode List Width Limit",
            "description": "Hides VESA modes wider than this many pixels from the list a program sees. Some DOS programs mishandle a long mode list or a mode larger than they can imagine; 1280 is DOSBox-X's own cap. 0 lists every mode the card can do, which is what a Windows 9x display driver wants.",
            "type": "int", "default": 1280, "min": 0, "max": 4096, "sync": True
        },
        {
            "name": "vesaModelistHeightLimit", "display": "VESA Mode List Height Limit",
            "description": "The same cap on height. 1024 is DOSBox-X's own; 0 lists every mode.",
            "type": "int", "default": 1024, "min": 0, "max": 4096, "sync": True
        },
        {
            "name": "dosVersion", "display": "Reported DOS Version",
            "description": "The version DOSBox-X's built-in DOS reports to programs that ask. 'auto' lets it pick (currently 5.0, the safest for DOS gaming). 6.22 is the last real MS-DOS; 7.0 and 7.1 are the DOS underneath Windows 95 and 98, and are what an installer or a long-filename-aware program looks for. Has no effect when the machine boots a DOS of its own from a disk.",
            "type": "enum", "options": DOS_VERSIONS, "default": "auto", "sync": True
        },
        {
            "name": "hardDriveDataRateLimit", "display": "Hard Disk Data Rate (bytes/s)",
            "description": "Slows the emulated hard disk to this many bytes per second, so a game that reads from disk takes as long as it did on the real machine. -1 uses DOSBox-X's own period-plausible limit; 0 removes the limit entirely, which is what a Windows 9x install wants and what plain DOSBox does.",
            "type": "int", "default": -1, "min": -1, "max": 1000000000, "sync": True
        },
        {
            "name": "floppyDriveDataRateLimit", "display": "Floppy Data Rate (bytes/s)",
            "description": "The same limit for the floppy drives. -1 uses DOSBox-X's own; 0 makes floppy reads instant.",
            "type": "int", "default": -1, "min": -1, "max": 1000000000, "sync": True
        },
        {
            "name": "int13FakeIo", "display": "Fake INT 13h Disk I/O",
            "description": "Makes the emulated IDE and floppy controllers react to BIOS disk calls as real hardware would - changing their registers, and raising fake virtual-8086 I/O traps and interrupts. Windows 3.11's and Windows 95's 32-bit disk access need this and will not drive the disks without it; a plain DOS machine does not, and switching it on costs nothing but does nothing. Sets int13fakeio and int13fakev86io on both IDE channels and int13fakev86io on the floppy controller.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "cdromInsertionDelayMs", "display": "CD-ROM Insertion Delay (ms)",
            "description": "How long the drive reports an empty tray after a disc is swapped, in milliseconds - the time it would take somebody to change the disc. 0 leaves the drive's own default (no delay). Windows 95 and later need about 4000 before their auto-insert notification will notice a new disc.",
            "type": "int", "default": 0, "min": 0, "max": 60000, "sync": True
        },
        {
            "name": "bootDrive", "display": "Boot From",
            "description": "Boot the machine from a mounted drive instead of dropping to the DOS prompt: 'a' boots a bootable floppy image, 'c' boots an operating system installed on the hard disk. 'none' keeps the built-in DOS shell. (Chimera addition; BizHawk movies use 'none'.)",
            "type": "enum", "options": ["none", "a", "c"], "default": "none",
            "sync": True
        },
        # ---- devices that are nothing without their ROM (chimera additions;
        # the defaults change nothing, so a BizHawk movie's machine is intact)
        {
            "name": "midiDevice", "display": "MIDI Device",
            "description": "What is plugged into the MPU-401 MIDI port. 'none' leaves the port with nothing on the other end of it, which is how every one of these machines left the factory. 'mt32_old' is a first-generation Roland MT-32 (control ROM v1.07), which the earliest Sierra titles rely on the quirks of; 'mt32_new' is the second generation (v2.04); 'cm32l' is a Roland CM-32L / LAPC-I (v1.02), the MT-32 with the extra sound effects later games use. Each needs its control and PCM ROM, dumped from a unit you own.",
            "type": "enum", "options": ["none", "mt32_old", "mt32_new", "cm32l"],
            "default": "none", "sync": True
        },
        {
            "name": "pc98FontRom", "display": "Use PC-98 Font ROM",
            "description": "Draw PC-98 text with a real NEC character ROM (FONT.ROM) instead of the built-in free font. Only meaningful with a pc98 video card type.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "pc98SoundBios", "display": "Use PC-98 Sound BIOS",
            "description": "Map the PC-9801-26K/86 sound board's BIOS (SOUND.ROM) at CC000h. The FM board plays without it; games that call the sound BIOS do not. Only meaningful with a pc98 video card type.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "pc98RhythmSamples", "display": "Use PC-98 Rhythm Samples",
            "description": "Give the PC-9801-86 board's YM2608 its six built-in drum samples (2608_bd/sd/top/hh/tom/rim.wav). Without them FM music plays with no drums. Only meaningful with a pc98 video card type.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "ibmRomBasic", "display": "Use IBM ROM BASIC",
            "description": "Load IBM Cassette/ROM BASIC below the BIOS at F6000h, as an IBM 5150 has it. PC-DOS's BASICA needs it, and so does booting with no disk.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "vgaBiosRom", "display": "Use Real Video BIOS",
            "description": "Run a real video BIOS dumped from the card chosen in Video Card Type, instead of the one DOSBox-X generates. For software that probes the card's BIOS.",
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
