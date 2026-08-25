#!/bin/sh
# Overlays patches/ onto the extern/dosbox-x submodule working tree (plain
# copies - the wbx modifications are carried as full files, see docs/PLAN.md).
# Idempotent; the submodule shows as dirty while overlaid, which is the same
# arrangement chimera-core-ppsspp uses for its patch series. To see the real
# modification against upstream:  git -C extern/dosbox-x diff -w
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/.."
find patches -type f | while read -r f; do
	dst="extern/dosbox-x/${f#patches/}"
	cmp -s "$f" "$dst" 2>/dev/null || cp "$f" "$dst"
done
echo "patches overlaid onto extern/dosbox-x"
