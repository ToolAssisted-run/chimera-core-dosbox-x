# DOSBox-X as a Chimera core - analysis and plan

The BizHawk DOSBox-X integration is the author's own work; this port carries
its mechanisms over to Chimera, keeps what is proven, and replaces exactly the
parts Chimera's pipeline makes unnecessary. This document is the analysis of
that integration (as of TASEmulators/dosbox-x branch `wbx` @ 892f07b19, which
already merged upstream release 9215f53 = dosbox-x-v2026.06.02, plus
TASEmulators/BizHawk `waterbox/dosbox/bizhawk.cpp` and
`src/BizHawk.Emulation.Cores/Computers/DOS/*`), and the plan for the
re-implementation.

## Upstream base

- Rebase target: **dosbox-x-v2026.08.02** (784240ad), the latest release,
  vendored unmodified as the submodule `extern/dosbox-x`.
- The wbx modifications are the diff `9215f53..origin/wbx` of
  TASEmulators/dosbox-x (local reference checkout:
  `~/BizHawk/waterbox/dosbox/dosbox-x`). They are carried here as `patches/`:
  full-file copies compiled INSTEAD of the pinned files, the
  chimera-core-ppsspp recipe (the build's source list excludes the pinned
  original and compiles the patched copy; nothing in the submodule is
  touched). Two releases of drift (06.02 -> 08.02) to absorb while porting
  each file.
- Beware: much of the wbx diff's bulk is line-ending/whitespace churn
  (bios_disk.cpp, savestates.cpp) and the opus decoder removal. The
  functional core is small and enumerated below.

## The BizHawk mechanisms, one by one

### 1. Frame slicing: coroutines + a virtual clock (KEEP)

DOSBox has no frame loop to call; it has `Normal_Loop`, which runs
emulation and pumps events until its tick budget runs out. The port never
restructures that. Instead:

- `include/timer.h`: `GetTicks()` -> `_GetTicks()`, the driver's virtual
  millisecond counter (`_ticksElapsed`). No SDL timer exists.
- `src/dosbox.cpp`: `wrap_delay(a)` -> `_Delay(a)`, which advances
  `_ticksElapsed` by `a` and `co_switch`es back to the driver; and
  `Normal_Loop`'s "out of ticksRemain" branch does
  `increaseticks(); co_switch(_driverCoroutine)`.
- The driver (`bizhawk.cpp`): `_main()` runs on a libco coroutine (4 MiB
  stack). `FrameAdvance` computes `ticksPerFrame = 1000/fps` from the
  frontend-chosen framerate, raises a float `ticksTarget`, and switches into
  the emu coroutine until `_ticksElapsed` reaches the integer target.
  `Cycles` per frame = ticks consumed (drives ICycleTiming; vsync is a sync
  setting, the machine has no fixed rate of its own).

Chimera: keep this exactly. libco's `amd64.c` is pure userland context
switching and is already proven inside the waterbox; vendor it in this repo
(`waterbox/libco/`). miniBox green threads are the fallback if libco
misbehaves under seal/savestate (the coroutine stacks live in guest memory
either way, so whole-machine savestates capture suspended coroutines - this
WORKED in BizHawk, keep the stack allocation inside the sealed arena).

### 2. Video (KEEP)

SDL2 is vendored and compiled with dummy video/audio drivers
(`SDL_VIDEODRIVER=dummy`); DOSBox renders into `sdl.surface` in guest
memory. The wbx branch adds a render-update callback (invoked by the render
path when the surface changes) so the driver copies pixels on demand,
avoiding tearing, plus a refresh-rate callback reporting DOS video mode
changes (`_refreshRateNumerator/Denominator`). Frame size is dynamic (DOS
modes change resolution); the frontend reads Width/Height per frame -
Chimera's config declares the maximum and `GetVideoWidth/Height` exports
report the live size.

### 3. Audio (KEEP)

`src/hardware/mixer.cpp` tees the mixer's converted stereo s16 output into
`std::vector<int16_t> _audioSamples`, cleared by the driver each
FrameAdvance; sample count varies per frame. Maps directly onto
`GetAudio`/`GetAudioSampleCount`.

### 4. Keyboard (KEEP mechanism, NEW transport)

`src/hardware/keyboard.cpp` exposes `_pressedKeys`/`_releasedKeys`
(std::set<KBD_KEYS>); the driver diffs a `char Keys[0x65]` array against the
previous frame and fills the sets; DOSBox's own `KEYBOARD_AddKey` pending
mechanism is bypassed in favour of the per-frame sets.

Chimera problem: the packed input word is uint64 - 101 keyboard keys plus
joystick/mouse buttons do not fit. The entry format itself
(chimera::EntryLayout) already handles arbitrary button counts (it did in
C# bk2 too); only the advance ABI is narrow. Plan: an ADDITIVE engine
extension mirroring SetAxis - the guest exports `SetButton(index, state)`
(or a key-block buffer), the session drives buttons beyond bit 63 through
it, `ce_session_movie_advance` gains a wide-mask variant. This is chimera
work, tracked there; until it lands, bring-up uses a reduced binding set
(joystick + a develpment subset of keys fits in 64) so the machine work is
not blocked on the ABI.

### 5. Joystick + mouse (KEEP)

- `src/hardware/joystick.cpp`: `stick[0..1]` made externally writable; the
  driver sets xpos/ypos (from digital up/down/left/right in BizHawk) and two
  buttons per stick, gated by sync settings `joystick1Enabled/2Enabled`
  (which also choose `joysticktype = 2axis`).
- `src/ints/mouse.cpp`: cursor permanently locked (`user_cursor_locked`),
  driver injects absolute position (scaled to the DOS range via
  mouse.min/max), relative mickeys from speedX/Y * sensitivity, PS/2
  accumulators, and button press/release events.

Chimera: joystick axes ride the existing axis channel (analog, an
improvement over BizHawk's digital-only sticks); mouse position/speed are
axes, its buttons are buttons.

### 6. The writable hard disk: in-guest memory file (KEEP)

The proven recipe (also the model for chimera's whole savedata design, see
chimera docs/save-data.md):

- The HDD image enters as a hash-bound READ-ONLY mounted file
  ("HardDiskDrive").
- At Init - before seal - `loadFileIntoMemoryFile` copies it, sector by
  sector, into a jaffarCommon `MemoryFile` ("HardDiskDrive.img") in the
  guest heap sized by the frontend (`MmapHeapSizeKB += image size`). The
  copy lands in the sealed baseline, so savestates carry only dirtied
  pages; rewind correctness is free.
- `include/memfile.h` + `imageDisk_Mem` (bios_disk) back `imgmount c` with
  that memfile; `dos_programs.cpp` recognises memfile paths. Writes grow
  the file if needed.
- Pre-formatted FAT16 images (21/41/241/504/2014 MB, zst-compressed
  resources, machine-generated so freely distributable) are offered when
  the user provides no image; `imgmount c HardDiskDrive.img` goes into the
  composed autoexec.

Chimera changes: the export side is the savedata guest ABI group
(`GetSaveDataFileCount/Name/Size/Buffer`, one entry, "HardDiskDrive.img")
instead of BizHawk's GetHDD* + ISaveRam-unregister + bespoke menu item; the
2 GB case is why `ce_session_savedata_read` is ranged. The import side
(bringing an exported image back) is a mounted input like any other. The
HDD also stays a memory domain, as in BizHawk.

### 7. CD-ROM: REPLACED - direct file mounts

BizHawk's frontend owned disc parsing (DiscSystem), so the wbx branch added
a `BizhawkFile : TrackFile` reading sectors through a host callback
(`SetCdCallbacks`), with track layout pushed via `PushCDData`/
`PushTrackData` (".cdrom" pseudo-files, `MAX_CD_COUNT` 10). Chimera
deliberately has no disc layer (a disc image is a rom like any other), so
ALL of that goes away: cue/bin/iso/chd files are mounted read-only and
DOSBox-X's own `BinaryFile`/`CueFile`/`CHDFile` TrackFiles read them
through stdio, which miniBox's syscall surface serves.

Sharp edge to verify at bring-up: miniBox allows ONE open per mounted file.
A cue sheet whose tracks share one bin opens it once per track, and
iso+mscdex paths may reopen. If that trips, a small guest-side shim caching
open FILE*s per mount name (dup'ing the read position) goes into the
driver, not into miniBox.

### 8. Floppies and drive swapping (KEEP)

Floppy images are RO mounts (`FloppyDisk0.img`, ...), `imgmount a` lists
them all, and per-frame `DriveActions.insertFloppyDisk/insertCDROM` call
`swapInDrive` (drive A = 0, D = 3) - the multi-disk mechanism without any
frontend disc knowledge. `_driveUsed` per frame backs the drive light
(IDriveLight survives in Chimera).

### 9. Configuration (KEEP composition, move declaration)

BizHawk composed one config string: base conf + a machine-year preset
(1981 IBM 5150 ... 1999 IBM Thinkpad 240) + joystick/speaker/sblaster/
memsize/cpu (cycles, type)/video-card sections + autoexec (@echo off,
imgmount lines) + user-provided .conf files, passed to the guest as a
mounted file. Chimera: identical composition, but the KNOBS are declared
in `waterbox.config` settings (machine preset, formatted-HDD choice, CPU
cycles/type, RAM size, video card, sound blaster model/IRQ, PC speaker,
joysticks, mouse sensitivity) so the settings dialog is generated and
movies carry them as sync settings. The base/preset .conf resources are
copied from the BizHawk tree into this repo's `waterbox/conf/` (author's
own work). A user .conf can still ride along as an input file.

### 10. What stays disabled (KEEP disabling)

- DOSBox-X's own savestate system (`savestates.cpp` PODs neutered): the
  machine state is the whole guest, snapshotted by miniBox.
- Capture subsystem, networking, MIDI passthrough, printer/parport
  passthrough, IPX, host filesystem drives (drive_local as host access;
  physfs), dynamic CPU cores (dynamic_x86 is JIT - like PPSSPP's JIT it
  cannot be cross-build deterministic; normal/full/simple cores only, the
  dynamic core is a later, gated, guest-only-determinism option).
- The opus decoder was dropped in wbx; keep dropped unless CD audio needs
  it (CDDA from cue/bin is raw PCM; opus was for other formats).

### 11. Determinism inventory (verify at bring-up)

- RTC/date: `src/misc/cross.cpp` was patched in wbx; the box clock is
  constant, and DOS's date/time must derive from emulated ticks, not
  host time. An rtc-base sync setting like PPSSPP's is the shape.
- `remove_duplicate_case`: menus/GUI code paths that read host state are
  compiled out (sdl_gui, mapper).
- Uninitialized-RAM policy, `rand()` seeding, and FPU determinism: the
  BizHawk port is the witness that these were already tamed; the gate
  re-proves it here (native == sandbox on every digest).

## The build (chimera-core-ppsspp shape)

- `waterbox/sources.mk`: ONE curated source list + defines shared by both
  builds - the BizHawk `waterbox/dosbox/Makefile` list is the starting
  point (dos/, cpu/ (no dynamic), hardware/, ints/, gui/ subset, misc/,
  shell/, builtin/, libs subset, fpu, ~129 vendored SDL2 sources with dummy
  drivers, libco/amd64.c, generated config.h with
  `USING_GENERATED_CONFIG_H`, `__LINUX__`, SDL 2.32 version defines).
- `waterbox/native.mk`: glibc reference build -> `bin/run-native`.
- `waterbox/guest.mk` + `build-core.sh`: musl/GCC guest toolchain from
  miniBox (the [[minibox-cpp-guest-toolchain]] recipe) -> `core.wbx`.
- SDL2: vendored tree (from the wbx fork's `third_party/SDL2`, which the
  Makefile's 129-file list compiles; verify whether it differs from stock
  SDL 2.32.0 - if unmodified, submodule libsdl-org/SDL instead of copying).
- jaffarCommon: submodule (as in the wbx fork), for MemoryFileDirectory.
- `waterbox/dosbox-driver.cpp`: bizhawk.cpp reborn - init (config compose,
  HDD memfile seed, coroutine start), FrameAdvance (keys/joy/mouse/drive
  actions in; ticks slice; video/audio out), memory domains (conventional/
  UMA/extended/physical RAM, video RAM, HDD), savedata group, tooling
  groups where cheap (registers: CPU regs; buses: none initially).
- `waterbox/run-wbx.c` + `run-gate.sh`: the standalone sandbox driver and
  the native==sandbox==rerecord digest gate; `tests/run-frontend.sh` for
  the Chimera-side legs (package boot, settings reach, keybinds, savedata
  engine export), all per the ppsspp templates.

## Test content

DOS is the friendliest system in the house for free test content: a `.com`
program is raw 8086 bytes - the gates can carry tiny hand-assembled
programs (write video memory, beep the speaker, read the keyboard, write a
file to C:) with no toolchain and no copyright. FreeDOS provides free
bootable floppies if a real OS boot is wanted. Savedata gates get real
coverage from a .com that writes C:\SAVE.DAT, no sceUtility-style dialog
timing to pin.

## Chimera-side work items (tracked in ~/chimera, not here)

1. **Wide input**: >64 buttons (SetButton guest export + wide-mask movie
   advance + adapter/frontend plumbing). Blocks full keyboard.
2. **Multi-file game descriptor**: floppies + HDD + confs + CDs as one
   citable input set (the user owns this design; single-file inputs and
   settings-chosen formatted HDDs suffice for bring-up).
3. Per-frame drive-action channel (insert floppy N / insert CD N) as a
   movie-recorded input: MAYBE reuse buttons ("Next Disk"), decide with
   the user.

## Milestones

- [ ] M1: repo skeleton, extern/dosbox-x @ v2026.08.02, wbx patch set
      rebased file by file into patches/, native reference build compiles
      and boots to the DOS prompt under a composed config.
- [ ] M2: run-native frame loop - video/audio/input digests, deterministic
      across runs; keyboard/joystick/mouse reach the machine.
- [ ] M3: guest build (musl/GCC toolchain), seal + savestate + rerecord
      correctness; run-gate.sh native==sandbox==rerecord green.
- [ ] M4: HDD memfile + savedata export group + gate leg (a .com writes
      C:\SAVE.DAT; export trees must match everywhere).
- [ ] M5: Chimera package (waterbox.config, settings, default_keybinds,
      build-package.sh), frontend gate legs green; reduced keyboard until
      wide input lands.
- [ ] M6: CD direct-mount proof (free cue/bin), floppy swapping, drive
      light; then real content with the user's movies.
