#!/bin/sh
# The equivalence gate: the same machine, config and input schedule through
# the native reference build and through the miniBox sandbox, requiring
# identical video/audio/memory-domain digests; the sandbox again with the
# whole machine round-tripped through save/load state around EVERY frame
# (the suspended dosbox coroutine included); and the savedata export trees
# byte-identical between the builds.
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

conf_base="$here/conf/dosbox-x.base.conf"
conf_mach="$here/conf/dosbox-x.1991.ibm_ps2_25_386.conf"

fail=0
digests() { grep -E '^(videoHash|audioHash|domain\[)'; }

# ---- the boot leg ----------------------------------------------------------
# Power-on to the DOS prompt, nothing pressed.
nat="$(timeout 600 "$rn" --workdir "$work/boot" --conf "$conf_base" --conf "$conf_mach" \
	--frames "$frames" --gate 2>/dev/null | digests)"
box="$(timeout 900 "$rw" "$core" --conf "$work/boot/dosbox-x.conf" --frames "$frames" 2>/dev/null | digests)"
rr="$(timeout 1800 "$rw" "$core" --conf "$work/boot/dosbox-x.conf" --frames "$frames" --rerecord 2>/dev/null | digests)"
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
# A 21MB FAT16 image mounted as C:, a DOS command typed at the prompt writing
# a file to it. The Hard Disk Drive domain digest and the savedata export
# trees must agree everywhere - this is the machine-writes-its-disk proof.
hddframes=600
zstd -q -d -f "$here/hdd/dosbox-x.hdd.fat16.21mb.img.zst" -o "$work/hdd21.img"
typed='echo SAVEME > C:\SAVED.TXT
'
nat="$(timeout 900 "$rn" --workdir "$work/hdd" --conf "$conf_base" --conf "$conf_mach" \
	--hdd "$work/hdd21.img" --hdd-grow 21411840 --frames "$hddframes" --gate \
	--type "$typed" --savedata-out "$work/sd-nat" 2>/dev/null | digests)"
box="$(timeout 1200 "$rw" "$core" --conf "$work/hdd/dosbox-x.conf" \
	--hdd "$work/hdd21.img" --hdd-grow 21411840 --frames "$hddframes" \
	--type "$typed" --savedata-out "$work/sd-box" 2>/dev/null | digests)"
rr="$(timeout 3600 "$rw" "$core" --conf "$work/hdd/dosbox-x.conf" \
	--hdd "$work/hdd21.img" --hdd-grow 21411840 --frames "$hddframes" \
	--type "$typed" --rerecord 2>/dev/null | digests)"
hddnat="$(printf '%s\n' "$nat" | grep 'Hard Disk Drive')"
if [ -z "$nat" ] || [ -z "$box" ]; then
	echo "FAIL hdd (a run produced no digests)"; fail=1
elif [ -z "$hddnat" ]; then
	echo "FAIL hdd (no Hard Disk Drive domain in the native run)"; fail=1
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
	echo "PASS hdd ($hddframes frames, typed DOS write, native==sandbox==rerecord, savedata trees identical)"
fi

exit $fail
