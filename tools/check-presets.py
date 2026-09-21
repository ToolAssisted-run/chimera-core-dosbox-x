#!/usr/bin/env python3
"""Validate the PRESET declarations against the settings they write into.

A preset's `values` is a setting-name to value map, and the frontend coerces
every value through that setting's own declaration. A name that is not a
declared setting is IGNORED - named on the status line, but ignored - so a
generator that emits one produces a preset that silently does less than it says.
A value outside a setting's options is coerced to the default, which is worse:
the preset appears to work and builds a different machine.

Neither can be caught by looking at a machine that booted, so it is caught here.
This runs in the gate and in build-package.sh, and its negative control is in
the gate's own preset legs (break a value, watch it go red).

What it enforces, all of it from chimera docs/project.md, "Configuration
presets", and source/gui/Chimera.Emulation.Common/Waterbox/WaterboxConfig.cs
(PresetDecl, SettingDecl.Coerce):

  - every preset has a unique, non-empty id
  - every preset has a label and a description (the selector shows the label;
    the id is only the fallback, and "1981_ibm_xt5150" is not a label)
  - every `values` key is a declared setting
  - every value is legal for that setting's declared type, options and range
  - a preset does not move the machine setting or the renderer, which page one
    of the wizard owns and which decide what files the project takes
  - every `when` names a declared machine value, when the package has machines

usage: check-presets.py <waterbox.config>
"""
import json
import sys
from pathlib import Path

# The frontend asks these on page one and a preset is not allowed to move them
# (docs/project.md: "The machine setting and the renderer are not a preset's to
# move"). A package names its machine setting in "machineSetting".
NOT_A_PRESETS_TO_MOVE = ("renderer",)


def effective_type(decl):
    """SettingDecl.EffectiveType: declared, else inferred from options/default."""
    if decl.get("type"):
        return decl["type"].lower()
    if decl.get("options"):
        return "enum"
    d = decl.get("default")
    if isinstance(d, bool):
        return "bool"
    if isinstance(d, int):
        return "int"
    if isinstance(d, float):
        return "float"
    return "string"


def check_value(decl, value):
    """None if the value is legal for this setting, else why it is not."""
    t = effective_type(decl)
    if t == "bool":
        if not isinstance(value, bool):
            return f"is {value!r}, but the setting is a bool"
    elif t == "int":
        if isinstance(value, bool) or not isinstance(value, int):
            return f"is {value!r}, but the setting is an int"
        lo, hi = decl.get("min"), decl.get("max")
        if lo is not None and value < lo:
            return f"is {value}, below the setting's minimum of {lo}"
        if hi is not None and value > hi:
            return f"is {value}, above the setting's maximum of {hi}"
    elif t == "float":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return f"is {value!r}, but the setting is a float"
    else:
        if not isinstance(value, str):
            return f"is {value!r}, but the setting takes a string"
        opts = decl.get("options")
        if opts and value not in opts:
            return f"is {value!r}, which is not one of its options ({', '.join(opts)})"
    return None


def main():
    cfg = json.loads(Path(sys.argv[1]).read_text())
    presets = cfg.get("presets", [])
    decls = {s["name"]: s for s in cfg.get("settings", []) if "name" in s}
    machine_setting = cfg.get("machineSetting")
    # PresetDecl.When is compared against the machine's own first "when" value,
    # falling back to its id (WaterboxConfig.PresetsFor).
    machine_values = [(m["when"][0] if m.get("when") else m.get("id"))
                      for m in cfg.get("machines", [])]
    bad = []

    seen = set()
    for i, p in enumerate(presets):
        pid = p.get("id")
        where = f"preset {pid!r}" if pid else f"preset #{i}"
        if not pid:
            bad.append(f"{where} has no id")
        elif pid in seen:
            bad.append(f"{where} is declared twice: an id must be unique")
        else:
            seen.add(pid)
        if not p.get("label"):
            bad.append(f"{where} has no label, so the selector would show its id")
        if not p.get("description"):
            bad.append(f"{where} has no description")

        for m in p.get("when", []) or []:
            if machine_values and m not in machine_values:
                bad.append(f"{where} is gated on machine {m!r}, which this package "
                           f"does not have ({', '.join(str(v) for v in machine_values)})")

        values = p.get("values")
        if not values:
            bad.append(f"{where} writes no values, so Apply would do nothing")
            continue
        for name, value in values.items():
            if name not in decls:
                bad.append(f"{where} sets {name!r}, which is not a declared setting: "
                           f"the frontend would ignore it")
                continue
            if name == machine_setting or name in NOT_A_PRESETS_TO_MOVE:
                bad.append(f"{where} sets {name!r}, which the wizard asks on page one "
                           f"and a preset may not move")
                continue
            why = check_value(decls[name], value)
            if why:
                bad.append(f"{where} sets {name}, which {why}")

    for b in bad:
        print("  BAD:", b)
    keys = sorted({k for p in presets for k in (p.get("values") or {})})
    print(f"{len(presets)} presets over {len(keys)} settings "
          f"(of {len(decls)} declared): {len(bad)} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
