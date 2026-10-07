# Building the DOSBox-X core

This repository builds DOSBox-X as a sandboxed guest (`core.wbx`) and packs it,
with its declarations, into one file: `dosbox-x.chimeraCore`. Chimera loads
that file. The steps below are the ones `.github/workflows/chimera.yml` runs on
a fresh clone on a public runner. Where this document and the workflow
disagree, the workflow is right.

Placeholders used below:

- `<chimera>`: the absolute path of a checkout of
  https://github.com/ToolAssisted-run/chimera
- `<minibox>`: `<chimera>/extern/chimera-common-minibox`, the miniBox submodule
  (the sandbox host and the guest toolchain)

Commands run from the root of this repository unless a step says otherwise.

## Requirements

- Linux on x86-64. CI uses GitHub's `ubuntu-latest` runner. Cores are built on
  Linux only. The package that comes out runs on Linux and on Windows.
- To build the core and run the core gate, CI installs:

  ```sh
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3
  ```

- To build Chimera and run the frontend gate and the contract tests, CI adds:

  ```sh
  sudo apt-get install -y --no-install-recommends mono-complete xvfb libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev
  ```

  and the .NET SDK 8.0. The workflow uses `actions/setup-dotnet@v4` with
  `dotnet-version: '8.0'`. By hand, Chimera's README gives
  `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`.
- `waterbox/tests/run-frontend.sh` also calls `zstd`. The workflow does not
  install it, so check the command exists.
- The workflow pins no compiler version: it uses the gcc that
  `build-essential` installs. `build-package.sh` records the gcc, binutils and
  musl versions and the miniBox commit in the package's `build.json`.
- The scripts of this repository download nothing. The guest toolchain is
  built from the miniBox sources (see "Build miniBox").

## Get the sources

This repository, with its submodules (`extern/dosbox-x`, `extern/jaffarCommon`).
The workflow uses `actions/checkout@v6` with `submodules: true`, which is:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-dosbox-x.git
cd chimera-core-dosbox-x
git submodule update --init
```

A Chimera checkout. The workflow checks out branch `main`. To build the core,
the package and the core gate, only the miniBox submodule is needed:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
git -C <chimera> submodule update --init extern/chimera-common-minibox
```

To also build Chimera itself (frontend gate, contract tests), the workflow
takes every Chimera submodule (`submodules: recursive`):

```sh
git -C <chimera> submodule update --init --recursive
```

Where the scripts look for Chimera when no option says:

- `waterbox/build-package.sh` and `waterbox/tests/run-frontend.sh`: `../chimera`
  beside this repository, then `$HOME/chimera`.
- `waterbox/run-gate.sh` (it wants miniBox): `./chimera-checkout`,
  `../chimera`, `$HOME/chimera`.
- `waterbox/setup-guest.sh`: `$HOME/chimera/extern/chimera-common-minibox`.

Pass the paths explicitly, as CI does, when the checkout is anywhere else.

## Build miniBox

Two builds of miniBox: the host library, and the C++ guest toolchain
(`-Dguest_cpp=true`). These are the workflow's commands:

```sh
mb=<minibox>
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The workflow keeps `build/meson-linux` and `build/meson-cpp` in
`actions/cache@v4`, keyed on the miniBox commit and its `meson.build`. By hand,
that is simply not deleting the two directories: the `[ -f ... ] ||` guards
skip the set-up when they exist.

## Build the core

### Patches

DOSBox-X is the submodule `extern/dosbox-x`, pinned and never committed to.
This repository's changes to it are FULL-FILE COPIES under `patches/`, at the
same relative path (`patches/src/dosbox.cpp` replaces
`extern/dosbox-x/src/dosbox.cpp`). There is no numbered patch series and no
`apply-patches.sh` here.

`waterbox/overlay-patches.sh` copies every file under `patches/` onto the
submodule's working tree. It is idempotent (a file that already matches is not
copied) and takes no options. `meson.build` runs it at every configure and
reconfigure, so a first build needs no manual step. After editing a file under
`patches/`, run it by hand before building:

```sh
./waterbox/overlay-patches.sh
```

While overlaid, `git status` shows `extern/dosbox-x` as modified. That is
expected. `git -C extern/dosbox-x diff -w` shows the real change against
upstream.

### The native reference and the sandbox runner

```sh
meson setup build/meson-native -Dminibox_dir=<minibox>
ninja -C build/meson-native
```

This builds two host programs:

- `build/meson-native/run-native`: DOSBox-X with this repository's driver,
  built for the host with no sandbox. The gate compares the sandboxed core
  against it.
- `build/meson-native/run-wbx`: a standalone program that runs `core.wbx`
  through miniBox's host library. It is built only when `-Dminibox_dir` is
  given, and it links `<minibox>/build/meson-linux`, so build miniBox first.

### The guest core

```sh
MINIBOX_DIR=<minibox> sh waterbox/setup-guest.sh -- -Dminibox_dir=<minibox>
ninja -C build/meson-guest
```

`waterbox/setup-guest.sh` writes a meson cross file for miniBox's guest
toolchain (`build/guest-cross.ini`, machine-local) and configures
`build/meson-guest`. It takes the miniBox directory from `MINIBOX_DIR` or
`-m <miniBox dir>`. It stops if
`<minibox>/build/meson-cpp/guest-sysroot/lib/libstdc++.a` is missing.

The result is `build/meson-guest/core.wbx`.

## Build the package

```sh
./waterbox/build-package.sh -m <minibox> -r <chimera>
```

Options:

- `-m <miniBox dir>`: the miniBox checkout. Default: `MINIBOX_DIR`, then
  `<chimera root>/extern/chimera-common-minibox`.
- `-r <chimera root>`: the Chimera checkout. Default: `../chimera`, then
  `$HOME/chimera`. The script stops if it finds none.

This repository's script has no `-o` option: the package always goes into a
Chimera checkout.

What it does, in order:

1. Configures the guest build if `build/meson-guest/build.ninja` is missing,
   then runs `ninja -C build/meson-guest core.wbx`.
2. Runs miniBox's `source/guest/check-wbx.sh` on `core.wbx` (the guest rules).
3. Runs `tools/check-presets.py` on `waterbox/waterbox.config`.
4. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json`, the licence texts (from `waterbox/package-licenses.json`)
   and `build.json` (what built the package).
5. Stamps the version into the staged `waterbox.config`.
6. Writes `<chimera>/build/Cores/dosbox-x.chimeraCore`, twice, and stops if the
   two files differ. It prints `package sha1 <hash>`: the package's SHA-1 is
   the core's identity.
7. Removes `<chimera>/build/CoreCache/dosbox-x-*`.

The version stamp:

- CI sets `CORE_VERSION` to the commit it built (`${{ github.sha }}`), and the
  script uses it as given.
- Without `CORE_VERSION`, the stamp is `<commit>+local` (12 hex digits), or
  `<commit>-dirty+local` when `git diff --quiet HEAD` finds changes. The
  overlaid `extern/dosbox-x` counts as a change, so a package built by hand
  normally reads `-dirty+local`.
- `versionDate` is the commit's date in UTC, never the build's.

A hand-built package is for testing. Chimera's publish step refuses a version
carrying `+local` or `-dirty`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets there as a file.

- In a Chimera SOURCE checkout the cores folder is `<chimera>/build/Cores/`.
  `build-package.sh -r <chimera>` has already written
  `dosbox-x.chimeraCore` there. Start Chimera with `build/ChimeraMono.sh` on
  Linux or `build\Chimera.exe` on Windows.
- In a release bundle, copy the `.chimeraCore` file into the `Cores` folder
  beside `Chimera.exe`, or into the folder chosen in
  File > Core Manager > Change folder...
- File > Core Manager lists what is in the folder. Refresh List rescans it.

The same package file works on Linux and on Windows: Chimera's sandbox
(miniBox) runs the guest inside it on either.

Released packages are on this repository's Releases page
(https://github.com/ToolAssisted-run/chimera-core-dosbox-x/releases): a
rolling `dev` release on every green push to `main`, and a dated
`nightly-YYYY-MM-DD` release from the scheduled run when `main` moved since the
last one.

## Run the gates

### Core gate

```sh
./waterbox/run-gate.sh
```

Options: `-f <frames>` (default 200) and `-m <miniBox dir>`.

It needs `build/meson-native/run-native`, `build/meson-native/run-wbx` and
`build/meson-guest/core.wbx`, and stops with the command to run when one is
missing. It needs a miniBox checkout too (`-m`, `MINIBOX_DIR`, or the places
listed under "Get the sources"); not finding one is a FAIL, not a skip.

It needs no content. The gate generates its own disks (a formatted hard disk,
floppies, an ISO and a cue/bin pair) and its own small DOS programs, so every
leg runs on a public runner. No leg is skipped.

Each leg prints `PASS` or `FAIL`; the exit status is non-zero when any failed.
The work directory is `waterbox/tests/work/`, emptied at the start. The legs:

- `wbx:clean`, `wbx:fresh`: the built `core.wbx` passes miniBox's
  `check-wbx.sh`, and ninja has nothing left to rebuild for it.
- `boot`, `hdd`, `cd`, `cue`, `cdswap`, `fdswap`, `hddpersist`, `slots`,
  `input:joystick`, `input:mouse`, `input:mouse-position`,
  `input:vmware-mouse`: the same machine and input through `run-native` and
  through the sandbox give the same digests of video, audio and every memory
  domain, and the sandbox gives them again with a state saved and loaded
  around every frame (`--rerecord`). Most carry a differential: the input or
  the disk must visibly change the machine. `hdd` also compares the save-data
  export trees.
- `knobs`, `preset:<id>`: settings that reshape the machine give a different
  machine from the default one, and both builds agree on it.
- `turbo`: frames that are not drawn leave the machine and the later pictures
  unchanged.
- `presets:declaration`, `presets:machines`: the ten declared presets are
  legal, and each still composes the machine its reference `.conf` describes.
- `cdswap-wrap`, `initialDrive`, `initialDrive:sandbox`, the mouse legs
  (`input:vmware-position`, `input:mouse-absolute`, `input:mouse-units`,
  `input:mouse-mode-change`, `input:pointer-port`), `mouse-driver:install`.
- `hddsize:<size>`: every size the Mount Formatted Hard Disk Drive setting
  offers mounts and boots.

### Frontend gate

The package inside the real frontend. Build Chimera first (in `<chimera>`,
the workflow's commands):

```sh
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from this repository, with the package installed:

```sh
./waterbox/tests/run-frontend.sh --chimera-root <chimera>
```

Options: `--chimera-root <path>` and `--frames <N>` (default 300).

It needs `<chimera>/build/Chimera.exe`, the package at
`<chimera>/build/Cores/dosbox-x.chimeraCore`, `run-native` and `run-wbx`,
`mono`, `zstd`, and `Xvfb` when `DISPLAY` is not set (it starts a private
display). It needs no content: it boots the blank formatted hard disk kept in
`waterbox/hdd/`.

Its checks are `hdd:frontend` (RAM identical to the native reference),
`settings:preset`, `keybinds` and `savedata:engine`. The last one needs
`<chimera>/build/meson-linux/chimera-run`; without it the check reports SKIP,
and the summary counts anything but PASS as failed. It ends with
`<n> ok, <m> failed`.

### Chimera's contract tests

Chimera's own tests, run against the packages in a cores folder: readable,
built for a guest ABI this frontend runs, a working factory, binding only
declared buttons, stamping a version. In `<chimera>`:

```sh
CHIMERA_CORES_DIR=<chimera>/build/Cores \
dotnet test source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj \
  -c Release --nologo \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

### Local images (not in CI)

```sh
./waterbox/tests/run-roms.sh
```

Option: `-f <frames>` (default 800). It boots every image you put in
`tests/roms/` (gitignored) through `run-native` and the sandbox and compares
them. With an empty folder it prints `SKIP`.

## Files the core needs at run time

Nothing licensed is in this repository or in the package. The user provides:

- Disks, through the project wizard's slots (`waterbox/file_slots.json`), at
  least one of:
  - floppy images (`.img`, `.ima`, `.xdf`, `.fdi`, `.hdm`, `.nfd`, `.d88`,
    `.dcp`), any number;
  - CD-ROM images (`.iso`, or `.cue` with its track files), any number;
  - one hard disk image (`.hdd`, `.img`, `.hdi`), mounted writable as C:;
  - one extra DOSBox-X configuration file (`.conf`).
- Firmware, only when a setting asks for it (`waterbox/waterbox.config`,
  `firmware`). The default settings need none: the machine boots to a DOS
  prompt with no disk and no ROM.
  - `MIDI Device`: `MT32_CONTROL.ROM` and `MT32_PCM.ROM`, or
    `CM32L_CONTROL.ROM` and `CM32L_PCM.ROM`.
  - `Use PC-98 Font ROM`: `FONT.ROM`.
  - `Use PC-98 Sound BIOS`: `SOUND.ROM`.
  - `Use PC-98 Rhythm Samples`: the six `2608_*.wav` files.
  - `Use IBM ROM BASIC`: the 32 KiB IBM BASIC ROM image.
  - `Use Real Video BIOS`: the video BIOS of the chosen card.

## Troubleshooting

- `miniBox C++ guest toolchain missing at ...` (`setup-guest.sh`): the
  `meson-cpp` build of miniBox is missing. Run the commands of "Build miniBox".
- `run-native not built`, `run-wbx not built (configure native with
  -Dminibox_dir=<miniBox>)`, `core.wbx not built` (`run-gate.sh`): build the
  named piece. `run-wbx` exists only when the native build was configured with
  `-Dminibox_dir`.
- `FAIL wbx:fresh (core.wbx is stale ...)`: a failed build leaves the previous
  `core.wbx` in place. Run `ninja -C build/meson-guest` and read its errors.
- `FAIL wbx:clean (no miniBox checkout found ...)`: pass `-m <minibox>` or set
  `MINIBOX_DIR`.
- `FAIL wbx:clean (the built core breaks the guest rules)`: a flag added to
  the guest toolchain rebuilds nothing, so objects built before it keep the old
  rules (`docs/PLAN.md`, "The gate looks at the binary"). Build again from an
  empty `build/meson-guest`.
- `chimera checkout not found; pass -r <path>` (`build-package.sh`) or
  `pass --chimera-root <path>` (`run-frontend.sh`): give the path.
- `extern/dosbox-x` is modified after a build: that is the overlay. Do not
  commit it, and do not reset it between builds.
- `extern/jaffarCommon` must stay at its pinned commit: the patched DOSBox-X
  files use that version's API (`docs/PLAN.md`, "Bring-up log").
- The cross file names the guest C++ headers by the host gcc's version
  (`gcc -dumpfullversion`). Build miniBox's guest toolchain and the core with
  the same gcc.
- `Xvfb not found`, `Chimera not built: ...`, `package not installed: ...`
  (`run-frontend.sh`): install `xvfb`, build Chimera, run `build-package.sh`.
