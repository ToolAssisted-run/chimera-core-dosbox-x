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
mb="${MINIBOX_DIR:-}"
frames=200
while getopts "f:m:" opt; do
	case "$opt" in
		f) frames="$OPTARG" ;;
		m) mb="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
if [ -z "$mb" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate/extern/chimera-common-minibox" ] \
			&& { mb="$candidate/extern/chimera-common-minibox"; break; }
	done
fi

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

# ---- the artifact itself ---------------------------------------------------
# WHAT IS ACTUALLY IN THE core.wbx, as opposed to what the sources say. Both of
# these legs exist because of the same day: build-package.sh refused to package
# because check-wbx found 18 red-zone memory operands in mt32/sha1/sha1.cpp.o,
# an object dated two weeks BEFORE -mno-red-zone was added to the guest
# sysroot's specs. A flag added to a spec file does not rebuild anything that
# is already built - ninja sees no changed input and skips it - so twelve
# objects had quietly kept the old rules, and this gate had passed 27 of 27
# over the top of them, because nothing here had ever looked at the binary.
#
# "Gate green" is not "the artifact conforms" if the conformance check only
# runs at package time. So it runs here.
if [ -n "$mb" ] && [ -f "$mb/source/guest/check-wbx.sh" ]; then
	if wbxout="$(sh "$mb/source/guest/check-wbx.sh" "$core" 2>&1)"; then
		echo "PASS wbx:clean (${wbxout#*core.wbx })"
	else
		echo "FAIL wbx:clean (the built core breaks the guest rules)"
		echo "$wbxout"; fail=1
	fi
else
	echo "FAIL wbx:clean (no miniBox checkout found; pass -m <dir> or set MINIBOX_DIR)"; fail=1
fi

# And that the binary is the one these sources describe. A build that fails
# leaves the PREVIOUS core.wbx in place, and every leg below would then test a
# machine nobody changed and pass - which is how a broken guest build was once
# packaged as its predecessor (chimera-core-pcem's gate has carried this leg
# ever since; this one did not).
stale=""
for src in "$here"/*.cpp "$here"/*.c "$here"/*.h "$here"/waterbox.config; do
	[ -f "$src" ] || continue
	[ "$src" -nt "$core" ] && stale="$stale $(basename "$src")"
done
if [ -z "$stale" ]; then
	echo "PASS wbx:fresh (core.wbx is newer than every source beside it)"
else
	echo "FAIL wbx:fresh (core.wbx is older than:$stale - run ninja -C build/meson-guest)"; fail=1
fi

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
# What the write CHANGED, read off the exported image rather than off a memory
# domain: the disk does not live in guest memory any more, so there is no domain
# to digest - and the export is the artefact a person actually gets, which makes
# it the better thing to hold this to.
timeout 900 "$rn" --workdir "$work/hdd0" --formatted-hdd 21mb --frames "$hddframes" --gate \
	--savedata-out "$work/sd-untyped" >/dev/null 2>&1
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL hdd (a run produced no digests)"; fail=1
elif [ ! -s "$work/sd-nat/HardDiskDrive.img" ]; then
	echo "FAIL hdd (nothing came out of the save-data channel)"; fail=1
elif cmp -s "$work/sd-nat/HardDiskDrive.img" "$work/sd-untyped/HardDiskDrive.img"; then
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

# ---- the declaration is legal ----------------------------------------------
# The ten machines are declared PRESETS: maps of setting values the wizard
# writes into the grid (chimera docs/project.md, "Configuration presets"). A
# `values` key that is not a declared setting is IGNORED by the frontend, and a
# value outside a setting's options is silently coerced to the default - neither
# is visible in a machine that booted, so both are caught here instead.
if python3 "$root/tools/check-presets.py" "$here/waterbox.config" > "$work/presets.log" 2>&1; then
	echo "PASS presets:declaration ($(tail -1 "$work/presets.log"))"
else
	echo "FAIL presets:declaration ($(grep -m1 BAD "$work/presets.log"))"; fail=1
fi

# ---- the presets still produce the machines they used to -------------------
# The most important leg of the conversion. Each machine was a .conf file the
# core appended to base.conf at boot; those files stay in conf/ as the
# reference, and this composes the machine from the PRESET'S SETTINGS instead
# and compares the effective configuration key by key, in both directions.
# A preset that quietly stopped producing its machine still boots and still
# looks like DOS, which is exactly why it needs a leg of its own.
if timeout 1800 python3 "$root/tools/check-preset-machines.py" "$rn" "$here/waterbox.config" \
   "$here/conf" "$work/presetmachines" > "$work/presetmachines.log" 2>&1; then
	echo "PASS presets:machines ($(tail -1 "$work/presetmachines.log"))"
else
	echo "FAIL presets:machines ($(grep -m1 BAD "$work/presetmachines.log"))"
	grep BAD "$work/presetmachines.log" | head -10; fail=1
fi

# ---- the machine-preset leg ------------------------------------------------
# An applied preset must produce a DIFFERENT machine from the default one (its
# values reach both builds), and the two builds must still agree on it. The
# arguments come from the DECLARATION, resolved the way the wizard's Apply
# resolves it, so this runs the preset a user would get and not a copy of it.
#
# TWO of them, at the two ends of the list, because they do not exercise the
# same settings: the 1983 XT is the only shape where RAM Size (KB) carries the
# whole memory size, and the 1997 Aptiva is the only shape that turns on the
# ex-preset keys the Windows era needs (video memory, the VESA mode list caps,
# the reported DOS version, the disk data rates, the INT 13h faking, the CD
# insertion delay). Between them every value the conversion moved out of a
# .conf file travels through the guest's own settings channel at least once.
base="$(timeout 600 "$rn" --workdir "$work/base2" --frames "$frames" --gate 2>/dev/null | digests)"
prev=""
for preset in 1983_ibm_xt5160 1997_ibm_aptiva_2140; do
	args="$(python3 "$root/tools/preset-args.py" "$here/waterbox.config" "$preset")"
	nat="$(timeout 600 "$rn" --workdir "$work/p-$preset" $args --frames "$frames" --gate 2>/dev/null | digests)"
	box="$(timeout 900 "$rw" "$core" $args --frames "$frames" 2>/dev/null | digests)"
	if [ -z "$nat" ] || [ -z "$box" ]; then
		echo "FAIL preset:$preset (a run produced no digests)"; fail=1
	elif [ "$nat" != "$box" ]; then
		echo "FAIL preset:$preset (native vs sandbox)"
		echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
	elif [ "$nat" = "$base" ]; then
		echo "FAIL preset:$preset (the preset did not change the machine)"; fail=1
	elif [ "$nat" = "$prev" ]; then
		echo "FAIL preset:$preset (indistinguishable from the previous preset)"; fail=1
	else
		echo "PASS preset:$preset (a machine of its own, and both builds agree on it)"
	fi
	prev="$nat"
done

# ---- the machine-knobs leg (the BizHawk-imported sync settings) ------------
# The imported settings (video card, CPU type, PC speaker, Sound Blaster)
# must reach the composed conf in both builds: a machine reshaped by all of
# them must DIFFER from the default machine, and the builds must agree on it.
# The same words on both sides, which is the point of run-native's --setting.
knobs="--setting videoCardType=cga --setting cpuType=8086 --setting pcSpeaker=disabled --setting soundBlasterModel=none"
nat="$(timeout 600 "$rn" --workdir "$work/knobs" $knobs --frames "$frames" --gate 2>/dev/null | digests)"
box="$(timeout 900 "$rw" "$core" $knobs --frames "$frames" 2>/dev/null | digests)"
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

# ...and the selector has to WRAP. It is a position, and DriveManager reads a
# position past the last disc as "put the FIRST one in" - so on a two-disc
# machine a second Next silently reinserts disc one, and no arrangement of
# inputs reaches disc two again for the rest of the session. Nothing says so.
# That is issue #47, and the check is that three Nexts land where one does.
wraptype='type D:\HELLO2.TXT
'
wrapnone="$(timeout 1200 "$rw" "$core" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --type "$wraptype" 2>/dev/null | digests)"
wrapone="$(timeout 1200 "$rw" "$core" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --swap-cd 100:1 --type "$wraptype" 2>/dev/null | digests)"
wrapthree="$(timeout 1200 "$rw" "$core" --rom "$work/test.iso" --extra-file "rom2=$work/disc2.iso" \
	--frames "$swapframes" --swap-cd 100:1 --swap-cd 115:2 --swap-cd 130:3 --type "$wraptype" 2>/dev/null | digests)"
if [ -z "$wrapone" ] || [ -z "$wrapthree" ]; then
	echo "FAIL cdswap-wrap (a run produced no digests)"; fail=1
elif [ "$wrapone" = "$wrapnone" ]; then
	echo "FAIL cdswap-wrap (one Next did not reach disc 2, so the wrap check proves nothing)"; fail=1
elif [ "$wrapthree" != "$wrapone" ]; then
	echo "FAIL cdswap-wrap (the selector walked past the last disc and did not come back: issue #47)"
	echo "--- one Next"; echo "$wrapone"; echo "--- three Nexts"; echo "$wrapthree"; fail=1
else
	echo "PASS cdswap-wrap (three Nexts on a two-disc machine land where one does)"
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
python3 "$here/tests/gen-testiso.py" "$work/input.iso" 	JOYTEST.COM="$work/coms/JOYTEST.COM" MOUSETEST.COM="$work/coms/MOUSETEST.COM" VMWTEST.COM="$work/coms/VMWTEST.COM" POSTEST.COM="$work/coms/POSTEST.COM" MODETEST.COM="$work/coms/MODETEST.COM" MICKTEST.COM="$work/coms/MICKTEST.COM" >/dev/null
inputframes=500
inputleg() {
	name="$1"; cmd="$2"; joyflag="$3"; exflag="${4:---exercise}"
	nat="$(timeout 900 "$rn" --workdir "$work/in-$name" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" --gate $exflag --type "$cmd" 2>/dev/null | digests)"
	quiet="$(timeout 900 "$rn" --workdir "$work/in-$name-q" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" --gate --type "$cmd" 2>/dev/null | digests)"
	box="$(timeout 1200 "$rw" "$core" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" $exflag --type "$cmd" 2>/dev/null | digests)"
	rr="$(timeout 3600 "$rw" "$core" --rom "$work/input.iso" $joyflag \
		--frames "$inputframes" $exflag --type "$cmd" --rerecord 2>/dev/null | digests)"
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
# Mouse Position alone, both speeds zero: moves by how far the position moved,
# as BizHawk's frontend does. Before that was ported (issue #61) this run drew
# exactly what the quiet one did, and the differential above said so.
inputleg mouse-position 'd:\mousetest.com
' "" --exercise-position
# The VMware absolute pointer, issue #135. VMWTEST asks port 5658h for absolute
# mode and stores what it answers in the IACA at 0000:04F0, so unlike every
# other leg here this one can ask the machine WHERE IT THINKS THE CURSOR IS
# instead of only whether the screen changed. Fed nothing - which is what
# happened until the driver made these calls, since only the SDL frontend ever
# did - the port answers 8000h,8000h, the middle of the screen, for ever.
inputleg vmware-mouse 'd:\vmwtest.com
' "" --exercise-position
vmwnat="$work/vmw-nat.bin"; vmwbox="$work/vmw-box.bin"
timeout 900 "$rn" --workdir "$work/vmw-n" --rom "$work/input.iso" \
	--frames "$inputframes" --exercise-position --type 'd:\vmwtest.com
' --ram-slice 0x4F0 8 "$vmwnat" >/dev/null 2>&1
timeout 1200 "$rw" "$core" --rom "$work/input.iso" \
	--frames "$inputframes" --exercise-position --type 'd:\vmwtest.com
' --ram-slice 0x4F0 8 "$vmwbox" >/dev/null 2>&1
hexof() { od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n'; }
vmwn="$(hexof "$vmwnat")"; vmwb="$(hexof "$vmwbox")"
if [ -z "$vmwn" ] || [ -z "$vmwb" ]; then
	echo "FAIL input:vmware-position (a run produced no slice)"; fail=1
elif [ "$vmwn" = "00800080" ]; then
	echo "FAIL input:vmware-position (the port still answers the centre of the screen)"; fail=1
elif [ "$vmwn" != "$vmwb" ]; then
	echo "FAIL input:vmware-position (native $vmwn vs sandbox $vmwb)"; fail=1
else
	echo "PASS input:vmware-position (the guest reads back $vmwn, native==sandbox)"
fi

# WHERE THE CURSOR ACTUALLY IS, as a number, checked against a position worked
# out without the core's help (issue #135 follow-up, 2026-09-22). Mouse Position
# X/Y is a FRACTION of the guest's screen, 0..65535, and the driver resolves it
# against the range INT 33h keeps for the mode it is in - so these expectations
# are just  (axis * screen) / 65536  masked by the mode's granularity, computed
# here and not read back out of anything the driver touched.
#
# Before this, the driver divided the axis by a constant 800 while the config
# declared a 2560 plane, and a HELD position never reached the machine at all:
# the same build answered 320,96 - the middle of the screen - for every value
# across the whole declared range.
mousepos() { # rom com axis -> "x y"
	rm -rf "$work/mp"; mkdir -p "$work/mp"
	timeout 900 "$rn" --workdir "$work/mp" --rom "$1" --frames 400 \
		--mouse-pos "$3" --type "$2
" --ram-slice 0x4F0 4 "$work/mp.bin" >/dev/null 2>&1
	python3 -c "
import sys
b = open(sys.argv[1], 'rb').read()
print(b[0] | (b[1] << 8), b[2] | (b[3] << 8))" "$work/mp.bin"
}
# 80-column text: the mouse range is 640x200, granularity 8 on both axes
abs80="$(mousepos "$work/input.iso" 'd:\postest.com' 0)|$(mousepos "$work/input.iso" 'd:\postest.com' 32768)|$(mousepos "$work/input.iso" 'd:\postest.com' 65535)"
want80="0 0|320 96|632 192"
# 40-column text: the range HALVES to 320x200, granularity 16 across
abs40="$(mousepos "$work/input.iso" 'd:\modetest.com' 32768)|$(mousepos "$work/input.iso" 'd:\modetest.com' 65535)"
want40="160 96|304 192"
if [ "$abs80" != "$want80" ]; then
	echo "FAIL input:mouse-absolute (80-column: wanted $want80, got $abs80)"; fail=1
elif [ "$abs40" != "$want40" ]; then
	echo "FAIL input:mouse-absolute (40-column: wanted $want40, got $abs40)"; fail=1
else
	echo "PASS input:mouse-absolute (the cursor lands where the axis says, and the same axis follows a mode change: $abs80 at 640 wide, $abs40 at 320)"
fi

# THE TWO PATHS MEASURE IN THE SAME UNITS. A position moved ten pixels and a
# Mouse Speed of ten pixels are the same movement, so the machine must be told
# the same thing - which is only true if the position path differences its
# PIXELS. Differencing the wire instead would leave this about a hundredfold
# out, and nothing else here would notice, because the absolute cursor would
# still land in the right place.
mickeys() { # extra-args... -> accumulated x mickeys
	rm -rf "$work/mk"; mkdir -p "$work/mk"
	timeout 900 "$rn" --workdir "$work/mk" --rom "$work/input.iso" --frames 400 \
		--mouse-pos 32768 "$@" --type 'd:\micktest.com
' --ram-slice 0x4F0 4 "$work/mk.bin" >/dev/null 2>&1
	python3 -c "
import sys
b = open(sys.argv[1], 'rb').read()
v = b[0] | (b[1] << 8)
print(v - 65536 if v > 32767 else v)" "$work/mk.bin"
}
mkStill="$(mickeys)"
mkPos="$(mickeys --mouse-nudge 300:1024)"   # 1024 wire = 10 px at 640 wide
mkSpd="$(mickeys --mouse-speed 300:10)"
mkNeg="$(mickeys --mouse-nudge 300:-1024)"
mkNegSpd="$(mickeys --mouse-speed 300:-10)"
if [ "$mkStill" != "0" ]; then
	echo "FAIL input:mouse-units (a held position invented $mkStill mickeys of movement)"; fail=1
elif [ "$mkPos" != "$mkSpd" ] || [ "$mkNeg" != "$mkNegSpd" ]; then
	echo "FAIL input:mouse-units (position $mkPos/$mkNeg vs speed $mkSpd/$mkNegSpd for the same ten pixels)"; fail=1
elif [ "$mkPos" = "0" ]; then
	echo "FAIL input:mouse-units (ten pixels of movement reached the machine as nothing)"; fail=1
else
	echo "PASS input:mouse-units (ten pixels is $mkPos mickeys whether asked for by position or by speed, and a held position is still)"
fi

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
