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
        }
    ]
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
