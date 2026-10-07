# AGENTS.md - DOSBox-X core for Chimera

This repository builds the DOSBox-X DOS/PC emulator as a sandboxed guest for
Chimera (https://github.com/ToolAssisted-run/chimera), a frontend for
tool-assisted speedruns. It produces one file, `dosbox-x.chimeraCore`: the
guest binary `core.wbx` plus the declarations Chimera reads. Chimera ships no
cores and downloads nothing; a user puts that file in Chimera's `Cores`
folder. `docs/BUILDING.md` has the detail behind every command here.

## Layout

- `extern/dosbox-x/` - upstream DOSBox-X, a pinned submodule. Never commit in it.
- `extern/jaffarCommon/` - submodule, pinned; keep the pin (`docs/PLAN.md`).
- `extern/vendored/` - SDL2, libco and SDL_net headers, committed here.
- `patches/` - FULL-FILE copies of the DOSBox-X files this core changes, at
  the same relative path as in `extern/dosbox-x/`.
- `meson.build` - the one build file for both builds (native and guest).
- `waterbox/overlay-patches.sh` - copies `patches/` onto the submodule tree.
- `waterbox/setup-guest.sh` - configures the guest build (`build/meson-guest`).
- `waterbox/build-package.sh` - builds and installs the package.
- `waterbox/run-gate.sh` - the core gate.
- `waterbox/tests/run-frontend.sh` - the frontend gate.
- `waterbox/tests/run-roms.sh` - optional leg over your own images in
  `tests/roms/` (gitignored).
- `waterbox/dosbox-driver.cpp`, `waterbox.cpp`, `guest-syscalls.cpp` - the
  driver and the guest ABI. `run-native.cpp`, `run-wbx.cpp` - the two runners.
- `waterbox/gen-config.py` - GENERATES `waterbox/waterbox.config` and
  `waterbox/default_keybinds.json`.
- `waterbox/file_slots.json` - the files the project wizard asks for.
- `waterbox/conf/`, `waterbox/hdd/` - the base configuration, the reference
  machine `.conf` files, the blank formatted disks.
- `guest-tools/` - the mouse drivers the core embeds as drive B:, with
  prebuilt binaries.
- `tools/` - preset checkers the gate and the package script call.
- `docs/PLAN.md` - design log: why each mechanism is the way it is.
- `build/` - all build output (gitignored).

## Set up the build environment

Linux x86-64. CI uses `ubuntu-latest`.

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3

git submodule update --init

CHIMERA=/absolute/path/to/chimera     # a Chimera checkout, branch main
MB=$CHIMERA/extern/chimera-common-minibox
git -C "$CHIMERA" submodule update --init extern/chimera-common-minibox

[ -f "$MB/build/meson-linux/build.ninja" ] || meson setup "$MB/build/meson-linux" "$MB"
meson compile -C "$MB/build/meson-linux"
[ -f "$MB/build/meson-cpp/build.ninja" ] || meson setup "$MB/build/meson-cpp" "$MB" -Dguest_cpp=true
meson compile -C "$MB/build/meson-cpp"
```

The frontend gate needs more (Chimera built, mono, xvfb, .NET SDK 8.0): see
`docs/BUILDING.md`.

## Build

```sh
# the native reference and the sandbox runner (the gate needs both)
meson setup build/meson-native -Dminibox_dir="$MB"
ninja -C build/meson-native

# the guest core and the package, in one step
./waterbox/build-package.sh -m "$MB" -r "$CHIMERA"
```

`build-package.sh` configures the guest build when needed, builds
`build/meson-guest/core.wbx`, checks it, and writes
`$CHIMERA/build/Cores/dosbox-x.chimeraCore`. To build the guest alone:

```sh
MINIBOX_DIR="$MB" sh waterbox/setup-guest.sh -- -Dminibox_dir="$MB"
ninja -C build/meson-guest
```

## Install the core into Chimera

`build-package.sh -r "$CHIMERA"` already put the package in
`$CHIMERA/build/Cores/`, the cores folder of a source checkout. For a release
bundle, copy the `.chimeraCore` file into the `Cores` folder beside
`Chimera.exe` (or the folder set in File > Core Manager > Change folder...);
Refresh List rescans. The same file works on Linux and on Windows.

A package built by hand stamps `<commit>+local` (`-dirty+local` when the tree
has changes, and the overlaid submodule counts as one). It is for testing.
Published packages come only from CI.

## Test before you commit

```sh
./waterbox/run-gate.sh                 # core gate; add -m "$MB" if miniBox is not found
```

Every leg must print `PASS`, and the exit status must be 0. The gate needs no
game, disk or ROM: it generates its content, so nothing is skipped. It needs
`run-native`, `run-wbx` and `core.wbx` built.

If your change touches the package, the declarations, settings, keybinds or
save data, also run the frontend gate (needs Chimera built):

```sh
./waterbox/tests/run-frontend.sh --chimera-root "$CHIMERA"
```

It must end with `0 failed`. CI runs both, plus Chimera's contract tests
against the package.

## Rules of this repository

- Never commit inside `extern/dosbox-x`. To change a DOSBox-X file, edit its
  full-file copy under `patches/` (add one, at the same relative path, if it
  is not there yet), run `./waterbox/overlay-patches.sh`, and rebuild BOTH
  builds. There is no numbered patch series here. The submodule shows as
  modified while overlaid; that is expected. Do not commit that state.
- Determinism is the product. The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate must
  round-trip. The gate compares native, sandbox and rerecord digests; a
  change that breaks one is a bug, not a gate problem.
- Run the gate before committing. A new leg needs a negative control: show it
  fails when the thing it checks is broken, and say so in the commit.
- Do not hand-edit `waterbox/waterbox.config` or
  `waterbox/default_keybinds.json`. Edit `waterbox/gen-config.py` and run it
  from `waterbox/` (`python3 gen-config.py`). The button order is the wire
  format: change it only with a matching guest change.
- Never commit game files, disk images, BIOS or firmware. Never add network
  access.
- The scripts under `waterbox/` must stay executable (git mode 100755): the
  workflow and `meson.build` run them directly.
- Documentation prose is plain ASCII.
- Commit messages: `type(scope): a sentence saying what is now true`, with
  the Chimera issue in parentheses when there is one, for example
  `fix(gate): ...` or `feat(mouse): ... (chimera#135)`. Types in use: feat,
  fix, docs, test, ci, chore. The body says what was measured and ends with
  the gate result.
- Problems with this core are reported in the chimera repository, not here.
- Do not edit `.github/workflows` unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every step, option and gate in detail.
- `docs/PLAN.md` - the design log; grep it before changing a mechanism.
- `.github/workflows/chimera.yml` - the authoritative build recipe.
- In the Chimera checkout: `docs/porting-a-core.md`, `docs/gates.md` (how a
  gate goes green on a broken thing) and `docs/core-manager.md`.
