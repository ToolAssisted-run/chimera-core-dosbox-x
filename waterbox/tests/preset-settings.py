#!/usr/bin/env python3
"""Prints one declared preset's values as the settings JSON object the frontend
would hold after Apply.

The wizard's Apply writes a preset's values into the settings and is then
finished with it; nothing records the preset. A frontend leg that wants to run
one of the ten machines therefore has to arrive with the VALUES, which is what
this hands it (settings-config.py takes it from here).

usage: preset-settings.py <waterbox.config> <preset-id>
"""
import json
import sys
from pathlib import Path


def main():
    cfg = json.loads(Path(sys.argv[1]).read_text())
    want = sys.argv[2]
    for p in cfg.get("presets", []):
        if p.get("id") == want:
            print(json.dumps(p["values"]))
            return 0
    raise SystemExit(f"no preset {want!r} in {sys.argv[1]}")


if __name__ == "__main__":
    sys.exit(main())
