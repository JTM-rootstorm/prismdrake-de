# PD1 Qt Quick and X11 evidence spike

- **Status:** Partial evidence; Gentoo VM runtime observations pending
- **Date:** 2026-07-16
- **Scope:** Experimental support for PD1-WP1 and Proposed ADR 0003
- **Architecture status:** This experiment does not accept Qt, CMake, or a
  production shell architecture.

## Evidence boundary

The removable experiment lives in
`experiments/pd1-toolkit-spike/`. It has its own CMake project, is absent from
the repository root build, has no installation rules, and can be deleted
without changing a production target. It reads the committed Prismdrake Lustre
and Prismdrake Forge token data; it does not create a production token resolver.

The C++ model owns profile selection, accessibility preferences, sample task
state, and token decoding. QML owns layout, bindings, deterministic focus
links, accessibility metadata, and brief interruptible animation. A separate
XCB adapter applies only standard dock properties. It completes checked
property requests before mapping the window, so an EWMH window manager such as
the test Openbox instance can classify the initial map as a dock. The experiment
implements no `GW_*` protocol, window-manager policy, compositor blur, scene
capture, or screenshot blur. Lustre uses ordinary alpha and its committed
opaque fallback when transparency is disabled.

This document distinguishes host build observations from the required Gentoo
VM observations. Rows marked **Pending guest observation** are commands or test
intent, not claimed results.

## Environment

### Observed host build environment

| Item | Observed value |
|---|---|
| Architecture | `x86_64` |
| CPU | AMD Ryzen 7 9800X3D 8-Core Processor, 16 logical CPUs |
| Kernel | Linux `7.1.2-gentoo-r1` |
| Compiler | GCC `15.3.0` (`c++ (Gentoo 15.3.0 p8)`) |
| CMake | `4.3.4` |
| Qt | `6.11.1` |
| XCB | `1.17.0` |
| Desktop transport | Wayland session with an Xwayland `DISPLAY=:0` |

The host was used only for configuration, compilation, display-free model
tests, and dependency inspection. The visible program was not run on the
maintainer's active desktop; X11 runtime claims remain reserved for the VM.

### Gentoo VM baseline

An SSH inspection observed `prismdrake-vm` running Linux
`6.18.38-gentoo-m10` on x86-64 and `/mnt/shared` mounted read/write using
virtiofs. At that inspection point, Qt, Xvfb, Xephyr, and X11 inspection tools
were not installed. Their eventual versions and USE configuration are
**Pending guest observation** after the reviewed PD1 package set is installed.

## Build and test commands

### Observed on the host

```bash
cmake -S experiments/pd1-toolkit-spike \
  -B /tmp/prismdrake-pd1-toolkit-host-build \
  -DBUILD_TESTING=ON
cmake --build /tmp/prismdrake-pd1-toolkit-host-build -j2
ctest --test-dir /tmp/prismdrake-pd1-toolkit-host-build --output-on-failure
python3 experiments/pd1-toolkit-spike/tests/check_source_contract.py
git diff --check -- experiments/pd1-toolkit-spike
```

Observed results:

- Configuration and compilation completed successfully.
- The `prismdrake-pd1-toolkit-spike-model` C++ test passed: 1/1 CTest
  tests, containing six focused model cases plus test setup and cleanup.
- The display-free source-contract check passed. It checks isolation cues,
  forward and reverse focus links, Escape handling, accessibility metadata,
  standard dock atoms, and the absence of `GW_*` and capture APIs.
- `git diff --check` reported no whitespace errors for the experiment.
- `qmllint` was not installed on the host, so a standalone lint result is
  **not observed**. QML ahead-of-time cache generation did complete as part of
  the build, but that is not a substitute for runtime validation.

### Planned in `prismdrake-vm`

These commands are a reproducible starting point. Every result in this section
is **Pending guest observation**; update paths if the synchronized source root
differs.

```bash
cmake -S /mnt/shared/prismdrake-de/experiments/pd1-toolkit-spike \
  -B /var/tmp/prismdrake-pd1-toolkit-spike-build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build /var/tmp/prismdrake-pd1-toolkit-spike-build --parallel 2
ctest --test-dir /var/tmp/prismdrake-pd1-toolkit-spike-build \
  --output-on-failure
```

Before recording a visible result, start the isolated X11 and D-Bus harness
documented by the PD1 VM plan, including Xvfb or Xephyr and the test window
manager. Then run the spike with the xcb QPA and software Qt Quick backend:

```bash
export DISPLAY=:99
export QT_QPA_PLATFORM=xcb
export QT_QUICK_BACKEND=software
/var/tmp/prismdrake-pd1-toolkit-spike-build/prismdrake-pd1-toolkit-spike \
  --profile lustre \
  --disable-transparency \
  --reduced-motion \
  --text-scale 1.25 \
  --exit-after-ms 3000
```

The application prints `PRISMDRAKE_SPIKE_WINDOW_ID` and
`PRISMDRAKE_SPIKE_DPR` for inspection. Use the reported ID rather than a
process-name heuristic:

```bash
xprop -id "$PRISMDRAKE_SPIKE_WINDOW_ID" \
  _NET_WM_WINDOW_TYPE _NET_WM_STRUT _NET_WM_STRUT_PARTIAL
xwininfo -id "$PRISMDRAKE_SPIKE_WINDOW_ID"
xdpyinfo -display "$DISPLAY"
xrandr --display "$DISPLAY" --query
```

## Linked modules and libraries

The experiment target explicitly links these CMake targets:

- `Qt6::Core`
- `Qt6::Gui`
- `Qt6::Qml`
- `Qt6::Quick`
- `Qt6::QuickControls2`
- `PkgConfig::XCB`

On the host, `readelf -d` observed these direct `NEEDED` entries:

```text
libGLX.so.0
libOpenGL.so.0
libQt6Core.so.6
libQt6Gui.so.6
libQt6Network.so.6
libQt6OpenGL.so.6
libQt6Qml.so.6
libQt6Quick.so.6
libQt6QuickControls2.so.6
libc.so.6
libgcc_s.so.1
libm.so.6
libstdc++.so.6
libxcb.so.1
```

`ldd` resolved 59 unique runtime objects on that host, including the dynamic
loader and vDSO. The complete observed set was:

```text
/lib64/ld-linux-x86-64.so.2 libEGL.so.1 libGLX.so.0 libGLdispatch.so.0
libOpenGL.so.0 libQt6Core.so.6 libQt6DBus.so.6 libQt6Gui.so.6
libQt6Network.so.6 libQt6OpenGL.so.6 libQt6Qml.so.6 libQt6QmlMeta.so.6
libQt6QmlModels.so.6 libQt6QmlWorkerScript.so.6 libQt6Quick.so.6
libQt6QuickControls2.so.6 libQt6QuickTemplates2.so.6 libX11.so.6
libXau.so.6 libXdmcp.so.6 libb2.so.1 libblkid.so.1 libbrotlicommon.so.1
libbrotlidec.so.1 libbz2.so.1 libc.so.6 libcrypto.so.3 libdbus-1.so.3
libdouble-conversion.so.3 libexpat.so.1 libffi.so.8 libfontconfig.so.1
libfreetype.so.6 libgcc_s.so.1 libgio-2.0.so.0 libglib-2.0.so.0
libgmodule-2.0.so.0 libgobject-2.0.so.0 libgomp.so.1 libgraphite2.so.3
libharfbuzz.so.0 libicudata.so.78 libicui18n.so.78 libicuuc.so.78
libm.so.6 libmd4c.so.0 libmount.so.1 libpcre2-16.so.0 libpcre2-8.so.0
libpng16.so.16 libproxy.so.1 libpxbackend-1.0.so libstdc++.so.6
libsystemd.so.0 libxcb.so.1 libxkbcommon.so.0 libz.so.1 libzstd.so.1
linux-vdso.so.1
```

This host list is not a minimum runtime set and must not be substituted for the
Gentoo package graph. Direct and transitive guest libraries, package atoms,
licenses, USE flags, installed sizes, and optionality are all **Pending guest
observation**. No dependency size is claimed here.

## Functional evidence matrix

| Area | Current result | Guest evidence still required |
|---|---|---|
| Lustre and Forge tokens | **Observed in model tests:** both committed profiles load; required evidence fields are decoded from `#RRGGBBAA` explicitly. | Visually inspect both profiles under X11. |
| Profile switch | **Observed in model tests:** profile changes while text scale, reduced motion, and disabled transparency remain independent. | Exercise control with pointer and keyboard. |
| Keyboard order | **Implemented, not runtime-observed:** explicit Tab and reverse-Tab links cover launcher, tasks, profile, scale, motion, transparency, and launcher close controls. | Record the actual focus sequence and visible focus at each step. |
| Enter and Space | **Implemented through Qt Quick Button controls, not runtime-observed.** | Activate every control using both keys where applicable. |
| Escape | **Implemented, not runtime-observed:** enabled while the launcher sample is open. | Confirm dismissal and focus return. |
| Accessible metadata | **Implemented, not AT-SPI-observed:** controls expose name, description, role, focus, checked state, and text status. | Inspect the actual AT-SPI tree and record any missing role, state, or action. |
| Text scaling | **Observed in model tests:** bounded from 1.0 to 2.0 and independent of profile. | Inspect 100%, 125%, and 150% for clipping and focus geometry. |
| Device pixel ratio | **Implemented, not runtime-observed:** the QML view and stdout expose the Qt-reported DPR. | Record X11 output geometry, Qt DPR, and at least one scale variation feasible in the harness. |
| Reduced motion | **Observed in model tests:** presentation duration resolves to zero. | Confirm transitions are immediate and input remains responsive. |
| Disabled transparency | **Observed in model tests:** Lustre resolves to its committed opaque `#202A42` fallback with full alpha. | Confirm the rendered window is opaque without blur or capture. |
| Deterministic rendering | **Build support only:** runtime can select `QT_QUICK_BACKEND=software`. | Record backend diagnostics and an own-window screenshot made by an external test tool. |
| X11 dock properties | **Compiled, not runtime-observed:** isolated XCB adapter completes checked writes for `_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STRUT`, and `_NET_WM_STRUT_PARTIAL` before C++ maps the window. | Verify exact values and initial Openbox classification with `xprop` under Xvfb or Xephyr. |
| Process restart | **Bounded-exit option implemented, not runtime-observed.** | Run at least two clean start/exit cycles and record exit status and logs. |

## Rendering and screenshots

No screenshot result is currently claimed. The experiment contains no capture
API. Guest evidence may use an external test tool to capture only the spike
window after its X11 ID is known. Record output size, profile, text scale,
transparency state, motion state, DPR, Qt version, renderer, and test font with
each artifact. Do not use screenshots to implement or simulate blur.

## Accessibility procedure still required

The guest run must inspect the live AT-SPI tree rather than infer accessibility
from QML properties. Record the isolated D-Bus session setup, AT-SPI inspection
tool and version, exposed application/window/control roles, names, checked or
selected state, focus changes, and actions. If the surface is absent or any
required property is missing, record that as a failure rather than converting
the implementation intent into a pass.

Keyboard review must additionally confirm deterministic forward and reverse
focus order, visible focus in both profiles, Enter and Space activation,
Escape dismissal, no keyboard trap, minimum target geometry, and no clipping at
the tested scales.

## Known failures and limitations

- Required guest runtime, X11, screenshot, scale, restart, and AT-SPI evidence
  is not yet observed.
- Host `qmllint` was unavailable.
- The spike uses one bottom-edge, primary-screen demonstration and does not
  establish a production multi-output policy.
- The X11 adapter demonstrates standard property publication only. It neither
  owns work-area policy nor proves behavior across window managers.
- The token reader is intentionally narrow and does not implement merging,
  immutable generations, general schemas, migrations, or production parsing.
- Standard Qt Quick controls still require live AT-SPI verification; metadata
  in source is necessary evidence, not sufficient evidence.
- The host dependency closure is environment-specific and says nothing yet
  about the reviewed Gentoo guest graph, licenses, USE flags, or installed size.

## Recommendation and ADR 0003 impact

The source and host build evidence support continuing the Qt Quick candidate to
the required VM run: the model/view split, shared profile tokens, fallback
states, focused tests, and isolated X11 boundary are feasible without touching
production targets. The evidence is not sufficient to accept or reject
ADR 0003 because its decisive X11, AT-SPI, scaling, deterministic rendering,
restart, and Gentoo dependency questions remain unobserved.

Do not change ADR 0003 from Proposed and do not re-score the toolkit matrix yet.
After the guest matrix is filled from observed results, re-score only criteria
whose evidence materially differs from the PD0 qualitative evaluation, then
request an explicit maintainer decision.
