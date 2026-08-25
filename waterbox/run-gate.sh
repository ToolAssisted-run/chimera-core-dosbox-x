#!/bin/sh
# The equivalence gate: the same machine, configuration and input schedule
# through the native reference build and through the miniBox sandbox - the
# NATIVE side composed by the driver from machine knobs, the SANDBOX side
# composed by the guest itself from the settings channel, so the gate also
# proves the settings path end to end. Digests must match on video, audio and
# every memory domain; the sandbox must survive save/load state around every
# frame; savedata export trees must be byte-identical.
#
# Usage: ./run-gate.sh [-f frames]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
rn="$root/build/meson-native/run-native"
rw="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
frames=200
while getopts "f:" opt; do
	case "$opt" in
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

[ -x "$rn" ] || { echo "run-native not built (meson setup build/meson-native -Dminibox_dir=... && ninja)"; exit 1; }
[ -x "$rw" ] || { echo "run-wbx not built (configure native with -Dminibox_dir=<miniBox>)"; exit 1; }
[ -f "$core" ] || { echo "core.wbx not built (./waterbox/setup-guest.sh && ninja -C build/meson-guest)"; exit 1; }

work="$here/tests/work"
rm -rf "$work"
mkdir -p "$work"

fail=0
digests() { grep -E '^(videoHash|audioHash|domain\[)'; }

# ---- the boot leg ----------------------------------------------------------
# Power-on to the DOS prompt, nothing pressed, settings at their defaults.
nat="$(timeout 600 "$rn" --workdir "$work/boot" --frames "$frames" --gate 2>/dev/null | digests)"
box="$(timeout 900 "$rw" "$core" --frames "$frames" 2>/dev/null | digests)"
rr="$(timeout 1800 "$rw" "$core" --frames "$frames" --rerecord 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL boot (a run produced no digests)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL boot (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL boot (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
else
	echo "PASS boot ($frames frames, native==sandbox==rerecord)"
fi

# ---- the hdd leg -----------------------------------------------------------
# The formattedHardDisk SETTING mounts a blank 21MB FAT16 disk as C: (from the
# embedded head, decompressed and grown in guest memory); a DOS command typed
# at the prompt writes a file to it. The Hard Disk Drive domain digest and the
# savedata export trees must agree everywhere.
hddframes=600
typed='echo SAVEME > C:\SAVED.TXT
'
nat="$(timeout 900 "$rn" --workdir "$work/hdd" --formatted-hdd 21mb --frames "$hddframes" --gate \
	--type "$typed" --savedata-out "$work/sd-nat" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --formatted-hdd 21mb --frames "$hddframes" \
	--type "$typed" --savedata-out "$work/sd-box" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --formatted-hdd 21mb --frames "$hddframes" \
	--type "$typed" --rerecord 2>/dev/null | digests)"
hddnat="$(printf '%s\n' "$nat" | grep 'Hard Disk Drive')"
untyped="$(timeout 900 "$rn" --workdir "$work/hdd0" --formatted-hdd 21mb --frames "$hddframes" --gate 2>/dev/null \
	| grep 'Hard Disk Drive')"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL hdd (a run produced no digests)"; fail=1
elif [ -z "$hddnat" ]; then
	echo "FAIL hdd (no Hard Disk Drive domain in the native run)"; fail=1
elif [ "$hddnat" = "$untyped" ]; then
	# the lesson of the hollow pass: equal machines prove nothing if the
	# machine silently ignored the input
	echo "FAIL hdd (the typed DOS write did not change the disk)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL hdd (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL hdd (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
elif ! diff -r "$work/sd-nat" "$work/sd-box" >/dev/null 2>&1; then
	echo "FAIL hdd (savedata export trees differ)"
	diff -r "$work/sd-nat" "$work/sd-box" 2>&1 | head -5; fail=1
else
	echo "PASS hdd ($hddframes frames, settings-mounted disk, typed DOS write, native==sandbox==rerecord, savedata trees identical)"
fi

# ---- the machine-preset leg ------------------------------------------------
# A different machine year must produce a DIFFERENT machine (the setting
# reaches both builds), and the two builds must still agree on it.
nat="$(timeout 600 "$rn" --workdir "$work/cga" --preset 1983_ibm_xt5160 --frames "$frames" --gate 2>/dev/null | digests)"
box="$(timeout 900 "$rw" "$core" --preset 1983_ibm_xt5160 --frames "$frames" 2>/dev/null | digests)"
base="$(timeout 600 "$rn" --workdir "$work/base2" --frames "$frames" --gate 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL preset (a run produced no digests)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL preset (native vs sandbox on 1983_ibm_xt5160)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$nat" = "$base" ]; then
	echo "FAIL preset (the machine preset did not change the machine)"; fail=1
else
	echo "PASS preset (1983_ibm_xt5160: a different machine, and both builds agree on it)"
fi

# ---- the cd leg ------------------------------------------------------------
# A machine-generated ISO9660 image (gen-testiso.py, free content) mounted as
# D: through the autoexec, its one file typed at the prompt. The proof is
# DIFFERENTIAL: typing the file that exists must render its content, so the
# digests must DIFFER from typing a file that does not - a broken mount fails
# both ways identically and cannot pass. Then native==sandbox==rerecord.
python3 "$here/tests/gen-testiso.py" "$work/test.iso" "HELLO.TXT=@GREETINGS FROM THE CHIMERA CD GATE" >/dev/null
cdframes=800
hit='type D:\HELLO.TXT
'
miss='type D:\MISSING.TXT
'
nat="$(timeout 900 "$rn" --workdir "$work/cd" --rom "$work/test.iso" --frames "$cdframes" --gate --type "$hit" 2>/dev/null | digests)"
natmiss="$(timeout 900 "$rn" --workdir "$work/cdmiss" --rom "$work/test.iso" --frames "$cdframes" --gate --type "$miss" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --rom "$work/test.iso" --frames "$cdframes" --type "$hit" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --rom "$work/test.iso" --frames "$cdframes" --type "$hit" --rerecord 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL cd (a run produced no digests)"; fail=1
elif [ "$nat" = "$natmiss" ]; then
	echo "FAIL cd (the CD's file did not reach the screen - is D: mounted?)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL cd (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL cd (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
else
	echo "PASS cd ($cdframes frames, iso mounted and read through plain file mounts, native==sandbox==rerecord)"
fi

# ---- the cue leg -----------------------------------------------------------
# The same disc as a cue/bin pair: the cue sheet is the loaded file, its one
# MODE1/2048 track file arrives as a second mounted input - the shape a real
# game's cue+bin will take through the multi-file descriptor. Same
# differential proof as the cd leg.
cp "$work/test.iso" "$work/TRACK01.BIN"
printf 'FILE "TRACK01.BIN" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n' > "$work/test.cue"
nat="$(timeout 900 "$rn" --workdir "$work/cue" --rom "$work/test.cue" --extra-file "TRACK01.BIN=$work/TRACK01.BIN" \
	--frames "$cdframes" --gate --type "$hit" 2>/dev/null | digests)"
natmiss="$(timeout 900 "$rn" --workdir "$work/cuemiss" --rom "$work/test.cue" --extra-file "TRACK01.BIN=$work/TRACK01.BIN" \
	--frames "$cdframes" --gate --type "$miss" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --rom "$work/test.cue" --extra-file "TRACK01.BIN=$work/TRACK01.BIN" \
	--frames "$cdframes" --type "$hit" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --rom "$work/test.cue" --extra-file "TRACK01.BIN=$work/TRACK01.BIN" \
	--frames "$cdframes" --type "$hit" --rerecord 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL cue (a run produced no digests)"; fail=1
elif [ "$nat" = "$natmiss" ]; then
	echo "FAIL cue (the track file did not reach the screen - did the cue parse?)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL cue (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL cue (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
else
	echo "PASS cue ($cdframes frames, cue/bin pair through plain mounts, native==sandbox==rerecord)"
fi

# ---- the input leg ---------------------------------------------------------
# Mouse and joystick, witnessed by tiny hand-assembled DOS programs delivered
# on the test CD (gen-testcom.py): JOYTEST renders the game port's button
# byte into video memory, MOUSETEST polls INT 33h and renders position and
# buttons. The shared deterministic pattern (exercise-input.h) drives the
# native input struct on one side and the guest's SetAxis/SetButton exports -
# the frontend's exact path - on the other. Differential (exercised vs quiet
# must differ) plus native==sandbox==rerecord.
python3 "$here/tests/gen-testcom.py" "$work/coms" >/dev/null
python3 "$here/tests/gen-testiso.py" "$work/input.iso" 	JOYTEST.COM="$work/coms/JOYTEST.COM" MOUSETEST.COM="$work/coms/MOUSETEST.COM" >/dev/null
inputframes=500
inputleg() {
	name="$1"; cmd="$2"; joyflag="$3"
	nat="$(timeout 900 "$rn" --workdir "$work/in-$name" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" --gate --exercise --type "$cmd" 2>/dev/null | digests)"
	quiet="$(timeout 900 "$rn" --workdir "$work/in-$name-q" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" --gate --type "$cmd" 2>/dev/null | digests)"
	box="$(timeout 1200 "$rw" "$core" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" --exercise --type "$cmd" 2>/dev/null | digests)"
	rr="$(timeout 3600 "$rw" "$core" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" --exercise --type "$cmd" --rerecord 2>/dev/null | digests)"
	if [ -z "$nat" ] || [ -z "$box" ]; then
		echo "FAIL input:$name (a run produced no digests)"; fail=1
	elif [ "$nat" = "$quiet" ]; then
		echo "FAIL input:$name (the exercised inputs did not reach the machine)"; fail=1
	elif [ "$nat" != "$box" ]; then
		echo "FAIL input:$name (native vs sandbox)"
		echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
	elif [ "$box" != "$rr" ]; then
		echo "FAIL input:$name (rerecord diverges)"
		echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
	else
		echo "PASS input:$name ($inputframes frames, inputs shape the screen, native==sandbox==rerecord)"
	fi
}
inputleg joystick 'd:\joytest.com
' --joysticks
inputleg mouse 'd:\mousetest.com
' ""

exit $fail
