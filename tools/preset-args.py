#!/usr/bin/env python3
"""Resolves a declared PRESET into --setting arguments, the way the wizard's
Apply resolves it into the settings grid.

A preset is not a thing the core knows about: the frontend writes its values
into the settings and is then finished with it (chimera docs/project.md,
"Configuration presets"). So a harness that wants to run one of the ten
machines has to do what the frontend does, and this is that one line of it.
Both run-native and run-wbx take --setting NAME=VALUE, so one output serves
both.

usage: preset-args.py <waterbox.config> <preset-id>
"""
import json
import sys
from pathlib import Path


def preset_args(cfg, preset_id):
    """The --setting arguments for one preset id, as a list of strings."""
    presets = {p["id"]: p for p in cfg.get("presets", [])}
    if preset_id not in presets:
        raise SystemExit(f"no preset {preset_id!r} (have: {', '.join(presets)})")
    decls = {s["name"]: s for s in cfg.get("settings", [])}
    args = []
    for name, value in presets[preset_id]["values"].items():
        if name not in decls:
            # check-presets.py is what makes this impossible; say it anyway
            # rather than quietly writing a setting nothing reads
            raise SystemExit(f"preset {preset_id} sets {name!r}, which is not a setting")
        if isinstance(value, bool):
            value = "true" if value else "false"
        args += ["--setting", f"{name}={value}"]
    return args


def main():
    cfg = json.loads(Path(sys.argv[1]).read_text())
    print(" ".join(preset_args(cfg, sys.argv[2])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
