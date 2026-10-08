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
	# chimera-checkout is where CI puts it (.github/workflows/chimera.yml); the
	# other two are a developer's machine. Missing it is a FAIL, not a skip: a
	# leg that quietly skips is a leg that cannot fail.
	for candidate in "$root/chimera-checkout" "$root/../chimera" "$HOME/chimera"; do
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
# Asked of the BUILD SYSTEM, not of a list of files. A hand-kept list was wrong
# twice in one day - it counted waterbox.config (read by the frontend, compiled
# by nobody) and then run-native.cpp (a host tool) - and each time the leg went
# red in a way no rebuild could satisfy. An instrument that cannot be satisfied
# is worse than none, because the next person learns to ignore it. ninja knows
# exactly what core.wbx is built from; a dry run that has work to do means the
# binary under test is not the one these sources describe.
if pending="$(ninja -C "$root/build/meson-guest" -n core.wbx 2>&1)"; then
	case "$pending" in
		*"no work to do"*)
			echo "PASS wbx:fresh (core.wbx is up to date with everything it is built from)" ;;
		*)
			echo "FAIL wbx:fresh (core.wbx is stale - ninja would rebuild: $(echo "$pending" | grep -c '^\[') step(s); run ninja -C build/meson-guest)"; fail=1 ;;
	esac
else
	echo "FAIL wbx:fresh (could not ask ninja about build/meson-guest)"; fail=1
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

# ---- where the shell starts (Initial Drive) ----------------------------------
# DOSBox-X leaves its shell on Z:, its own drive of built-in commands, where
# nothing the project supplied lives. Initial Drive moves it: 'auto' takes the
# first mounted of A:, D:, C:; a named drive that is not mounted falls back to
# that order; with nothing mounted at all the shell stays on Z:.
#
# The answer is read from DOS itself (DOS_GetDefaultDrive, via run-native's
# --print-drive), not from the composed configuration - the conf only says what
# was asked for, and a line DOSBox-X rejected would still be in it.
idw="$work/idrive"; mkdir -p "$idw"
python3 "$here/tests/gen-testfloppy.py" "$idw/f.img" HELLO.TXT hi >/dev/null
python3 "$here/tests/gen-testiso.py" "$idw/c.iso" HELLO.TXT=@hi >/dev/null
drv() { rm -rf "$idw/w"; mkdir -p "$idw/w"
	timeout 600 "$rn" --workdir "$idw/w" --frames 200 --print-drive "$@" 2>/dev/null \
		| sed -n 's/^drive=//p'; }
idgot="$(drv --rom "$idw/f.img")$(drv --rom "$idw/c.iso")$(drv --formatted-hdd 21mb)"
idgot="$idgot$(drv --floppy A.IMG="$idw/f.img" --cd C.ISO="$idw/c.iso")"
idgot="$idgot$(drv --rom "$idw/c.iso" --formatted-hdd 21mb)"
idgot="$idgot$(drv --rom "$idw/c.iso" --formatted-hdd 21mb --setting initialDrive=c)"
idgot="$idgot$(drv --rom "$idw/f.img" --setting initialDrive=c)$(drv)"
# floppy, CD, hdd, floppy+CD, CD+hdd, CD+hdd asked for C, C asked with no hdd, nothing
idwant="ADCADCAZ"
if [ "$idgot" = "$idwant" ]; then
	echo "PASS initialDrive (auto picks A: then D: then C:, a named drive is honoured, a missing one falls back, nothing mounted stays on Z:)"
else
	echo "FAIL initialDrive (wanted $idwant, got ${idgot:-nothing})"; fail=1
fi
# And the SANDBOX reads the setting through its own settings channel, which
# --print-drive cannot see. The prompt shows the drive letter, so the picture
# differs between C: and D:; native and sandbox must agree on each.
idnatC="$(timeout 600 "$rn" --workdir "$idw/nc" --frames 200 --gate --rom "$idw/c.iso" --formatted-hdd 21mb --setting initialDrive=c 2>/dev/null | digests)"
idboxC="$(timeout 900 "$rw" "$core" --frames 200 --rom "$idw/c.iso" --formatted-hdd 21mb --setting initialDrive=c 2>/dev/null | digests)"
idboxD="$(timeout 900 "$rw" "$core" --frames 200 --rom "$idw/c.iso" --formatted-hdd 21mb 2>/dev/null | digests)"
if [ -z "$idnatC" ] || [ -z "$idboxC" ]; then
	echo "FAIL initialDrive:sandbox (a run produced no digests)"; fail=1
elif [ "$idnatC" != "$idboxC" ]; then
	echo "FAIL initialDrive:sandbox (native vs sandbox with initialDrive=c)"; fail=1
elif [ "$idboxC" = "$idboxD" ]; then
	echo "FAIL initialDrive:sandbox (the sandbox ignored the setting: C: and auto (D:) drew the same)"; fail=1
else
	echo "PASS initialDrive:sandbox (the guest honours the setting, and native == sandbox)"
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
python3 "$here/tests/gen-testiso.py" "$work/input.iso" 	JOYTEST.COM="$work/coms/JOYTEST.COM" MOUSETEST.COM="$work/coms/MOUSETEST.COM" VMWTEST.COM="$work/coms/VMWTEST.COM" POSTEST.COM="$work/coms/POSTEST.COM" MODETEST.COM="$work/coms/MODETEST.COM" MICKTEST.COM="$work/coms/MICKTEST.COM" MODEMICK.COM="$work/coms/MODEMICK.COM" PORTTEST.COM="$work/coms/PORTTEST.COM" PORTMICK.COM="$work/coms/PORTMICK.COM" >/dev/null
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

# THE POSITION APPLIES WHILE Mouse Set Position IS HELD, AND ONLY THEN
# (chimera#210, chimera#211; user-decided 2026-10-08). The four things that
# follow from it, each read back as a number from the DOS cursor at the end of
# the run (80-column text, 640x200, as above). The button is held across the
# test program's start, because the program resets the mouse when it starts:
#   held, then let go with the axes back at rest - an untouched cell - the
#     pointer stays where it was put (it used to go back to the middle);
#   never held, the axes at the far corner: the pointer never goes there;
#   held, and a speed given too: the position wins (a hand moving a real
#     mouse gives both, and the speed used to win);
#   let go, then a speed: the pointer moves from where it was put.
mouseset() { # run-native options -> "x y"
	rm -rf "$work/ms"; mkdir -p "$work/ms"
	timeout 900 "$rn" --workdir "$work/ms" --rom "$work/input.iso" --frames 400 "$@" --type 'd:\postest.com
' --ram-slice 0x4F0 4 "$work/ms.bin" >/dev/null 2>&1
	python3 -c "
import sys
b = open(sys.argv[1], 'rb').read()
print(b[0] | (b[1] << 8), b[2] | (b[3] << 8))" "$work/ms.bin"
}
msGot="$(mouseset --mouse-pos 65535 --mouse-release 280:32768:32768)|$(mouseset --mouse-pos 65535 --mouse-release 0)|$(mouseset --mouse-pos 0 --mouse-speed 320:40)|$(mouseset --mouse-pos 0 --mouse-release 280:32768:32768 --mouse-speed 320:40)"
msWant="632 192|320 96|0 0|40 0"
if [ "$msGot" != "$msWant" ]; then
	echo "FAIL input:mouse-set-position (stays|never goes|position wins|speed moves it: wanted $msWant, got $msGot)"; fail=1
else
	echo "PASS input:mouse-set-position (let go, the pointer stays put; not held, the position is ignored; held, it beats a speed; let go, a speed moves it: $msGot)"
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
# a speed applies only with Mouse Set Position let go: released ten frames before
mkSpd="$(mickeys --mouse-release 290 --mouse-speed 300:10)"
mkNeg="$(mickeys --mouse-nudge 300:-1024)"
mkNegSpd="$(mickeys --mouse-release 290 --mouse-speed 300:-10)"
if [ "$mkStill" != "0" ]; then
	echo "FAIL input:mouse-units (a held position invented $mkStill mickeys of movement)"; fail=1
elif [ "$mkPos" != "$mkSpd" ] || [ "$mkNeg" != "$mkNegSpd" ]; then
	echo "FAIL input:mouse-units (position $mkPos/$mkNeg vs speed $mkSpd/$mkNegSpd for the same ten pixels)"; fail=1
elif [ "$mkPos" = "0" ]; then
	echo "FAIL input:mouse-units (ten pixels of movement reached the machine as nothing)"; fail=1
else
	echo "PASS input:mouse-units (ten pixels is $mkPos mickeys whether asked for by position or by speed, and a held position is still)"
fi

# A HELD POSITION STAYS STILL ACROSS A MODE CHANGE (chimera#176). The INT 33h
# range is what an axis resolves against, and it changes with the video mode:
# Windows 98 booted through 512x200 into 640x400, and an X-only nudge reached
# the guest with a 100-pixel Y move, because the driver differenced a pixel of
# the old range against a pixel of the new one. MODEMICK halves the range under
# a position held at the middle; the counters must read no motion at all.
mmLeg="$(rm -rf "$work/mm"; mkdir -p "$work/mm"
	timeout 900 "$rn" --workdir "$work/mm" --rom "$work/input.iso" --frames 400 \
		--mouse-pos 32768 --type 'd:\modemick.com
' --ram-slice 0x4F0 4 "$work/mm.bin" >/dev/null 2>&1
	python3 -c "
import sys
b = open(sys.argv[1], 'rb').read()
s = lambda v: v - 65536 if v > 32767 else v
print(s(b[0] | (b[1] << 8)), s(b[2] | (b[3] << 8)))" "$work/mm.bin")"
if [ "$mmLeg" = "0 0" ]; then
	echo "PASS input:mouse-mode-change (a held position halved under by a mode change moved nothing)"
else
	echo "FAIL input:mouse-mode-change (a held position across a mode change counted $mmLeg mickeys of motion)"; fail=1
fi

# THE CHIMERA POINTER PORT (chimera#135). Windows 9x hands a ring-3 program's
# IN to the VMM, which performs it in ring 0 and returns only the value read,
# so VMware's backdoor (answers in EBX/ECX/EDX) cannot reach guest-tools'
# chimabs there. The core offers the position where the value read is all
# there is: 5666h answers "CP", 5664h is X << 16 | Y, every frame, held or
# moved. And a guest that reads it places its own pointer, so a moved position
# must reach it as no relative motion - where MICKTEST, which never reads the
# port, counted $mkPos mickeys for the same move above.
portWords() { # args... -> the four words at 0:04F0
	rm -rf "$work/pt"; mkdir -p "$work/pt"
	timeout 900 "$rn" --workdir "$work/pt" --rom "$work/input.iso" --frames 400 "$@" \
		--ram-slice 0x4F0 8 "$work/pt.bin" >/dev/null 2>&1
	python3 -c "
import struct, sys
print(' '.join('%04x' % v for v in struct.unpack('<4H', open(sys.argv[1], 'rb').read())))" "$work/pt.bin"
}
ptHeld="$(portWords --mouse-pos 49152:16384 --type 'd:\porttest.com
')"
ptMoved="$(portWords --mouse-pos 49152:16384 --mouse-nudge 300:256 --type 'd:\porttest.com
')"
ptMick="$(portWords --mouse-pos 32768 --mouse-nudge 300:1024 --type 'd:\portmick.com
')"
# The port's reader acts only when what it reads changes, so a pointer placed
# again on the spot it was placed before - after a speed moved it away - must
# read differently: the lowest bit of X is a mark that turns over then, and
# only then (let go and held again with no speed between, it reads the same).
ptAgain="$(portWords --mouse-pos 49152:16384 --mouse-release 280 --mouse-speed 300:25 --mouse-repress 330 --type 'd:\porttest.com
')"
ptSame="$(portWords --mouse-pos 49152:16384 --mouse-release 280 --mouse-repress 330 --type 'd:\porttest.com
')"
if [ "$ptHeld" != "5043 4000 c000 0000" ] || [ "$ptMoved" != "5043 4000 c100 0000" ]; then
	echo "FAIL input:pointer-port (held: $ptHeld, moved: $ptMoved; want 5043 4000 c000 / c100)"; fail=1
elif [ "$ptAgain" != "5043 4000 c001 0000" ] || [ "$ptSame" != "5043 4000 c000 0000" ]; then
	echo "FAIL input:pointer-port (placed again after a speed: $ptAgain, want ...c001; with none: $ptSame, want ...c000)"; fail=1
elif [ "$ptMick" != "0000 0000 0000 0000" ]; then
	echo "FAIL input:pointer-port (a reader of the port still got relative motion: $ptMick)"; fail=1
else
	echo "PASS input:pointer-port (\"CP\" and X<<16|Y, held and moved; a reader gets no relative motion where MICKTEST got $mkPos mickeys)"
fi

# USE CHIMERA MOUSE DRIVER (chimera#135). The setting puts the driver disk in
# B: and has the composed autoexec run B:\INSTALL /AUTO against C: before
# anything else. No Windows is licensed here, so a Windows tree is built out
# of echo lines on a formatted disk with the setting OFF (nothing may install
# then), and THAT disk is booted with the setting on - natively and in the
# sandbox, which must export the same disk - before the installer's work is
# read back out of the export. B:\INSTALL /AUTO typed again afterwards must
# change nothing: the installer runs at every start.
mdw="$work/mousedrv"; rm -rf "$mdw"; mkdir -p "$mdw"
fatread() { python3 "$here/tests/fat-read.py" "$@"; }
crlf() { printf '%s\r\n' "$@"; }
mdtree() { # name conf-lines... -> $mdw/name.hdd
	n="$1"; shift
	{ echo "[autoexec]"; for l in "$@"; do printf '%s\n' "$l"; done; } > "$mdw/$n.conf"
	timeout 600 "$rn" --workdir "$mdw/w-$n" --formatted-hdd 21mb --setting chimeraMouseDriver=false --extra-conf "$mdw/$n.conf" \
		--frames 300 --savedata-out "$mdw/sd-$n" >/dev/null 2>&1
	cp "$mdw/sd-$n/HardDiskDrive.img" "$mdw/$n.hdd" 2>/dev/null
}
mdboot() { # tree out args... -> $mdw/out.hdd, natively
	t="$1"; o="$2"; shift 2
	timeout 600 "$rn" --workdir "$mdw/w-$o" --rom "$mdw/$t.hdd" --setting chimeraMouseDriver=true \
		--frames 400 "$@" --savedata-out "$mdw/sd-$o" >/dev/null 2>&1
	cp "$mdw/sd-$o/HardDiskDrive.img" "$mdw/$o.hdd" 2>/dev/null
}
mdTyped='b:\install /auto
'
mdtree tree9x 'md c:\windows' 'md c:\windows\system' 'echo x>c:\windows\system\vmm32.vxd' \
	'echo [windows]>c:\windows\win.ini' 'echo load=>>c:\windows\win.ini' 'echo run=>>c:\windows\win.ini'
mdboot tree9x out9x --type "$mdTyped"
timeout 900 "$rw" "$core" --rom "$mdw/tree9x.hdd" --setting chimeraMouseDriver=true --frames 400 \
	--type "$mdTyped" --savedata-out "$mdw/sd-box9x" >/dev/null 2>&1
mdtree tree31 'md c:\windows' 'md c:\windows\system' 'echo [boot]>c:\windows\system.ini' \
	'echo shell=progman.exe>>c:\windows\system.ini' 'echo mouse.drv=mouse.drv>>c:\windows\system.ini' \
	'echo [boot.description]>>c:\windows\system.ini' 'echo mouse.drv=Microsoft>>c:\windows\system.ini' \
	'echo @echo off>c:\autoexec.bat' 'echo cd windows>>c:\autoexec.bat' 'echo win>>c:\autoexec.bat'
mdboot tree31 out31 --type "$mdTyped"
mdboot tree31 out31boot --setting bootDrive=c
md9x0="$(crlf '[windows]' 'load=' 'run=')"; md9x1="$(crlf '[windows]' 'load=' 'run=C:\WINDOWS\CHIMABS.EXE')"
md310="$(crlf '[boot]' 'shell=progman.exe' 'mouse.drv=mouse.drv' '[boot.description]' 'mouse.drv=Microsoft')"
md311="$(crlf '[boot]' 'shell=progman.exe' 'mouse.drv=vbmouse.drv' '[boot.description]' 'mouse.drv=Microsoft')"
mdae0="$(crlf '@echo off' 'cd windows' 'win')"; mdae1="$(crlf '@echo off' 'cd windows' 'C:\WINDOWS\VBMOUSE.EXE' 'win')"
mdwhy=""
[ "$(fatread "$mdw/tree9x.hdd" 'WINDOWS\WIN.INI')" = "$md9x0" ] || mdwhy="$mdwhy; the 9x tree is not what was built (or the setting off installed)"
[ "$(fatread "$mdw/out9x.hdd" 'WINDOWS\WIN.INI')" = "$md9x1" ] || mdwhy="$mdwhy; 9x WIN.INI run= is not exactly CHIMABS once"
fatread "$mdw/out9x.hdd" 'WINDOWS\CHIMABS.EXE' | cmp -s - "$root/guest-tools/chimabs/CHIMABS.EXE" || mdwhy="$mdwhy; 9x CHIMABS.EXE is not the disk's"
cmp -s "$mdw/out9x.hdd" "$mdw/sd-box9x/HardDiskDrive.img" || mdwhy="$mdwhy; native and sandbox exported different 9x disks"
[ "$(fatread "$mdw/out31.hdd" 'WINDOWS\SYSTEM.INI')" = "$md311" ] || mdwhy="$mdwhy; 3.1 SYSTEM.INI is not [boot] mouse.drv=vbmouse.drv alone"
fatread "$mdw/out31.hdd" 'WINDOWS\SYSTEM\VBMOUSE.DRV' | cmp -s - "$root/guest-tools/vbados/VBMOUSE.DRV" || mdwhy="$mdwhy; 3.1 VBMOUSE.DRV is not the disk's"
[ "$(fatread "$mdw/out31.hdd" 'AUTOEXEC.BAT')" = "$mdae0" ] && ! fatread "$mdw/out31.hdd" 'WINDOWS\VBMOUSE.EXE' >/dev/null \
	|| mdwhy="$mdwhy; under the core's own DOS the 3.1 install touched AUTOEXEC.BAT or copied the TSR"
[ "$(fatread "$mdw/out31boot.hdd" 'AUTOEXEC.BAT')" = "$mdae1" ] || mdwhy="$mdwhy; booting C:, AUTOEXEC.BAT does not load VBMOUSE.EXE before WIN"
fatread "$mdw/out31boot.hdd" 'WINDOWS\VBMOUSE.EXE' | cmp -s - "$root/guest-tools/vbados/VBMOUSE.EXE" || mdwhy="$mdwhy; booting C:, VBMOUSE.EXE is not the disk's"
[ "$(fatread "$mdw/tree31.hdd" 'WINDOWS\SYSTEM.INI')" = "$md310" ] || mdwhy="$mdwhy; the 3.1 tree is not what was built"
if [ -z "$mdwhy" ]; then
	echo "PASS mouse-driver:install (95/98: CHIMABS + run= once; 3.1: VBMOUSE.DRV + [boot]; TSR only when C: boots; native == sandbox)"
else
	echo "FAIL mouse-driver:install (${mdwhy#; })"; fail=1
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
