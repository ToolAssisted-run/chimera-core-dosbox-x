#!/usr/bin/env python3
"""Hold every declared PRESET to the .conf file the machine came from.

The ten machines used to be .conf files the core appended to base.conf at boot
(waterbox/conf/dosbox-x.<year>.<model>.conf). They are now declared presets: a
map of setting values the frontend writes into the grid. That conversion is only
correct if a preset still produces the machine it used to - and a preset that
quietly stopped doing so is invisible, because it still boots, still looks like
a DOS machine, and only differs in a CPU type or half a megabyte of RAM.

So the .conf files stay in conf/, as the reference, and this compares:

  expected = the effective configuration of (base.conf + that machine's .conf)
  actual   = the effective configuration the driver composes from the preset's
             settings, read out of run-native with DOSDRV_PRINT_CONF

"Effective" means: parse the conf the way DOSBox-X does - sections, key = value,
last assignment wins - and compare the resulting section/key/value map. A
difference in either direction is a failure: a key the preset lost, and a key
the settings now set that the machine never asked for, are both wrong.

The comparison is over the keys that can differ - every key any of the .conf
files mentions, plus every key where the composed conf departs from base.conf.
The autoexec (mount lines) and the sections the harness itself drives are
excluded by name, because they are not the machine.

usage: check-preset-machines.py <run-native> <waterbox.config> <conf-dir> <workdir>
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
preset_args = import_module("preset-args").preset_args

# Not the machine: the autoexec is mount lines the harness composes, and
# ExtraInfo is the .conf's own label for itself (a section DOSBox-X does not
# have - the preset's `label` carries it now).
SKIP_SECTIONS = ("autoexec", "extrainfo")


def parse_conf(text):
    """{section: {key: value}}, last assignment winning, as DOSBox-X reads it."""
    out = {}
    section = None
    for line in text.split("\n"):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("%"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            out.setdefault(section, {})
            continue
        if section is None or "=" not in line:
            continue
        if section.lower() in SKIP_SECTIONS:
            continue
        key, _, value = line.partition("=")
        out[section][key.strip().lower()] = value.strip()
    return out


def flatten(conf):
    return {(s, k): v for s, keys in conf.items() for k, v in keys.items()}


def compose(run_native, workdir, args):
    """The conf the driver composes for these --setting arguments."""
    env = dict(os.environ, DOSDRV_PRINT_CONF="1", TZ="UTC")
    # --frames 0 still boots the machine, which is what makes the conf real
    # rather than a guess; its log goes to the same stream, so cut at the first
    # line DOSBox-X's logger writes.
    p = subprocess.run([run_native, "--workdir", workdir, "--frames", "1"] + args,
                       env=env, capture_output=True, text=True, timeout=600)
    text = p.stderr.split("LOG: Early LOG Init complete")[0]
    if "[autoexec]" not in text:
        raise SystemExit(f"run-native printed no conf (rc={p.returncode}):\n"
                         f"{p.stderr[-2000:]}")
    return text


def main():
    run_native, config, confdir, workdir = sys.argv[1:5]
    cfg = json.loads(Path(config).read_text())
    conf_dir = Path(confdir)
    Path(workdir).mkdir(parents=True, exist_ok=True)

    base = parse_conf((conf_dir / "dosbox-x.base.conf").read_text(errors="replace"))

    # Every key any machine .conf touches: what the conversion had to carry.
    confs = {}
    for p in cfg["presets"]:
        # "1981_ibm_xt5150" -> "dosbox-x.1981.ibm_xt5150.conf"
        year, _, model = p["id"].partition("_")
        f = conf_dir / f"dosbox-x.{year}.{model}.conf"
        if not f.is_file():
            raise SystemExit(f"preset {p['id']} has no reference conf at {f}")
        confs[p["id"]] = parse_conf(f.read_text(errors="replace"))
    preset_keys = {k for c in confs.values() for k in flatten(c)}

    # The composed machine with every setting at its declared default: the
    # difference between this and base.conf is what the harness and the
    # non-preset settings contribute, and it is the same for every preset.
    defaults = flatten(parse_conf(compose(run_native, f"{workdir}/defaults", [])))
    flat_base = flatten(base)
    harness_keys = {k for k, v in defaults.items() if flat_base.get(k) != v}

    bad = []
    for p in cfg["presets"]:
        pid = p["id"]
        args = preset_args(cfg, pid)
        actual = flatten(parse_conf(compose(run_native, f"{workdir}/{pid}", args)))
        # what the old composition would have produced: base.conf, then the
        # machine's own .conf, then the settings composition's own contribution
        expected = dict(flat_base)
        expected.update({k: defaults[k] for k in harness_keys})
        expected.update(flatten(confs[pid]))
        for k in sorted(preset_keys | harness_keys | {k for k in actual if actual[k] != flat_base.get(k)}):
            want, got = expected.get(k), actual.get(k)
            if want is None and got is None:
                continue
            if want == got:
                continue
            # a key the new composition leaves unset falls through to
            # base.conf's own value, which is the same machine
            if got is None and want == flat_base.get(k):
                continue
            if want is None and got == flat_base.get(k):
                continue
            bad.append(f"{pid}: [{k[0]}] {k[1]} is {got!r}, the machine says {want!r}")

    for b in bad:
        print("  BAD:", b)
    print(f"{len(cfg['presets'])} presets against their reference confs, "
          f"{len(preset_keys)} machine keys: {len(bad)} differences")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
