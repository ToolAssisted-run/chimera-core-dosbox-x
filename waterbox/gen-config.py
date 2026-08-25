#!/usr/bin/env python3
"""Generates waterbox.config and default_keybinds.json from the KBD_KEYS
order in extern/dosbox-x/include/keyboard.h.

The button order IS the wire format: config index i (i < 100) maps to KBD
value i+1 in the guest's SetButton export, and movies carry one column per
button in this order. Regenerate only with a matching guest change; run from
waterbox/: python3 gen-config.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# (kbd token, display name, default host bind) - KBD_KEYS order, values 1..100
# (KEY_COUNT is 0x65, so the array covers KBD_NONE + these hundred keys).
KEYS = []
def K(token, name, bind):
    KEYS.append((token, name, bind))

for d in '1234567890':
    K(f'KBD_{d}', f'Key {d}', f'D{d}')
for c in 'qwertyuiop' + 'asdfghjkl' + 'zxcvbnm':
    K(f'KBD_{c}', f'Key {c.upper()}', c.upper())
for i in range(1, 13):
    K(f'KBD_f{i}', f'Key F{i}', f'F{i}')
K('KBD_esc', 'Key Escape', 'Escape')
K('KBD_tab', 'Key Tab', 'Tab')
K('KBD_backspace', 'Key Backspace', 'Back')
K('KBD_enter', 'Key Enter', 'Enter')
K('KBD_space', 'Key Space', 'Space')
K('KBD_leftalt', 'Key Left Alt', 'LeftAlt')
K('KBD_rightalt', 'Key Right Alt', 'RightAlt')
K('KBD_leftctrl', 'Key Left Ctrl', 'LeftCtrl')
K('KBD_rightctrl', 'Key Right Ctrl', 'RightCtrl')
K('KBD_leftshift', 'Key Left Shift', 'LeftShift')
K('KBD_rightshift', 'Key Right Shift', 'RightShift')
K('KBD_capslock', 'Key Caps Lock', 'Capital')
K('KBD_scrolllock', 'Key Scroll Lock', 'Scroll')
K('KBD_numlock', 'Key Num Lock', 'NumLock')
K('KBD_grave', 'Key Grave', 'OemTilde')
K('KBD_minus', 'Key Minus', 'OemMinus')
K('KBD_equals', 'Key Equals', 'OemPlus')
K('KBD_backslash', 'Key Backslash', 'OemPipe')
K('KBD_leftbracket', 'Key Left Bracket', 'OemOpenBrackets')
K('KBD_rightbracket', 'Key Right Bracket', 'OemCloseBrackets')
K('KBD_semicolon', 'Key Semicolon', 'OemSemicolon')
K('KBD_quote', 'Key Quote', 'OemQuotes')
K('KBD_period', 'Key Period', 'OemPeriod')
K('KBD_comma', 'Key Comma', 'OemComma')
K('KBD_slash', 'Key Slash', 'OemQuestion')
K('KBD_extra_lt_gt', 'Key LtGt', 'OemBackslash')
K('KBD_printscreen', 'Key Print Screen', 'PrintScreen')
K('KBD_pause', 'Key Pause', 'Pause')
K('KBD_insert', 'Key Insert', 'Insert')
K('KBD_home', 'Key Home', 'Home')
K('KBD_pageup', 'Key Page Up', 'PageUp')
K('KBD_delete', 'Key Delete', 'Delete')
K('KBD_end', 'Key End', 'End')
K('KBD_pagedown', 'Key Page Down', 'PageDown')
K('KBD_left', 'Key Left', 'Left')
K('KBD_up', 'Key Up', 'Up')
K('KBD_down', 'Key Down', 'Down')
K('KBD_right', 'Key Right', 'Right')
for d in '1234567890':
    K(f'KBD_kp{d}', f'Key Numpad {d}', f'NumPad{d}')
K('KBD_kpdivide', 'Key Numpad Divide', 'Divide')
K('KBD_kpmultiply', 'Key Numpad Multiply', 'Multiply')
K('KBD_kpminus', 'Key Numpad Minus', 'Subtract')
K('KBD_kpplus', 'Key Numpad Plus', 'Add')
assert len(KEYS) == 100, len(KEYS)

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

JOY = [(f'P{p} Joystick {b}', bind)
       for p, pad in ((1, 'J1'), (2, 'J2'))
       for b, bind in (('Up', f'{pad} POV1U'), ('Down', f'{pad} POV1D'),
                       ('Left', f'{pad} POV1L'), ('Right', f'{pad} POV1R'),
                       ('Button 1', f'{pad} B1'), ('Button 2', f'{pad} B2'))]
MOUSE_BTNS = [('Mouse Left Button', 'WMouse L'), ('Mouse Middle Button', 'WMouse M'),
              ('Mouse Right Button', 'WMouse R')]
SWAP = [('Previous Floppy Disk', ''), ('Next Floppy Disk', ''), ('Swap Floppy Disk', ''),
        ('Previous CD', ''), ('Next CD', ''), ('Swap CD', '')]

buttons = [name for _, name, _ in KEYS] + [n for n, _ in JOY] \
    + [n for n, _ in MOUSE_BTNS] + [n for n, _ in SWAP]

axes = [
    {"name": "Mouse Position X", "min": 0, "max": 800, "neutral": 400},
    {"name": "Mouse Position Y", "min": 0, "max": 600, "neutral": 300},
    {"name": "Mouse Speed X", "min": -180, "max": 180, "neutral": 0},
    {"name": "Mouse Speed Y", "min": -180, "max": 180, "neutral": 0},
]

PRESETS = [
    "1981_ibm_xt5150", "1983_ibm_xt5160", "1986_ibm_xt5162", "1987_ibm_ps2_25",
    "1990_ibm_ps2_25_286", "1991_ibm_ps2_25_386", "1993_ibm_ps2_53_slc2_486",
    "1994_ibm_ps2_76i_slc2_486", "1997_ibm_aptiva_2140", "1999_ibm_thinkpad_240",
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
        "_comment": "the BUFFER capacity; the live size comes from GetVideoWidth/Height per frame (text mode is 720x400, VGA up to 720x480, SVGA modes above 1024x768 are clamped)",
        "width": 1024, "height": 768,
        "virtualWidth": 1024, "virtualHeight": 768,
        "vsyncNumerator": 3146888, "vsyncDenominator": 44900,
        "getBgra": "GetVideoBgra"
    },
    "audio": {"samplesPerFrame": 4096, "channels": 2, "get": "GetAudio"},
    "input": {
        "name": "DOS Keyboard and Devices",
        "_comment": "index order is the wire format: 0..99 are KBD_KEYS 1..100 (see gen-config.py), then joysticks, mouse buttons, disk-swap controls. Wider than 64, so everything rides the SetButton channel.",
        "buttons": buttons,
        "axes": axes,
        "_axes_note": "Mouse position is absolute in an 800x600 plane; speed is the per-frame relative movement. The frontend feeds these through SetAxis before every frame."
    },
    "extensions": {
        ".ima": "DOS", ".img": "DOS", ".xdf": "DOS", ".fdi": "DOS",
        ".hdd": "DOS", ".conf": "DOS"
    },
    "settings": [
        {
            "name": "machinePreset", "display": "Machine",
            "description": "Which PC this is: a machine-year preset bundling CPU, memory, video adapter and sound hardware of a representative system. Shapes the whole machine; movies record it.",
            "type": "enum", "options": PRESETS, "default": "1991_ibm_ps2_25_386",
            "sync": True
        },
        {
            "name": "formattedHardDisk", "display": "Formatted Hard Disk",
            "description": "Mount a blank pre-formatted FAT16 hard disk as C: when the loaded file is not itself a hard disk image. Its contents are this core's save data (Emulator > Export Save Data).",
            "type": "enum",
            "options": ["none", "21mb", "41mb", "241mb", "504mb", "2014mb"],
            "default": "none", "sync": True
        },
        {
            "name": "memsizeMB", "display": "RAM Size (MB)",
            "description": "Emulated memory size in megabytes; -1 keeps the machine preset's value. Shapes the machine.",
            "type": "int", "default": -1, "sync": True
        },
        {
            "name": "cpuCycles", "display": "CPU Cycles",
            "description": "Emulated CPU cycles per millisecond; -1 keeps the machine preset's value. Shapes timing everywhere, so movies record it.",
            "type": "int", "default": -1, "sync": True
        },
        {
            "name": "bootDrive", "display": "Boot From",
            "description": "Boot the machine from a mounted drive instead of dropping to the DOS prompt: 'a' boots a bootable floppy image, 'c' boots an operating system installed on the hard disk. 'none' keeps the built-in DOS shell.",
            "type": "enum", "options": ["none", "a", "c"], "default": "none",
            "sync": True
        },
        {
            "name": "joysticksEnabled", "display": "Joysticks",
            "description": "Whether the two-axis game port joysticks are plugged in.",
            "type": "bool", "default": False, "sync": True
        },
        {
            "name": "mouseSensitivity", "display": "Mouse Sensitivity",
            "description": "Scales relative mouse movement before it reaches the DOS driver.",
            "type": "int", "default": 100, "sync": True
        }
    ]
}

with open(os.path.join(HERE, 'waterbox.config'), 'w') as f:
    json.dump(config, f, indent=2)
    f.write('\n')

binds = {name: bind for _, name, bind in KEYS}
binds.update({n: b for n, b in JOY})
binds.update({n: b for n, b in MOUSE_BTNS})
binds.update({n: b for n, b in SWAP})
analog = {
    "Mouse Position X": {"Value": "WMouse X", "Mult": 1.0, "Deadzone": 0.0,
                          "ButtonBindPositive": "", "ButtonBindNegative": ""},
    "Mouse Position Y": {"Value": "WMouse Y", "Mult": 1.0, "Deadzone": 0.0,
                          "ButtonBindPositive": "", "ButtonBindNegative": ""},
    "Mouse Speed X": {"Value": "RMouse X", "Mult": 1.0, "Deadzone": 0.0,
                       "ButtonBindPositive": "", "ButtonBindNegative": ""},
    "Mouse Speed Y": {"Value": "RMouse Y", "Mult": 1.0, "Deadzone": 0.0,
                       "ButtonBindPositive": "", "ButtonBindNegative": ""},
}
keybinds = {
    "_comment": [
        "Default bindings for the DOS machine this package declares. The keyboard maps",
        "1:1 onto the host keyboard, the game-port joysticks onto host gamepads, and",
        "the mouse onto the host mouse. Disk-swap controls ship unbound."
    ],
    "AllTrollers": {"DOS Keyboard and Devices": binds},
    "AllTrollersAutoFire": {"DOS Keyboard and Devices": {}},
    "AllTrollersAnalog": {"DOS Keyboard and Devices": analog},
}
with open(os.path.join(HERE, 'default_keybinds.json'), 'w') as f:
    json.dump(keybinds, f, indent=2)
    f.write('\n')

print(f'waterbox.config: {len(buttons)} buttons, {len(axes)} axes')
print('default_keybinds.json written')
