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
  accumulators, and button press/release events. A speed of zero on an
  axis means "move by how far the position moved since the last frame",
  the conversion BizHawk's frontend makes before its driver sees the frame
  (the last position is guest memory, so savestates carry it); without it
  Mouse Position X/Y alone never moved the mouse (issue #61).

#### The position plane is a fraction of the screen (2026-09-22)

`Mouse Position X/Y` is `0..65535` with neutral `32768`, the one convention
every Chimera core uses for an absolute point (chimera
docs/porting-a-core.md). It is not a pixel count and cannot be: a DOS box
changes video mode whenever it likes, so no number declared in
waterbox.config could be the screen. What CAN be is the range INT 33h keeps
for the mode it is in - `Mouse_AfterNewVideoMode` sets `mouse.min_x/max_x`
per mode, and functions 07h/08h let the guest move them - and the fraction
is resolved against that, every frame.

What this replaced was wrong twice over. The config declared a 2560x2048
plane (BizHawk's `MouseAbsoluteScreenWidth/Height`) and the driver divided
by a constant 800x600, so an axis at its maximum came out as 3.2x the screen
width; and the cursor was only written when the position CHANGED, so a held
position was overwritten by the next mode set and never restored. Measured
on the same build: every value across the whole declared range answered
320,96 - the middle of the screen. The axis did not reach the machine at all.

So the position is now asserted EVERY frame, not only when it changes: a
fraction is not a fixed pixel, and the pixel it means moves when the mode
does. An axis driven by an explicit `Mouse Speed` is exempt and moves
relatively, so a movie that steers with speed alone is not dragged back to
the neutral - which is now the middle of the screen.

`Mouse Speed X/Y` stays relative and stays in PIXELS, which forces the
position path to difference its pixels rather than its wire: one wire unit
is about a hundredth of a pixel at 640 wide, so differencing the wire would
hand the mickey path numbers a hundredfold too large while the absolute
cursor still landed correctly - a fault nothing but a unit comparison can
see. `input:mouse-units` in the gate is that comparison.

Proof (`waterbox/tests/gen-testcom.py`, read back through `--ram-slice`):
`POSTEST.COM` polls INT 33h fn 03h and stores the position in the IACA;
`MODETEST.COM` does the same after switching to 40-column text, which halves
the mouse range; `MICKTEST.COM` accumulates fn 0Bh's motion counters.
`run-native` gained `--mouse-pos`, `--mouse-nudge` and `--mouse-speed` to
drive them. Every expectation is `(axis * screen) / 65536` masked by the
mode's granularity, computed outside the core: 0 reads 0,0; 32768 reads
320,96; 65535 reads 632,192 at 640 wide, and the same values land at 160,96
and 304,192 at 320 wide. Ten pixels of movement is 30 mickeys whether asked
for by position or by speed, in both directions, and a held position
produces none. Negative controls, all reverted: differencing the wire made a
held position invent 27,200 mickeys (caught by `input:mouse-units`, and
correctly NOT by `input:mouse-absolute`, since the cursor still landed
right); resolving against a constant 800 put the midpoint at 400 instead of
320 and broke both legs.

Chimera: joystick axes ride the existing axis channel (analog, an
improvement over BizHawk's digital-only sticks); mouse position/speed are
axes, its buttons are buttons.

#### The VMware absolute pointer (2026-09-22, issue #135)

Everything above reaches the guest as a PS/2 mouse, which is a RELATIVE
device: it reports how far the mouse moved, never where it is. Under plain
DOS that is enough, because the cursor the programs read is DOSBox's own
INT 33h cursor and the driver writes its position directly. Under a guest
OS that keeps its own cursor - Windows 3.1 through 9x - it is not: the OS
integrates the deltas through its own acceleration curve, so nothing
outside the guest knows where the pointer actually is, and a movie that
wants the pointer at a particular place has to steer it there by feel.
That is what issue #135 asks for, and what its reporter's Lua helper does
by hand: it is given the current position, because nothing can tell it.

DOSBox-X already answers this. `src/ints/mouse.cpp` implements VMware's
absolute-pointer backdoor - I/O port 5658h, magic 564D5868h in EAX, the
command in CX - which a guest driver uses to ask for absolute mode and then
read the cursor position as a pair of 0..0FFFFh coordinates instead of
integrating PS/2 packets. Drivers for it exist already; upstream's own
comment names vmwmouse for Windows 3.1. It is on in the base conf
(`vmware = true`) and `MOUSE_Init` registers the port handler.

It was never FED, though. `VMWARE_MousePosition`, the two button calls and
`VMWARE_ScreenParams` are only called from `src/gui/sdlmain.cpp` and
`src/gui/render.cpp` - the SDL frontend, which does not run here. A guest
that asked for absolute mode got 8000h,8000h, the middle of the screen, on
every read for ever. The driver now makes those calls:

- the absolute cursor carries the SAME displacement as the PS/2 stream
  (`mouseSpeedX/Y`, so an explicit Mouse Speed still moves a guest that is
  in absolute mode), clamped at the edges of the plane the way a screen
  clamps. It and `lastMousePos` both start at zero, so a movie that drives
  Mouse Position X/Y and leaves the speeds alone puts the guest cursor
  exactly where it asked;
- the plane is the one the FRONTEND declares for those axes, 2560x2048
  (waterbox.config), not the 800x600 range the BizHawk code scales DOSBox's
  own cursor against. A guest driver rescales 0..0FFFFh to its own screen,
  so the choice only sets the granularity, and the axis neutral should be
  the middle of the screen;
- `VMWARE_ScreenParams` is restated EVERY FRAME, not once at boot.
  render.cpp says it too, from the SDL window's geometry, on every video
  mode change. Said once at boot it was silently replaced the first time
  the mode changed, and every position after that was scaled against the
  wrong width - which is how the first measurement came back wrong (1534
  instead of 431) and what the second one fixed;
- the PS/2 event is still sent. It is the interrupt that tells the guest
  driver to go and poll the port, so the two interfaces are not
  alternatives: the relative one schedules, the absolute one answers.

What this does NOT do is give the guest a driver. The user installs one in
their own disk image, the way a BIOS is supplied, and games that read raw
relative deltas (mouselook, DirectInput exclusive mode) are unaffected
either way - absolute targeting means nothing to them.

Proof: `VMWTEST.COM` (tests/gen-testcom.py) asks for absolute mode, polls
the port and stores x, y and the buttons in the IACA at 0000:04F0 - the 16
bytes IBM reserved for programs to talk to each other, which DOS never
touches - so a run can be asked where the GUEST thinks the cursor is rather
than only whether the screen changed. Run with `--exercise-position`, whose
last frame feeds position 431,43, the guest reads back 2B1Eh,0561h, which
is 431 and 43 scaled to the protocol's range and computed independently of
the core. The same build with the driver's calls removed reads 8000h,8000h.
Native and sandbox agree, and the rerecord leg proves the accumulator is
ordinary guest memory that savestates carry.

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

**PC-98 .hdi images (2026-09-18).** An Anex86 .hdi is a header and then the
raw sectors: dummy, hddtype, headersize, hddsize, sectorsize, sectors,
surfaces, cylinders, eight little-endian dwords, the header usually 4096
bytes (verified on a 10,817,536-byte image: header 4096, 256 bytes a sector,
33 sectors x 4 surfaces x 320 cylinders = 10,813,440). The geometry in it is
what the IPL1 partition table is written in, so it has to reach the disk
layer. DOSBox-X types a disk by its extension, and everything falls out of
one name: a seed ending in .hdi is mounted as `HardDiskDrive.hdi`
(`dosbox-driver.cpp`, `openHardDiskFromFile`, which also refuses a header it
cannot read), IMGMOUNT then skips geometry detection, the FAT layer knows a
hard disk when it sees one, and `imageDisk_Sparse` (`patches/src/ints/
bios_disk.cpp`) reads the header out of the sparse base the way the
file-backed `imageDisk` does - sector size, image base, C/H/S - so the
overlay still works in whole-file offsets and a savestate carries only what
was written. The export keeps the name (`dosdrv_hdd_name`), so a dumped disk
goes back into the hdd slot as an .hdi. The slot accepts `hdi`; run-native
and the rom path take one as they take an .hdd.

Verified on a PC-98 hard-disk game: IPL1 found, MS-DOS 6.20 boots from the
image to its banner and stops there - in CONFIG.SYS (HIMEM, EMM386) - and
DOSBox-X's own file-backed HDI path stops at the identical frame hashes, so
that is the machine, not the mount. From DOSBox-X's internal DOS instead
(bootDrive none, a .conf slot with `c:` and the game's name under
[autoexec]) the game runs to its attract screen; native == sandbox pictures
at 300, 600 and 899, save+load around every frame identical at 599, the gate
20 of 20. Booting the disk's own DOS is the open item, and it is upstream's.

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

**PC-98 .dcp dumps (2026-09-18).** DCP is a PC-98 image format DOSBox-X does
not read: a 162-byte header - media type, 160 per-track "in the file" flags,
an "every track" flag - then the present tracks back to back as plain sector
runs; a track not in the file reads as 0xE5 (the layout Neko Project II
accepts, its fdd_head_dcp.h). The core decodes one when it is OPENED:
`chimera_dcp_open` (waterbox/dosbox-driver.cpp) builds the flat image in memory
once per name - a mount and a boot share what they write - and `fopen_lock`
(patches/src/dos/dos_programs.cpp) hands every open of that name an fmemopen
over it. The result is byte for byte the raw image the disk would be as .hdm,
which DOSBox-X types by size (1232K for the 1.25 MB format every PC-98 game
disk is); media types 0x01-0x05 and 0x08 decode, the N88-BASIC ones are
refused with a line. Verified on an eight-disk PC-98 game: all disks decode
(154 of 154 tracks each), the machine boots to the game's FM-sound dialog
and, keyed past it, into the publisher's logo; a swap to disk 2 at frame 400 and 600
frames give native == sandbox digests; the gate is green. Note the harness
convention: rom/rom2.. extras carry no extension, so a .dcp only decodes under
its own name - the project's slot map, which is how the frontend mounts.

### 9. Configuration (KEEP composition, move declaration)

BizHawk composed one config string: base conf + a machine-year preset
(1981 IBM 5150 ... 1999 IBM Thinkpad 240) + joystick/speaker/sblaster/
memsize/cpu (cycles, type)/video-card sections + autoexec (@echo off,
imgmount lines) + user-provided .conf files, passed to the guest as a
mounted file. Chimera: identical composition, but the KNOBS are declared
in `waterbox.config` settings (formatted-HDD choice, CPU cycles/type/core,
RAM size, video card and video memory, sound blaster model/IRQ, PC speaker,
joysticks, mouse sensitivity, and the Windows-era disk knobs) so the
settings dialog is generated and movies carry them as sync settings. The
base .conf resource is copied from the BizHawk tree into this repo's
`waterbox/conf/` (author's own work). A user .conf can still ride along as
an input file.

**The machine-year presets are no longer a conf blob (2026-09-21).** See
"Configuration presets" below: they are declared presets the frontend
resolves into settings, and base.conf is the only .conf the core embeds.

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

## Configuration presets (2026-09-21)

The ten machines used to be a SETTING. `machinePreset` ("Configuration
Preset", ten options) was resolved by the core at boot: `dosdrv_compose_conf`
appended `waterbox/conf/dosbox-x.<year>.<model>.conf` after base.conf and
BEFORE the sections it built from settings, so the preset silently outranked
the user for every key the settings did not re-state. Seven settings then had
to document themselves as "Auto uses the configuration preset's default", the
grid stopped saying what the machine was, and a movie recorded a preset NAME
whose meaning the next core build could change under it.

They are now declared PRESETS (chimera `docs/project.md`, "Configuration
presets"): `waterbox.config` carries a top-level `presets` list, the wizard
shows a selector and an Apply button above the settings grid, and **Apply
writes the values into the settings and is then finished with the preset**.
The project pins the resolved values; nothing records a preset name. The core
knows nothing about presets at all: `dosdrv_compose_conf` is base.conf plus
what the settings say, and nothing else.

### The three-way decision, key by key

Every key that differs between a machine `.conf` and `dosbox-x.base.conf`.
(a) = maps onto a setting that already existed, (b) = a new setting was added
for it, (c) = stays in base.conf.

| conf key | section | base | what the machines want | decision |
| --- | --- | --- | --- | --- |
| `cputype` | cpu | auto | 8086, 80186, 286, 386, 486, pentium_ii, pentium_iii | (a) `cpuType` |
| `cycles` | cpu | auto | fixed 315 ... fixed 200000 | (a) `cpuCycles` |
| `core` | cpu | auto | normal (all ten) | (b) `cpuCore` |
| `sbtype` | sblaster | sb16 | none, gb, sb1, sb2, sbpro2, sb16, sb16vibra | (a) `soundBlasterModel` |
| `machine` | dosbox | svga_s3 | mda, cga, ega, mcga, svga_s3, svga_s3trio64 | (a) `videoCardType` |
| `memsize` | dosbox | 16 | 0, 1, 4, 6, 32, 64, 96, 128 | (a) `memsizeMB` |
| `memsizekb` | dosbox | 0 | 256, 640 | (b) `memsizeKB` |
| `vmemsize` | video | -1 | 8, 16 | (b) `videoMemoryMB` |
| `vesa modelist width limit` | video | 1280 | 0 | (b) `vesaModelistWidthLimit` |
| `vesa modelist height limit` | video | 1024 | 0 | (b) `vesaModelistHeightLimit` |
| `ver` | dos | (unset) | 7.1 | (b) `dosVersion` |
| `hard drive data rate limit` | dos | -1 | 0 | (b) `hardDriveDataRateLimit` |
| `floppy drive data rate limit` | dos | -1 | 0 | (b) `floppyDriveDataRateLimit` |
| `int13fakev86io` | fdc, primary | false | true | (b) `int13FakeIo` |
| `int13fakeio` | ide, primary | false | true | (b) `int13FakeIo` |
| `int13fakev86io` | ide, primary | false | true | (b) `int13FakeIo` |
| `int13fakeio` | ide, secondary | false | true | (b) `int13FakeIo` |
| `int13fakev86io` | ide, secondary | false | true | (b) `int13FakeIo` |
| `cd-rom insertion delay` | ide, secondary | 0 | 4000 | (b) `cdromInsertionDelayMs` |
| `Name` | ExtraInfo | - | the machine's own label | (c) - see below |

**The (c) list is one entry.** `[ExtraInfo] Name=` is not a DOSBox-X section
at all (there is no `ExtraInfo` in `dosbox.cpp`); it was the .conf file's
label for itself, and the preset's own `label` and `description` carry that
now. Nothing else was judged "not the user's business": every key the ten
machines set is reachable in the grid.

`int13FakeIo` is one setting for five keys, because the five are one feature
(Windows 3.11's and Windows 95's 32-bit disk access, which needs the BIOS
calls to move both IDE channels' and the floppy controller's registers and
raise v86-mode traps). Its description names all five. Splitting them would
offer four choices with no meaning apart from each other.

### What the "auto" options became

- `cpuType`: keeps `auto`, which is a real DOSBox-X value ("pick the type
  that suits the machine"). Default is now `486`.
- `videoCardType`, `soundBlasterModel`: `auto` was NOT a legal DOSBox-X value
  for `machine` or `sbtype` (`machines[]` and `sbtypes[]` in `dosbox.cpp` do
  not contain it) - it only ever meant "leave it to the preset". Removed.
  Defaults `svga_s3` and `sbpro2`.
- `cpuCycles`: `-1` meant "the preset's". Now a plain count, default 22000,
  and the composition ALWAYS writes `cycles = fixed N`: DOSBox-X's own `auto`
  and `max` chase the host's speed, which is a different machine on every PC
  and not one a movie can be replayed on.
- `memsizeMB`: `-1` meant "the preset's". Now a real size, default 32,
  minimum 0 (a pre-1987 PC's memory is `memsizeKB` alone).
- `pcSpeaker`: `auto` only ever meant base.conf's `true`. Options are now
  `disabled`/`enabled`, default `enabled`; a stale `auto` in an old project
  coerces to the default, which is the machine it already had.
- `midiDevice`: `auto` is renamed `none` and says what it is - nothing
  plugged into the MPU-401 port, which is how all ten machines shipped.
- `cpuCore` offers `normal`/`full`/`simple` and not DOSBox-X's `auto`: the
  recompiling cores are not compiled in (section 10), so `auto` would be a
  no-op option that looks like a choice.

Every one of these defaults is the 1993 IBM PS/2 53's value, which was the
default `machinePreset` - so a fresh project's machine is unchanged.

### The OS configs

`conf/dosbox-x.osconfig.{dos,windows95,windows98,windowsXP}.conf` are a second
axis in the author's BizHawk integration. **Nothing in this repo ever read
them**: `gen-assets.py` skipped any file with `osconfig` in the name, so they
were never embedded and `dosdrv_compose_conf` never saw one. They are not a
conf applied last here; they are dead files.

Their content decides the rest. `osconfig.dos.conf` is empty.
`osconfig.windows95.conf` and `osconfig.windows98.conf` are byte-identical to
each other, and their whole content - the `[video]`, `[dos]`, `[fdc]`, `[ide]`
block plus `cputype=pentium_mmx` and `sbtype=sb16vibra` - is already inside
the 1997 Aptiva and 1999 Thinkpad machine .confs. `osconfig.windowsXP.conf` is
the same block with `cputype=pentium_iii` and `cycles=fixed 400000`.

So the decision is **their differing keys become settings**, which is the same
decision the table above already forced: every key they set is now a real
setting. The axis is preserved without a second mechanism - "the 1999
Thinkpad, but at Windows XP's 400,000 cycles" is the 1999 preset and one edit
to CPU Cycles, in the grid, where it can be read. No extra presets were
invented for combinations that are already machines. The four files stay in
`conf/` as the authored reference.

### Proving the machines survived

Three things, in order of strength.

1. **Boot digests, before and after.** For each of the ten presets, 200 frames
   through run-native: video hash, audio hash and all five memory domain
   hashes, from the build with the preset blob (`--preset <id>`) and from the
   build with the preset resolved into settings (`tools/preset-args.py`).
   **All ten byte-identical**, and all ten distinct from each other - so the
   comparison is not vacuously comparing ten copies of one machine.
2. **Effective configuration, before and after.** The composed conf from each
   build, parsed the way DOSBox-X parses it (sections, `key = value`, last
   assignment wins) and compared as a map: **0 differences on all ten**. One
   cosmetic difference was found and removed on the way - the composition used
   to write `pcspeaker = Enabled`, the BizHawk spelling, where base.conf says
   `true`; `Value::set_bool` treats them identically, but writing base.conf's
   own spelling lets the two texts be compared line for line.
3. **A permanent gate leg**, `tools/check-preset-machines.py`, so this cannot
   rot: the ten .conf files stay in `conf/` and each preset's composed machine
   is compared against `base.conf + that .conf`, in both directions, over
   every key any .conf touches. A preset that stops producing its machine goes
   red. Negative control: dropping the `core =` line from the composition
   turned all ten red; putting a wrong `cpuType` into one preset and clearing
   `int13FakeIo` in another turned exactly those two red, naming the keys.

`tools/check-presets.py` is the declaration check the frontend cannot do for
itself (a `values` key that is not a setting is ignored, and a value outside a
setting's options is coerced to the default - both invisible in a machine that
booted). It runs in the gate and in `build-package.sh`. Negative control: a
typo'd name, an illegal enum value, an out-of-range int, a non-bool for a
bool, a duplicate id and a missing description were each detected, and each
named.

Not established: the wizard's selector itself was not seen on screen. The
frontend side was implemented and committed separately (chimera 8125d43); what
is proven here is that the declaration is legal, that the package loads with
it, and that the values a preset writes produce the machine they used to.

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

## Rebase log (2026-08-25)

The wbx diff `9215f53..origin/wbx` restricted to src/ + include/, opus
removals excluded, is 56 functional files. Rebased onto v2026.08.02 into
`patches/` (line endings normalized to LF on all sides first - the wbx
branch had normalized files upstream keeps as mixed CRLF, which is most of
the raw diff's apparent bulk):

- 27 files: upstream untouched between releases -> wbx version copied.
- 26 files: 3-way merged clean (`git merge-file`).
- 3 files needed hand resolution:
  - `dos_mscdex.cpp`: wbx side kept (the whole interface-type switch is
    dead in the sandbox; CDROM_Interface_Image is forced). Upstream's
    const-signature change landed outside the conflict.
  - `drive_fat.cpp`: upstream's restructured constructor (local diskfile,
    rawsize) kept, with the wbx memfile fallback grafted into its
    `!diskfile` branch; upstream's new cluster-chain reset AND the wbx
    drive-light line both kept in fatFile::Close.
  - `dos_programs.cpp`: six conflicts, all whitespace-triggered in GUI
    dialog paths; upstream sides taken with the wbx sandbox tweaks
    re-applied (getcwd commented, CurrentDir initialised, setbuf(newDisk)
    commented). The wbx ".cdrom" dialog filter entries were deliberately
    dropped - the .cdrom pseudo-file scheme does not exist here.

Also vendored (author's own work, from the wbx tree / BizHawk):
- `extern/vendored/SDL2` - the waterbox-modified SDL2 2.32 (signals and
  sigaction removed; NOT stock, so copied rather than submoduled).
- `extern/vendored/libco` - the coroutine slice.
- `waterbox/config.h` - the generated dosbox-x config for this target.
- `waterbox/conf/` - base + machine-year + OS preset .confs.
- `waterbox/hdd/` - pre-formatted FAT16 images (zstd, machine-generated).
- `waterbox/reference/` - the BizHawk driver (bizhawk.cpp/hpp, Makefile)
  as the porting reference for dosbox-driver.cpp and sources.mk.

NOT audited yet: whether the two upstream releases (06.02 -> 08.02) touch
the mechanisms the patches hook (Normal_Loop shape, mixer output path,
keyboard pending queue). The first native build (M1) is that audit.

## The gate looks at the binary, not only at the sources (2026-09-22)

Two legs, `wbx:clean` and `wbx:fresh`, both from one afternoon.
`build-package.sh` refused to package: `check-wbx` found 18 red-zone memory
operands in `mt32/sha1/sha1.cpp.o`, an object dated 2026-09-08 - two weeks
before `-mno-red-zone` reached the guest sysroot's `musl-gcc.specs` on
2026-09-21. **A flag added to a spec file rebuilds nothing.** Ninja sees no
changed input and skips the object, so twelve of 502 had quietly kept the old
rules, and this gate had passed 27 of 27 over the top of them, because nothing
in it had ever looked at the binary.

"Gate green" is not "the artifact conforms" when the conformance check only
runs at package time, and nobody runs `build-package.sh` between changes. So
`check-wbx` now runs in the gate, against the same `core.wbx` every other leg
tests.

`wbx:fresh` is the companion, and the one chimera-core-pcem's gate has carried
since a guest build that failed under dash was silently packaged as its
predecessor: a failed build leaves the PREVIOUS `core.wbx` in place, and every
leg below would then test a machine nobody changed and pass.

Negative controls. `wbx:fresh`: touching `dosbox-driver.cpp` turned it red and
named the file. `wbx:clean`: a host binary put in `core.wbx`'s place was
reported with 6,924 red-zone operands. The control that did NOT work is worth
recording - recompiling sha1.cpp with `-mred-zone` appended produced a
byte-identical object, because the specs file appends `-mno-red-zone` after the
user's flags. That is the property the specs delivery was chosen for: it
reaches every build path whatever that path's own flags say. It is also why the
only way to get a bad object is to have built it before the specs existed,
which is exactly what had happened.

## Bring-up log (2026-08-25, native)

The build is MESON (the user's standard): one root meson.build carries the
curated list for both builds; `meson setup build/meson-native && ninja -C
build/meson-native` produces run-native. Patches overlay at every
(re)configure. What the first bring-up established:

- extern/jaffarCommon must stay pinned at 8474151 (the wbx-era API);
  newer jaffarCommon reshaped MemoryFile and the patched dosbox files
  would all need porting.
- Upstream additions the old Makefile list lacked: SDL_sound decoders,
  libchdr modules + its lzma/, cqm.c, ipspatch/ipsmake, directlpt. libchdr
  inlines its OWN dr_flac; it builds as a small separate lib with
  -DDRFLAC_API=static or its globals collide with the SDL_sound flac
  decoder's.
- Three new patches beyond the wbx set, all upstream bugs exposed by
  compiling what their autotools apparently no longer does: a bare
  `extern "C"` closing SDL_sound_internal.h (poisons the next declaration
  of any C++ includer), mp3.cpp's commented-out mp3_seek_table.h include
  (defines mp3_t), and ceil_udivide (lives in dos/cdrom.h; the Staging-
  imported decoders expect it from support.h). Plus mixer.h's
  `static struct mixer_t` from the wbx move (invalid C++, g++13 rejects).
- DETERMINISM: video/audio/VRAM were deterministic immediately; DOS RAM
  was not, because the CMOS seeds from host time(NULL) at boot. run-native
  freezes the whole process clock to the SANDBOX EPOCH (1495889068 =
  2017-05-27 12:44:28 UTC, what miniBox pins) with strong time()/
  gettimeofday()/clock_gettime() definitions + TZ=UTC; after that, two
  runs are byte-identical on every digest. The guest gets this for free.
- The pre-formatted HDD .zst resources hold only the image HEAD (MBR +
  FATs, ~93KB); the full size comes from growing the memfile
  (--hdd-grow <enum size>: 21411840, 42823680, 252370944, 527966208,
  2111864832). That is how BizHawk used them too (the enum VALUE is the
  byte size).
- Proven end to end natively: boot to Z:\> (720x400, correct text), the
  composed conf pipeline, --type keyboard injection through the per-frame
  press/release sets, imgmount c -> _memFileDirectory -> imageDisk_Mem
  ("Drive C is mounted as HardDiskDrive.img"), and a DOS
  `echo saveme > c:\saved.txt` changing the Hard Disk Drive domain digest.

## Milestones

- [x] M1: repo skeleton, extern/dosbox-x @ v2026.08.02, wbx patch set
      rebased file by file into patches/, native reference build compiles
      and boots to the DOS prompt under a composed config. (2026-08-25)
- [~] M2: run-native frame loop - video/audio/input digests, deterministic
      across runs; keyboard reaches the machine and the HDD takes writes.
      Remaining: joystick/mouse exercise, refresh-rate change (a mode-set
      test), drive-activity flag.
- [x] M3: guest build (2026-08-25). Meson cross file generated by
      waterbox/setup-guest.sh over miniBox's musl/libstdc++ toolchain
      (build/meson-guest -> core.wbx, 22MB); waterbox.cpp adapts dosdrv_*
      to the guest ABI - input rides a guest-memory WbxInput block
      (GetInputBuffer) because the keyboard outgrows the packed u64;
      run-wbx.cpp drives it through libminiboxhost. Layout template
      {256M sbrk, 16M sealed, 16M invisible, 64M plain, 1024M mmap}.
      C_DIRECTLPT switched off in config.h (host parallel-port hardware
      has no place in the core; also unbuildable under musl). zlib and
      zstd now come BUNDLED from the tree (vs/zlib, libchdr/zstd,
      -DZSTD_DISABLE_ASM=1) for both builds - the guest has no system
      libs and the two machines must run the same bytes.
      run-gate.sh green ON THE FIRST GUEST RUN: boot leg (200 frames)
      and hdd leg (600 frames, typed DOS write) both
      native==sandbox==rerecord, whole gate 11 seconds wall.
- [x] M4: savedata export (2026-08-25, folded into the gate). The guest
      exports group six (one entry, HardDiskDrive.img); run-native
      --savedata-out writes the same file natively; the hdd leg diffs the
      trees byte for byte, with the typed `echo SAVEME > C:\SAVED.TXT`
      changing the image on both sides identically.
- [ ] M3: guest build (musl/GCC toolchain), seal + savestate + rerecord
      correctness; run-gate.sh native==sandbox==rerecord green.
- [ ] M4: HDD memfile + savedata export group + gate leg (a .com writes
      C:\SAVE.DAT; export trees must match everywhere).
- [x] M5: the Chimera package (2026-08-25). The guest COMPOSES its own
      dosbox-x.conf from the settings channel (formattedHardDisk,
      memsizeMB, cpuCycles, joysticksEnabled, mouseSensitivity and the
      rest of the machine; machinePreset was one of them until
      2026-09-21, see "Configuration presets") using the base conf and
      formatted-disk heads embedded at build time (gen-assets.py); a
      small setup.cpp patch
      lets ParseConfigFile read the composed text from memory when no
      real file exists, so run-native (which stages a work directory)
      and the frontend (which mounts nothing but rom+settings) reach
      identical configuration through dosdrv_compose_conf. The loaded
      file routes by rom.name extension when a host provides one, else
      by SNIFFING (text = extra conf, <= 2.88MB = floppy, else hard
      disk) - mounted files may be reopened after close, one open at a
      TIME is the miniBox contract. waterbox.config +
      default_keybinds.json are GENERATED from the real KBD_KEYS enum
      (gen-config.py, order verified): 121 buttons (100 keys + 2
      joysticks + 3 mouse + 6 disk-swap) through the wide-input
      SetButton export, 4 mouse axes, full keyboard bound 1:1 to the
      host. Chimera grew dynamic video size for this (config = buffer
      capacity, GetVideoWidth/Height = live size; 720x400 text in a
      1024x768 buffer). run-gate.sh: boot + hdd (settings-mounted
      formatted disk, typed DOS write, savedata trees) + machine-preset
      legs, all native==sandbox==rerecord. tests/run-frontend.sh 4/4:
      RAM slice identical to native through the real frontend, preset
      setting arrives, 121+4 keybinds adopted, savedata engine export
      == sandbox runner. deterministic package -> build/Cores/dosbox-x.chimeraCore.
- [~] M6: CD direct-mount DONE (2026-08-25). The BizHawk-era CD layer is
      gone: cdrom_image.cpp rebuilt from pristine upstream (keeping only
      the drive-light hooks and swapping its unity-includes for the
      separate objects both builds compile), cdrom.h reverted, the
      cd_read_callback/_cdData driver stubs deleted, the ".cdrom"
      pseudo-file remnants swept. CD images are plain hash-bound mounts
      read by upstream's own BinaryFile/CueFile/CHDFile; the guest
      sniffer recognises ISO9660 (CD001 at 0x8001) for nameless roms.
      The gate's cd leg boots a HAND-ROLLED minimal ISO
      (tests/gen-testiso.py - no iso tool on the machine, and the bytes
      are a pure function of the inputs), mounts it via autoexec, types
      the file it holds, and proves it DIFFERENTIALLY: the hit must
      digest differently from a miss, so a silently broken mount cannot
      pass. Two real bugs found on the way: sdlmain's global-config
      scavenging ran ClearExtraData() when no real dosbox-x.conf file
      exists, wiping the memory-composed conf's autoexec lines (patched:
      the in-memory conf counts as an existing config) - the hdd leg had
      been passing HOLLOWLY with both builds identically inputless, and
      it now asserts the typed write actually changes the disk; and the
      conf presets said machine=vga, which this DOSBox-X rejects (classic
      dosbox vga meant s3) - now machine=svga_s3 explicitly.
      Cue/bin DONE the same day: the cue sheet loads as the rom, its
      track file as a second mounted input (--extra-file in both
      runners - the shape the multi-file descriptor will take), one
      small patch letting the extensionless "rom" mount reach the cue
      content parse. Mouse and joystick DONE the same day: witnessed by
      hand-assembled DOS programs delivered on the test CD
      (tests/gen-testcom.py - JOYTEST renders the game port's button
      byte to video memory, MOUSETEST polls INT 33h), driven by one
      shared deterministic pattern (exercise-input.h) through the
      native input struct on one side and the guest's SetAxis/SetButton
      exports on the other, differential + native==sandbox==rerecord.
      Gate is now SEVEN legs (boot, hdd, preset, cd, cue,
      input:joystick, input:mouse), ~56s wall.
      LIVE CD SWAPPING done the same day: extra discs mount as
      rom2..romN (the convention the descriptor will formalize), the
      guest probes them and composes one imgmount swap list, and the
      gate's cdswap leg puts disc 2 in through the ACTUAL Swap CD
      buttons (next+swap rising together), reads it, brings disc 1 back
      through the previous-disc path during a typed pause, and reads
      that too - differential plus native==sandbox==rerecord. This
      needed a miniBox relaxation (commit 4ed92d8): opens became
      handles, so READ-ONLY mounts open concurrently with independent
      positions - a two-disc drive holds both images open, which the
      old one-open rule failed mid-boot; writable mounts stay
      single-open. Gate is EIGHT legs, ~65s. run-wbx grew --dump-video.
      FLOPPY SWAPPING done the same day, the cdswap proof on drive A:
      hand-rolled 1.44MB FAT12 floppies (tests/gen-testfloppy.py, one
      root file each, fixed timestamps), the rom2..romN convention
      generalized to every swappable image type, --swap-fd in both
      runners, floppy 2 in and back out through the floppy swap
      buttons. And the SAVEDATA LIFECYCLE closes end to end: the
      hddpersist leg reloads the hdd leg's EXPORTED image as the
      machine's disk and types the file the first run wrote - the
      differential run on a fresh formatted disk proves the read only
      succeeds because the modification persisted through
      export -> reload. Gate is TEN legs, ~81s.
      REAL CONTENT arrived (2026-08-25): the user's bootable Alley Cat
      floppy (tests/roms/, gitignored). This brought the bootDrive sync
      setting (none/a/c - the last autoexec line becomes `boot a:`,
      which never returns to the shell) and tests/run-roms.sh, the
      licensed-content leg: every image in tests/roms/ boots through
      both builds, digests must be native==sandbox==rerecord, final
      frames saved for eyeballing; SKIPs when the directory is empty so
      the public gates never depend on licensed bytes. Alley Cat boots
      to its IBM title in CGA 640x400 (dynamic video size again), a
      typed keypress reaches the game's joystick prompt identically in
      both builds, and the deliberately absurd long filename proved rom
      staging and rom.name handling clean.
      Remaining: CHD + CD audio with real content, the user's movies.
