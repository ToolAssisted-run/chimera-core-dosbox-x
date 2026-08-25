#!/bin/sh
# The licensed-content leg: boots every image the user placed in tests/roms/
# (gitignored - nothing here ships) through the native reference and the
# sandbox, requiring identical video/audio/memory digests and rerecord
# stability, and saves each machine's final frame for eyeballing.
#
# Routing by extension: floppy images boot from A:, hard disks boot from C:,
# disc images mount without booting (their machines land at the DOS prompt).
#
# Usage: ./run-roms.sh [-f frames]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
rn="$root/build/meson-native/run-native"
rw="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
frames=800
while getopts "f:" opt; do
	case "$opt" in
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

[ -x "$rn" ] && [ -x "$rw" ] && [ -f "$core" ] || { echo "build the runners and core.wbx first"; exit 1; }

romdir="$root/tests/roms"
work="$here/work/roms"
mkdir -p "$work"

fail=0
any=0
digests() { grep -E '^(videoHash|audioHash|domain\[)'; }

for rom in "$romdir"/*; do
	[ -f "$rom" ] || continue
	any=1
	name="$(basename "$rom")"
	short="$(echo "$name" | cut -c1-24)"
	case "$name" in
		*.img|*.ima|*.xdf|*.fdi) bootflag="--boot-drive a" ;;
		*.hdd) bootflag="--boot-drive c" ;;
		*) bootflag="" ;;
	esac
	nat="$(timeout 1200 "$rn" --workdir "$work/$name.native" --rom "$rom" $bootflag \
		--frames "$frames" --gate --dump-video "$work/$name." 2>/dev/null | digests)"
	box="$(timeout 1800 "$rw" "$core" --rom "$rom" $bootflag --frames "$frames" 2>/dev/null | digests)"
	rr="$(timeout 3600 "$rw" "$core" --rom "$rom" $bootflag --frames "$frames" --rerecord 2>/dev/null | digests)"
	# keep only the last dumped frame
	last="$(ls "$work/$name."*.tga 2>/dev/null | tail -1)"
	for f in "$work/$name."*.tga; do [ "$f" = "$last" ] || rm -f "$f"; done
	if [ -z "$nat" ] || [ -z "$box" ]; then
		echo "FAIL $short (a run produced no digests)"; fail=1
	elif [ "$nat" != "$box" ]; then
		echo "FAIL $short (native vs sandbox)"
		echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"; fail=1
	elif [ "$box" != "$rr" ]; then
		echo "FAIL $short (rerecord diverges)"
		echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"; fail=1
	else
		echo "PASS $short ($frames frames, native==sandbox==rerecord; frame at $last)"
	fi
done

[ "$any" -eq 1 ] || echo "SKIP (no files in tests/roms - licensed content stays local)"
exit $fail
