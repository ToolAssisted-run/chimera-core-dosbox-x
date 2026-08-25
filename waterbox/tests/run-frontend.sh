#!/bin/bash
# The frontend half of the gate: load the DOSBox-X package in Chimera (under
# Mono, on a private Xvfb display), boot a hard-disk image for a fixed number
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

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/dosbox-x.zip"
rn="$root/build/meson-native/run-native"
rw="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
[ -f "$emu_exe" ] || { echo "Chimera not built: $emu_exe" >&2; exit 1; }
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

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "$work/testdisk.hdd" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2"; }

# --- the machine the frontend builds must be the one the gate signed off on ---
settings_config "$work/config.base.ini" '{}'
if ! "$rn" --workdir "$work/native" --rom "$work/testdisk.hdd" --frames "$frames" \
	--ram-slice 0x0 0x10000 "$work/native.slice.bin" \
	> "$work/native.txt" 2>"$work/native.err"; then
	report "hdd:frontend" FAIL "native runner error: $(head -1 "$work/native.err")"
else
	if ! run_frontend "base" "$work/config.base.ini" "$frames" "$work/base.png"; then
		report "hdd:frontend" FAIL "no OK meta (see tests/work/base.log)"
	elif cmp -s "$work/native.slice.bin" "$work/base.ram.bin"; then
		report "hdd:frontend" PASS "$frames frames, RAM slice identical to the native reference"
	else
		report "hdd:frontend" FAIL "RAM slice differs"
	fi
fi

# --- a machine-shaping sync setting must reach the guest ---
settings_config "$work/config.cga.ini" '{"machinePreset": "1983_ibm_xt5160"}'
if run_frontend "cga" "$work/config.cga.ini" 60; then
	h1="$(grep '^ramhash=' "$work/base.meta.txt" | cut -d= -f2)"
	h2="$(grep '^ramhash=' "$work/cga.meta.txt" | cut -d= -f2)"
	if [ -n "$h2" ] && [ "$h1" != "$h2" ]; then
		report "settings:preset" PASS "machinePreset=1983_ibm_xt5160 booted a different machine"
	else
		report "settings:preset" FAIL "RAM hash did not change (h1=$h1 h2=$h2)"
	fi
else
	report "settings:preset" FAIL "run did not report OK (see tests/work/cga.log)"
fi

# --- the bindings the package ships must become the frontend's defaults ---
python3 "$here/forget-controller.py" "$work/config.base.ini" "$work/config.keys.ini" "DOSBox Controller"
if run_frontend "keys" "$work/config.keys.ini" 1; then
	if python3 "$here/check-keybinds.py" "$work/config.keys.ini" \
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
else
	rm -rf "$work/sd.engine" "$work/sd.box"
	# a neutral movie for the DOSBox Controller: 4 mouse axes at neutral
	# (position 1280,1024 on the 2560x2048 plane) in the console group before
	# its 111 buttons (mouse, swap, the 102 keys), then the two joysticks
	python3 - "$work/sd.movie.txt" <<'PYMOVIE'
import sys
entry = "| 1280, 1024,    0,    0," + "." * 111 + "|" + "." * 6 + "|" + "." * 6 + "|"
open(sys.argv[1], "w").write((entry + "\n") * 300)
PYMOVIE
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" timeout 900 "$crun" \
		"$package" "$work/testdisk.hdd" "$work/sd.movie.txt" --export-savedata "$work/sd.engine" ) \
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
