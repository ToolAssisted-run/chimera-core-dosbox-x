# chimera-core-dosbox-x

**DOSBox-X as a Chimera waterbox core** - the [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
DOS/PC emulator, compiled into miniBox's deterministic sandbox and packaged as a
Chimera core (`core.wbx` + `waterbox.config`), the same shape as
[chimera-core-ppsspp](https://github.com/ToolAssisted-run/chimera-core-ppsspp),
[chimera-core-quickernes](https://github.com/ToolAssisted-run/chimera-core-quickernes) and
[chimera-core-neshawk](https://github.com/ToolAssisted-run/chimera-core-neshawk).

Status: **in progress**. This is a re-implementation of the author's own
DOSBox-X integration for BizHawk (TASEmulators/BizHawk `waterbox/dosbox`,
TASEmulators/dosbox-x branch `wbx`), rebased onto the latest DOSBox-X upstream
release and re-targeted at Chimera's engine-driven waterbox pipeline. The
mechanisms that carry over, the ones that change, and the milestone plan are in
[`docs/PLAN.md`](docs/PLAN.md).

## Credits & provenance

All emulation comes from **DOSBox-X**, by Jonathan Campbell and contributors
(itself descended from DOSBox by the DOSBox Team), GPL-2.0-or-later, vendored
unmodified as the submodule [`extern/dosbox-x`](extern/dosbox-x) (pinned to a
release tag). The integration layer is this repository's own work under the
**MIT License**, with two GPL-2.0-or-later exceptions: [`patches/`](patches)
(modified DOSBox-X source) and `waterbox/dosbox-driver.cpp` (descended from the
author's BizHawk-side `bizhawk.cpp`). Built artifacts combine this code with
DOSBox-X and are therefore distributed under the GPL-2.0-or-later; MIT permits
that combination. See [`LICENSE`](LICENSE).

The prior art this re-implementation draws on, all by this repository's author:

- the BizHawk DOSBox-X waterbox port (TASEmulators/BizHawk `waterbox/dosbox`,
  its C# core `src/BizHawk.Emulation.Cores/Computers/DOS`, and the
  TASEmulators/dosbox-x `wbx` branch),
- jaffarCommon's `MemoryFileDirectory` (the in-guest writable file layer the
  hard-disk image lives in).
