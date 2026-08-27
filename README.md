# chimera-core-dosbox-x

**DOSBox-X as a Chimera waterbox core** - the [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
DOS/PC emulator, compiled into [miniBox](https://github.com/ToolAssisted-run/chimera-common-minibox)'s
deterministic sandbox and packaged as a Chimera core (`core.wbx` + `waterbox.config`),
the same shape as
[chimera-core-ppsspp](https://github.com/ToolAssisted-run/chimera-core-ppsspp),
[chimera-core-quickernes](https://github.com/ToolAssisted-run/chimera-core-quickernes) and
[chimera-core-neshawk](https://github.com/ToolAssisted-run/chimera-core-neshawk).

Status: **working**. The sandboxed machine boots to the DOS prompt and runs real
bootable floppies, byte-identical to a native reference build on every digest
(video, audio, all memory domains) and stable under whole-machine savestate
round-trips around every frame. What the machine has:

- **Full input**: the 100-key keyboard, mouse (absolute + relative, three
  buttons), two game-port joysticks - all through Chimera's wide-input channel
  (a DOS keyboard does not fit a packed button word).
- **Storage**: floppy, hard disk and CD images load as plain hash-bound
  mounted files (`.img`/`.ima`/`.hdd`/`.iso`/`.cue` and friends); bootable
  floppies boot (`bootDrive` sync setting); the writable hard disk lives in
  guest memory and exports through Chimera's savedata channel
  (`Emulator > Export Save Data...`), and an exported image reloads as a
  machine's disk with every modification intact.
- **Live disk swapping**: extra images mount as a swap list (`rom2..romN`)
  and the disk-swap input controls cycle floppies and CDs at runtime.
- **Machine presets**: ten machine-year configurations (1981 IBM XT 5150 to
  1999 IBM ThinkPad 240) plus RAM size, CPU cycles and blank pre-formatted
  FAT16 hard disks, all as declared sync settings that movies record.
- **Dynamic video**: the frame follows the DOS mode (720x400 text, CGA
  640x400, VGA and beyond) up to a 1024x768 buffer.

The equivalence gate (`waterbox/run-gate.sh`) runs ten legs in about a minute,
each proving native == sandbox == savestate-rerecord and, where input is
involved, that the input visibly shaped the machine (a hollow pass cannot
sneak through). Its content is entirely machine-generated: hand-rolled
ISO9660 discs, FAT12 floppies and 24-byte DOS programs that render the game
port and INT 33h state into video memory. `waterbox/tests/run-frontend.sh`
proves the packaged core inside the Chimera frontend itself, and
`waterbox/tests/run-roms.sh` boots whatever licensed images sit in the
gitignored `tests/roms/` - nothing licensed ships in this repository.

## Building

Both builds are meson. The native reference:

```
meson setup build/meson-native -Dminibox_dir=<miniBox checkout>
ninja -C build/meson-native            # run-native + run-wbx
```

The guest needs a miniBox checkout built with its C++ guest toolchain
(`meson setup build/meson-cpp <miniBox> -Dguest_cpp=true`):

```
./waterbox/setup-guest.sh [-m <miniBox>] # generates the cross file
ninja -C build/meson-guest               # core.wbx
./waterbox/build-package.sh              # -> <chimera>/build/Cores/dosbox-x.chimeraCore
./waterbox/run-gate.sh                   # the equivalence gate
```

The wbx modifications to DOSBox-X live in `patches/` as full-file copies,
overlaid onto the pristine submodule at build time (`git -C extern/dosbox-x
diff -w` shows the real change). `waterbox/gen-config.py` generates
`waterbox.config` and `default_keybinds.json` from the KBD_KEYS enum, so the
button wire format is derived, not hand-maintained.

## Credits & provenance

All emulation comes from **DOSBox-X**, by Jonathan Campbell and contributors
(itself descended from DOSBox by the DOSBox Team), GPL-2.0-or-later, vendored
unmodified as the submodule [`extern/dosbox-x`](extern/dosbox-x), pinned to a
release tag. The integration layer is this repository's own work under the
**MIT License**, with two GPL-2.0-or-later exceptions: [`patches/`](patches)
(modified DOSBox-X source) and `waterbox/dosbox-driver.cpp` (descended from
the author's BizHawk-side `bizhawk.cpp`). Built artifacts combine this code
with DOSBox-X and are therefore distributed under the GPL-2.0-or-later; MIT
permits that combination. See [`LICENSE`](LICENSE).

The prior art this re-implementation draws on, all by this repository's author:

- the BizHawk DOSBox-X waterbox port (TASEmulators/BizHawk `waterbox/dosbox`,
  its C# core `src/BizHawk.Emulation.Cores/Computers/DOS`, and the
  TASEmulators/dosbox-x `wbx` branch) - the coroutine frame slicing, the
  virtual clock, the in-guest hard-disk memory file and the input injection
  all originate there; `docs/PLAN.md` maps each mechanism to its new home,
- [jaffarCommon](https://github.com/SergioMartin86/jaffarCommon)'s
  `MemoryFileDirectory` (the in-guest writable file layer the hard-disk
  image lives in), vendored as the submodule `extern/jaffarCommon`.

`extern/vendored/` carries the sandbox-adapted SDL2 (zlib license, signal
handling removed), libco (ISC) and the SDL_net headers (zlib license).
