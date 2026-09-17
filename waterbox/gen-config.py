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

# BizHawk's axis planes: absolute position on a MouseAbsoluteScreenWidth/
# Height plane (defaults SVGA_MAX 2560x2048), relative speed -180..180.
axes = [
    {"name": "Mouse Position X", "min": 0, "max": 2560, "neutral": 1280},
    {"name": "Mouse Position Y", "min": 0, "max": 2048, "neutral": 1024},
    {"name": "Mouse Speed X", "min": -180, "max": 180, "neutral": 0},
    {"name": "Mouse Speed Y", "min": -180, "max": 180, "neutral": 0},
]

PRESETS = [
    "1981_ibm_xt5150", "1983_ibm_xt5160", "1986_ibm_xt5162", "1987_ibm_ps2_25",
    "1990_ibm_ps2_25_286", "1991_ibm_ps2_25_386", "1993_ibm_ps2_53_slc2_486",
    "1994_ibm_ps2_76i_slc2_486", "1997_ibm_aptiva_2140", "1999_ibm_thinkpad_240",
]

CPU_TYPES = [
    "auto", "8086", "8086_prefetch", "80186", "80186_prefetch",
    "286", "286_prefetch", "386", "386_prefetch",
    "486old", "486old_prefetch", "486", "486_prefetch",
    "pentium", "pentium_mmx", "ppro_slow", "pentium_ii", "pentium_iii",
]

VIDEO_CARDS = [
    "auto", "mda", "cga", "cga_mono", "cga_rgb", "cga_composite", "cga_composite2",
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

SB_MODELS = ["auto", "none", "sb1", "sb2", "sbpro1", "sbpro2", "sb16",
             "sb16vibra", "gb", "ess688", "reveal_sc400"]

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
        "_axes_note": "Mouse position is absolute on a 2560x2048 plane (BizHawk's default MouseAbsoluteScreenWidth/Height); speed is the per-frame relative movement. The frontend feeds these through SetAxis before every frame."
    },
    "extensions": {
        ".ima": "DOS", ".img": "DOS", ".xdf": "DOS", ".fdi": "DOS",
        ".hdd": "DOS", ".conf": "DOS"
    },
    "settings": [
        {
            "name": "machinePreset", "display": "Configuration Preset",
            "description": "Establishes a base configuration for DOSBox roughly corresponding to the selected computer model. We recommend choosing a model that is roughly of the same year or above of the game / tool you plan to run. More modern models may require more CPU power to emulate.",
            "type": "enum", "options": PRESETS, "default": "1993_ibm_ps2_53_slc2_486",
            "sync": True
        },
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
            "description": "For relative mouse inputs, this adjusts the mouse relative speed (mickey) multiplier.",
            "type": "float", "default": 3.0, "sync": True
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
            "description": "How many CPU cycles to emulate per ms. Default: -1, to keep the one included in the configuration preset.",
            "type": "int", "default": -1, "sync": True
        },
        {
            "name": "cpuType", "display": "CPU Type",
            "description": "Chooses the CPU type to emulate. Auto uses the configuration preset's default.",
            "type": "enum", "options": CPU_TYPES, "default": "auto", "sync": True
        },
        {
            "name": "videoCardType", "display": "Video Card Type",
            "description": "Chooses the video card to emulate. Auto uses the configuration preset's default.",
            "type": "enum", "options": VIDEO_CARDS, "default": "auto", "sync": True
        },
        {
            "name": "memsizeMB", "display": "RAM Size (MB)",
            "description": "The size of the memory capacity (RAM) to emulate. -1 to keep the value for the machine preset. Maximum value: 256",
            "type": "int", "default": -1, "min": -1, "max": 256, "sync": True
        },
        {
            "name": "pcSpeaker", "display": "PC Speaker",
            "description": "Chooses whether to enable/disable the PC Speaker. Auto uses the configuration preset's default.",
            "type": "enum", "options": ["auto", "disabled", "enabled"],
            "default": "auto", "sync": True
        },
        {
            "name": "soundBlasterModel", "display": "Sound Blaster Model",
            "description": "Chooses the Sound Blaster model to emulate. Auto uses the configuration preset's default.",
            "type": "enum", "options": SB_MODELS, "default": "auto", "sync": True
        },
        {
            "name": "soundBlasterIRQ", "display": "Sound Blaster IRQ",
            "description": "Chooses the interrupt request number for the Sound Blaster. -1 for automatic.",
            "type": "int", "default": -1, "sync": True
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
            "description": "What is plugged into the MPU-401 MIDI port. 'auto' keeps the machine preset's own (no synthesizer). 'mt32_old' is a first-generation Roland MT-32 (control ROM v1.07), which the earliest Sierra titles rely on the quirks of; 'mt32_new' is the second generation (v2.04); 'cm32l' is a Roland CM-32L / LAPC-I (v1.02), the MT-32 with the extra sound effects later games use. Each needs its control and PCM ROM, dumped from a unit you own.",
            "type": "enum", "options": ["auto", "mt32_old", "mt32_new", "cm32l"],
            "default": "auto", "sync": True
        },
        {
            "name": "pc98FontRom", "display": "PC-98 Font ROM",
            "description": "Draw PC-98 text with a real NEC character ROM (FONT.ROM) instead of the built-in free font. Only meaningful with a pc98 video card type.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "pc98SoundBios", "display": "PC-98 Sound BIOS",
            "description": "Map the PC-9801-26K/86 sound board's BIOS (SOUND.ROM) at CC000h. The FM board plays without it; games that call the sound BIOS do not. Only meaningful with a pc98 video card type.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "pc98RhythmSamples", "display": "PC-98 Rhythm Samples",
            "description": "Give the PC-9801-86 board's YM2608 its six built-in drum samples (2608_bd/sd/top/hh/tom/rim.wav). Without them FM music plays with no drums. Only meaningful with a pc98 video card type.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "ibmRomBasic", "display": "IBM ROM BASIC",
            "description": "Load IBM Cassette/ROM BASIC below the BIOS at F6000h, as an IBM 5150 has it. PC-DOS's BASICA needs it, and so does booting with no disk.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "vgaBiosRom", "display": "Video BIOS ROM",
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
