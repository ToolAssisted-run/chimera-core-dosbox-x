#!/bin/bash
# The frontend half of the gate: load the DOSBox-X package in Chimera's window
# (run headless: it needs no display), boot a hard-disk image for a fixed number
# of frames with nothing pressed, and require a slice of Conventional Memory
# to be byte-identical to the native reference. Then prove a machine-shaping
# sync setting arrives, the package's keybinds become the defaults, and the
# savedata export survives the whole pipeline (engine == sandbox runner).
#
# Usage: ./run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=300
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root|--minihawk-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/dosbox-x.chimeraCore"
rn="$root/build/meson-native/run-native"
rw="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$rn" ] || { echo "native reference not built" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

# the test disk: the machine-generated 21MB FAT16 head, padded to full size -
# free content, and the same image every leg boots
if [ ! -s "$work/testdisk.hdd" ]; then
	zstd -q -d -c "$wb/hdd/dosbox-x.hdd.fat16.21mb.img.zst" > "$work/head.img"
	python3 -c "d=open('$work/head.img','rb').read(); open('$work/testdisk.hdd','wb').write(d + b'\0'*(21411840-len(d)))"
fi
[ -s "$work/testdisk.hdd" ] || { echo "could not build the test disk" >&2; exit 1; }

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}"
	local job="$work/job.$tag.txt"
	local out="$work/$tag.ram.bin"
	[ "${lua_script:-}" = dump-controls.lua ] && out="$work/$tag.controls.json"
	{
		echo "frames=$nframes"
		echo "out=$out"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	local project
	project="$(game_project "$tag" "$cfg" "$package" "$work/testdisk.hdd")" || { echo "no project made (see tests/work/project.$tag/make.log)" > "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/${lua_script:-frontend-ram.lua}" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings
# settings_config was given for the config it runs with.
declare -A settings_of
settings_config() { python3 "$here/settings-config.py" "$1" "$2"; settings_of["$1"]="$2"; }
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/project.$1" settings="${settings_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=()
	rm -rf "$dir" && mkdir -p "$dir" && cp "$game" "$dir/"
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" "$dir/$(basename "$game")" \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

# --- the machine the frontend builds must be the one the gate signed off on ---
settings_config "$work/settings.base.json" '{}'
if ! "$rn" --workdir "$work/native" --rom "$work/testdisk.hdd" --frames "$frames" \
	--ram-slice 0x0 0x10000 "$work/native.slice.bin" \
	> "$work/native.txt" 2>"$work/native.err"; then
	report "hdd:frontend" FAIL "native runner error: $(head -1 "$work/native.err")"
else
	if ! run_frontend "base" "$work/settings.base.json" "$frames" "$work/base.png"; then
		report "hdd:frontend" FAIL "no OK meta (see tests/work/base.log)"
	elif cmp -s "$work/native.slice.bin" "$work/base.ram.bin"; then
		report "hdd:frontend" PASS "$frames frames, RAM slice identical to the native reference"
	else
		report "hdd:frontend" FAIL "RAM slice differs"
	fi
fi

# --- a machine-shaping sync setting must reach the guest ---
# The values of the 1983 IBM XT 5160 PRESET, written into the settings exactly
# as the wizard's Apply writes them (tools/preset-args.py resolves the same
# declaration the wizard reads) - there is no preset setting for the guest to
# resolve for itself any more, so what travels here is the settings themselves.
python3 "$here/preset-settings.py" "$wb/waterbox.config" 1983_ibm_xt5160 > "$work/preset.json"
settings_config "$work/settings.cga.json" "$(cat "$work/preset.json")"
if run_frontend "cga" "$work/settings.cga.json" 60; then
	h1="$(grep '^ramhash=' "$work/base.meta.txt" | cut -d= -f2)"
	h2="$(grep '^ramhash=' "$work/cga.meta.txt" | cut -d= -f2)"
	if [ -n "$h2" ] && [ "$h1" != "$h2" ]; then
		report "settings:preset" PASS "the 1983_ibm_xt5160 preset's values booted a different machine"
	else
		report "settings:preset" FAIL "RAM hash did not change (h1=$h1 h2=$h2)"
	fi
else
	report "settings:preset" FAIL "run did not report OK (see tests/work/cga.log)"
fi

# --- the bindings the package ships must become the frontend's defaults ---
# the keys the package ships are the machine's when nobody changed them (the
# window keeps only the keys a person changed; fresh settings have none)
lua_script=dump-controls.lua
cp "$work/settings.base.json" "$work/settings.keys.json"
if run_frontend "keys" "$work/settings.keys.json" 1; then
	lua_script=
	if python3 "$here/check-keybinds.py" "$work/keys.controls.json" \
		"$wb/default_keybinds.json" "DOSBox Controller" > "$work/keys.txt" 2>&1; then
		report "keybinds" PASS "$(cat "$work/keys.txt")"
	else
		report "keybinds" FAIL "$(head -1 "$work/keys.txt")"
	fi
else
	report "keybinds" FAIL "run did not report OK (see tests/work/keys.log)"
fi

# --- the savedata export must survive the whole pipeline ---
# chimera-run drives the same ce_session_savedata_* calls the Export Save Data
# menu item makes; its tree must equal the standalone sandbox runner's.
crun="$chimera_root/build/meson-linux/chimera-run"
if [ ! -x "$crun" ]; then
	report "savedata:engine" SKIP "chimera-run not built"
elif [ ! -x "$rw" ]; then
	# without it the comparison has nothing to compare against, and an empty
	# tree reads as a mismatch - say what actually went wrong instead
	report "savedata:engine" FAIL "sandbox runner not built: $rw (ninja -C build/meson-native)"
else
	rm -rf "$work/sd.engine" "$work/sd.box"
	# a neutral movie for the DOSBox Controller: 4 mouse axes at neutral
	# (position 32768,32768 - the middle of the guest's screen, whatever mode
	# it is in) in the console group before its 112 buttons (mouse, swap, the
	# 102 keys), then the two joysticks
	python3 - "$work/sd.movie.txt" <<'PYMOVIE'
import sys
entry = "|32768,32768,    0,    0," + "." * 112 + "|" + "." * 6 + "|" + "." * 6 + "|"
open(sys.argv[1], "w").write((entry + "\n") * 300)
PYMOVIE
	# a game runs only in a project (Chimera, 2026-10-26): one of the disk and that movie
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" "$crun" --new-project "$work/sd.chimeraProject" \
		"$package" "$work/testdisk.hdd" "$work/sd.movie.txt" ) > "$work/sd.make.log" 2>&1
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" timeout 900 "$crun" \
		--project "$work/sd.chimeraProject" "$package" --files "$work" --export-savedata "$work/sd.engine" ) \
		> "$work/sd.engine.log" 2>&1
	LD_LIBRARY_PATH="" timeout 900 "$rw" "$core" --rom "$work/testdisk.hdd" --frames 300 \
		--savedata-out "$work/sd.box" > "$work/sd.box.log" 2>&1
	nfiles="$(find "$work/sd.engine" -type f 2>/dev/null | wc -l)"
	if [ "$nfiles" -eq 0 ]; then
		report "savedata:engine" FAIL "engine exported no files (see tests/work/sd.engine.log)"
	elif diff -r "$work/sd.engine" "$work/sd.box" >/dev/null 2>&1; then
		report "savedata:engine" PASS "engine export tree == sandbox runner tree"
	else
		report "savedata:engine" FAIL "engine vs sandbox export trees differ"
	fi
fi

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
