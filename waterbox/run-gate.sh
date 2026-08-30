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
# What a turbo run can be held to: everything except the whole-run video hash,
# which a run that skipped the first half cannot possibly match - the second
# half it did draw is compared instead.
turboDigests() { grep -E '^(tailVideoHash|audioHash|domain\[)'; }

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

# ---- the turbo leg ---------------------------------------------------------
# The RENDER layer switched off for the first half of the run and back on for
# the second. Everything the 8086 can see - and every picture of that second
# half - must be what it would have been: what turbo skips is the production of
# pixels, never the VGA's timing.
# --turbo-settle 1 excuses exactly ONE picture: DOSBox-X redraws a row only when
# it differs from the last row it drew, so the frame that resumes drawing is the
# one that rebuilds the whole surface. It converges the very next frame -
# measured, not assumed - and both runs skip the same frame, so nothing else is
# excused.
tnorm="$(timeout 900 "$rw" "$core" --frames "$frames" --turbo-settle 1 2>/dev/null | turboDigests)"
tturbo="$(timeout 900 "$rw" "$core" --frames "$frames" --turbo --turbo-settle 1 2>/dev/null | turboDigests)"
if [ -z "$tnorm" ] || [ "$tnorm" != "$tturbo" ]; then
	echo "FAIL turbo (an undrawn frame changed the machine or the picture after it)"
	echo "--- drawn"; echo "$tnorm"; echo "--- turbo"; echo "$tturbo"; fail=1
else
	echo "PASS turbo ($frames frames, half of them undrawn, same machine and same pictures)"
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

# ---- the machine-knobs leg (the BizHawk-imported sync settings) ------------
# The imported settings (video card, CPU type, PC speaker, Sound Blaster)
# must reach the composed conf in both builds: a machine reshaped by all of
# them must DIFFER from the default machine, and the builds must agree on it.
knobs_native="--video-card cga --cpu-type 8086 --pc-speaker disabled --sb-model none"
knobs_box="--setting videoCardType=cga --setting cpuType=8086 --setting pcSpeaker=disabled --setting soundBlasterModel=none"
nat="$(timeout 600 "$rn" --workdir "$work/knobs" $knobs_native --frames "$frames" --gate 2>/dev/null | digests)"
box="$(timeout 900 "$rw" "$core" $knobs_box --frames "$frames" 2>/dev/null | digests)"
base="$(timeout 600 "$rn" --workdir "$work/base3" --frames "$frames" --gate 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL knobs (a run produced no digests)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL knobs (native vs sandbox on the reshaped machine)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$nat" = "$base" ]; then
	echo "FAIL knobs (the settings did not change the machine)"; fail=1
else
	echo "PASS knobs (cga + 8086 + no speaker + no sblaster: a different machine, both builds agree)"
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

# ---- the cd-swap leg -------------------------------------------------------
# Two discs in the drive's swap list (the rom2..romN convention). Disc 2 goes
# in at frame 100 - through the actual Swap CD buttons in the sandbox, the
# driver's direct channel natively - its file is typed, the ORIGINAL disc
# returns at frame 300 (the previous-disc path) during a typed pause, and its
# file is typed too. Both commands succeed only if both swaps landed; the
# differential run without swapping fails the first command and passes the
# second, so the digests must differ.
python3 "$here/tests/gen-testiso.py" "$work/disc2.iso" "HELLO2.TXT=@THE SECOND DISC SPEAKS" >/dev/null
swapframes=600
swaptype='type D:\HELLO2.TXT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~type D:\HELLO.TXT
'
nat="$(timeout 900 "$rn" --workdir "$work/swap" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --gate --swap-cd 100:1 --swap-cd 300:0 --type "$swaptype" 2>/dev/null | digests)"
natnoswap="$(timeout 900 "$rn" --workdir "$work/swap0" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --gate --type "$swaptype" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --swap-cd 100:1 --swap-cd 300:0 --type "$swaptype" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --swap-cd 100:1 --swap-cd 300:0 --type "$swaptype" --rerecord 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL cdswap (a run produced no digests)"; fail=1
elif [ "$nat" = "$natnoswap" ]; then
	echo "FAIL cdswap (swapping discs changed nothing)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL cdswap (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL cdswap (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
else
	echo "PASS cdswap ($swapframes frames, disc 2 in and back out through the swap buttons, native==sandbox==rerecord)"
fi

# ---- the floppy-swap leg ---------------------------------------------------
# The cdswap proof on drive A: two machine-generated FAT12 floppies
# (gen-testfloppy.py) in the swap list, floppy 2 in at frame 100 through the
# floppy swap buttons, its file typed, floppy 1 back at frame 300 through the
# previous-disk path, its file typed too. Differential plus
# native==sandbox==rerecord.
python3 "$here/tests/gen-testfloppy.py" "$work/fd1.img" HELLO1.TXT "THE FIRST FLOPPY SPEAKS" >/dev/null
python3 "$here/tests/gen-testfloppy.py" "$work/fd2.img" HELLO2.TXT "THE SECOND FLOPPY SPEAKS" >/dev/null
fdtype='type A:\HELLO2.TXT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~type A:\HELLO1.TXT
'
nat="$(timeout 900 "$rn" --workdir "$work/fdswap" --rom "$work/fd1.img" --extra-file "rom2=$work/fd2.img" \
	--frames "$swapframes" --gate --swap-fd 100:1 --swap-fd 300:0 --type "$fdtype" 2>/dev/null | digests)"
natnoswap="$(timeout 900 "$rn" --workdir "$work/fdswap0" --rom "$work/fd1.img" --extra-file "rom2=$work/fd2.img" \
	--frames "$swapframes" --gate --type "$fdtype" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --rom "$work/fd1.img" --extra-file "rom2=$work/fd2.img" \
	--frames "$swapframes" --swap-fd 100:1 --swap-fd 300:0 --type "$fdtype" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --rom "$work/fd1.img" --extra-file "rom2=$work/fd2.img" \
	--frames "$swapframes" --swap-fd 100:1 --swap-fd 300:0 --type "$fdtype" --rerecord 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL fdswap (a run produced no digests)"; fail=1
elif [ "$nat" = "$natnoswap" ]; then
	echo "FAIL fdswap (swapping floppies changed nothing)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL fdswap (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL fdswap (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
else
	echo "PASS fdswap ($swapframes frames, floppy 2 in and back out through the swap buttons, native==sandbox==rerecord)"
fi

# ---- the hdd-persistence leg ------------------------------------------------
# The savedata lifecycle end to end: the hdd leg's run WROTE a file and
# exported the disk; that exported image now RELOADS as the machine's disk,
# and typing the file must print what was saved. The differential run types
# the same command on a fresh formatted disk, where the file does not exist -
# so the pass proves the modification genuinely persisted through
# export -> reload. Then native==sandbox==rerecord on the reloaded machine.
persistframes=500
persisttype='type C:\SAVED.TXT
'
if [ ! -f "$work/sd-nat/HardDiskDrive.img" ]; then
	echo "FAIL hddpersist (the hdd leg left no exported disk)"; fail=1
else
	cp "$work/sd-nat/HardDiskDrive.img" "$work/saved.hdd"
	nat="$(timeout 900 "$rn" --workdir "$work/persist" --rom "$work/saved.hdd" \
		--frames "$persistframes" --gate --type "$persisttype" 2>/dev/null | digests)"
	natfresh="$(timeout 900 "$rn" --workdir "$work/persist0" --formatted-hdd 21mb \
		--frames "$persistframes" --gate --type "$persisttype" 2>/dev/null | digests)"
	box="$(timeout 1200 "$rw" "$core" --rom "$work/saved.hdd" \
		--frames "$persistframes" --type "$persisttype" 2>/dev/null | digests)"
	rr="$(timeout 3600 "$rw" "$core" --rom "$work/saved.hdd" \
		--frames "$persistframes" --type "$persisttype" --rerecord 2>/dev/null | digests)"
	natvid="$(printf '%s\n' "$nat" | grep videoHash)"
	freshvid="$(printf '%s\n' "$natfresh" | grep videoHash)"
	if [ -z "$nat" ] || [ -z "$box" ]; then
		echo "FAIL hddpersist (a run produced no digests)"; fail=1
	elif [ "$natvid" = "$freshvid" ]; then
		echo "FAIL hddpersist (the saved file did not survive export and reload)"; fail=1
	elif [ "$nat" != "$box" ]; then
		echo "FAIL hddpersist (native vs sandbox)"
		echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
	elif [ "$box" != "$rr" ]; then
		echo "FAIL hddpersist (rerecord diverges)"
		echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
	else
		echo "PASS hddpersist ($persistframes frames, the write survives export and reload, native==sandbox==rerecord)"
	fi
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

# ---- the slots leg ---------------------------------------------------------
# The project's slot map (chimera docs/project.md): MIXED media, which the
# single-rom channel never allowed - two floppies on A:'s swap chain AND a
# CD on D:, every file mounted by its canonical name, list order = swap
# order. The sandbox reads the mounted "slots" JSON; the native side takes
# the same lists through --floppy/--cd. Floppy 2 goes in through the swap
# buttons and its file is typed, then a file from the CD - both only print
# if the mixed mounts and the swap landed. Differential (no swap fails the
# first type) plus native==sandbox==rerecord.
printf '{"floppy":["fd1.img","fd2.img"],"cdrom":["test.iso"]}' > "$work/slots.json"
mixedtype='type A:\HELLO2.TXT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~type D:\HELLO.TXT
'
nat="$(timeout 900 "$rn" --workdir "$work/slots" --floppy "fd1.img=$work/fd1.img" --floppy "fd2.img=$work/fd2.img" \
	--cd "test.iso=$work/test.iso" --frames "$swapframes" --gate --swap-fd 100:1 --type "$mixedtype" 2>/dev/null | digests)"
natnoswap="$(timeout 900 "$rn" --workdir "$work/slots0" --floppy "fd1.img=$work/fd1.img" --floppy "fd2.img=$work/fd2.img" \
	--cd "test.iso=$work/test.iso" --frames "$swapframes" --gate --type "$mixedtype" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --extra-file "slots=$work/slots.json" --extra-file "fd1.img=$work/fd1.img" \
	--extra-file "fd2.img=$work/fd2.img" --extra-file "test.iso=$work/test.iso" \
	--frames "$swapframes" --swap-fd 100:1 --type "$mixedtype" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --extra-file "slots=$work/slots.json" --extra-file "fd1.img=$work/fd1.img" \
	--extra-file "fd2.img=$work/fd2.img" --extra-file "test.iso=$work/test.iso" \
	--frames "$swapframes" --swap-fd 100:1 --type "$mixedtype" --rerecord 2>/dev/null | digests)"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL slots (a run produced no digests)"; fail=1
elif [ "$nat" = "$natnoswap" ]; then
	echo "FAIL slots (the floppy swap changed nothing)"; fail=1
elif [ "$nat" != "$box" ]; then
	echo "FAIL slots (native vs sandbox)"
	echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
elif [ "$box" != "$rr" ]; then
	echo "FAIL slots (rerecord diverges)"
	echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
else
	echo "PASS slots ($swapframes frames, mixed floppies+CD by canonical names via the slot map, native==sandbox==rerecord)"
fi

# ---- the biggest disk this package OFFERS must be one it can mount ---------
# The writable drive C: is a memory file: a sandbox has nowhere to put a real
# one, and a movie needs the disk to be part of the machine. So the guest's mmap
# arena has to be big enough for whatever formattedHardDisk is set to - and it
# was not. The arena was 1024 MiB and the largest option is 2014mb, so that
# option had never once worked; it failed with "could not create the hard disk
# drive mem file", which is the resize failing one line further up.
#
# Every size the package offers is mounted here, largest first, because the one
# nobody tests is the one that is broken.
for size in 2014mb 504mb 241mb 41mb 21mb; do
	if timeout 900 "$rw" "$core" --formatted-hdd "$size" --frames 3 >"$work/hddsize.txt" 2>&1; then
		echo "PASS hddsize:$size (mounted and booted)"
	else
		echo "FAIL hddsize:$size ($(grep -iE 'could not|failed' "$work/hddsize.txt" | head -1))"
		fail=1
	fi
done

exit $fail
