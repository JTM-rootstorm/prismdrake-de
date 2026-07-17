# PD1 Qt Quick and X11 evidence spike

- **Status:** Observed single-output Gentoo VM evidence used for an Accepted decision
- **Date:** 2026-07-16
- **Scope:** Experimental support for PD1-WP1 and Accepted ADR 0003
- **Architecture status:** The experiment remains removable and does not itself
  constitute production shell architecture.

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

This document distinguishes host build observations from live Gentoo VM
results. Multi-output behavior remains explicitly unproven.

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
virtiofs. The completed evidence environment used Qt 6.11.1, Xorg 21.1.24,
GCC 15.3.0, CMake 4.3.3-r1, Ninja 1.13.2-r1, Xvfb, Openbox, D-Bus, and
AT-SPI. The final VM verifier reported zero failures and zero warnings. The
package closure is recorded in
[PD1 Gentoo dependency evidence](pd1-gentoo-dependency-evidence.md).

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
- Qt 6 `qmllint` completed without findings after the model was exposed through
  an engine initial property and every QML model reference was qualified.

### Observed in `prismdrake-vm`

The committed source was configured, compiled, and tested as the ordinary
`prismdrake` guest user:

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

Configuration, compilation, the six-case model test, and `qmllint` all passed.
The reusable harness then started disposable Xvfb, Openbox, D-Bus, and AT-SPI
sessions with the xcb QPA and software Qt Quick backend:

```bash
experiments/pd1-toolkit-spike/tests/run_guest_evidence.sh \
  /var/tmp/prismdrake-pd1-toolkit-spike-build \
  /mnt/shared/pd1-toolkit-evidence forge 1.0 forge-100
PRISMDRAKE_SPIKE_DISPLAY=100 \
  experiments/pd1-toolkit-spike/tests/run_guest_evidence.sh \
  /var/tmp/prismdrake-pd1-toolkit-spike-build \
  /mnt/shared/pd1-toolkit-evidence lustre 1.25 lustre-125
PRISMDRAKE_SPIKE_DISPLAY=101 \
  experiments/pd1-toolkit-spike/tests/run_guest_evidence.sh \
  /var/tmp/prismdrake-pd1-toolkit-spike-build \
  /mnt/shared/pd1-toolkit-evidence forge 1.5 forge-150
```

All three scenarios passed. The harness resolves the exact titled window,
records its X11 ID, and inspects the live window before shutdown. The observed
properties used the scenario's actual height of 280, 302, or 324 pixels:

```bash
_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DOCK
_NET_WM_STRUT(CARDINAL) = 0, 0, 0, <height>
_NET_WM_STRUT_PARTIAL(CARDINAL) = 0, 0, 0, <height>, 0, 0, 0, 0, 0, 0, 0, 959
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

The VM target had the same 14 direct entries. Its `ldd` output contained 46
resolved lines. The host resolved 59 unique runtime objects, including the
dynamic loader and vDSO; its complete observed set was:

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

This host list is not a minimum runtime set and is not substituted for the
observed Gentoo package graph.

## Functional evidence matrix

| Area | Observed result | Remaining boundary |
|---|---|---|
| Lustre and Forge | Both profiles rendered under Xvfb and were visually inspected from own-window captures. | Production visual approval is not implied. |
| Profile state | Unit tests confirmed profile switching preserves text scale, reduced motion, and transparency overrides. | Live profile-button and pointer activation remain manual-review items. |
| Keyboard and launcher | Forward Tab, reverse Tab, Space activation, Escape dismissal, and focus return passed in all scenarios. Focus rings were visible. | Enter activation and the complete control cycle remain manual-review items. |
| Accessible metadata | Live AT-SPI asserted unique names, button roles, descriptions, Press actions, focusable/enabled state, exactly one checked task, and focus transitions. | A production screen-reader workflow is broader than this smoke test. |
| Text scaling | 100%, 125%, and 150% rendered at 960x280, 960x302, and 960x324. The expanded 150% launcher capture showed no clipping. | Mixed toolkit scale and multiple outputs remain untested. |
| Reduced motion | The 125% Lustre scenario forced duration zero; input and focus checks passed. | No frame-timing claim is made. |
| Disabled transparency | Unit tests require both panel and elevated materials to be fully opaque; the Lustre capture showed the opaque fallback. | This is ordinary alpha, not compositor blur evidence. |
| Deterministic rendering | All runs forced the software Qt Quick backend and captured only the known X11 window with `xwd`. | Font/rendering stability across guest updates still needs baseline policy. |
| X11 dock properties | Openbox observed the initial map as `_NET_WM_WINDOW_TYPE_DOCK`; strut and partial-strut values matched exact window geometry. | Other WMs and real multi-output RandR topologies remain untested. |
| Process restart | More than two independent start, inspect, input, capture, and clean-stop cycles passed. | Supervisor/backoff behavior is outside this isolated experiment. |

## Rendering and screenshots

The experiment contains no capture API. The external harness captured only the
known spike X11 window as raw XWD, then the host converted local copies to PNG
for visual inspection. Forge 100%, Lustre 125% opaque/reduced-motion, Forge
150%, and Forge 150% with the launcher expanded were reviewed. The artifacts
remain in the untracked shared evidence directory and are not visual baselines.

## Accessibility procedure

`inspect_atspi.py` performs the live assertions inside the same isolated D-Bus
session as the application. It fails when the application, a required control,
role, description, action, checked task state, or expected focus state is
missing. The harness records JSON trees before input, after Tab, after reverse
Tab, with the launcher open, and after Escape dismissal.

## Known failures and limitations

- The spike uses one bottom-edge, primary-screen demonstration and does not
  establish a production multi-output policy.
- The X11 adapter demonstrates standard property publication only. It neither
  owns work-area policy nor proves behavior across window managers.
- The token reader is intentionally narrow and does not implement merging,
  immutable generations, general schemas, migrations, or production parsing.
- The token reader remains experimental. It needs hostile-input size, numeric
  range, and non-finite-value hardening before reuse in production.
- The VM result does not cover a real GPU, multiple RandR outputs, mixed DPI,
  another window manager, a complete screen-reader workflow, or Enter on every
  control.

## Recommendation and ADR 0003 impact

The guest evidence supported the maintainer's 2026-07-16 acceptance of ADR
0003. The model/view split, profile tokens, accessibility metadata, opaque and
reduced-motion fallbacks, standard X11 boundary, and isolated test harness all
worked without a production target.

No qualitative score changed materially, so the toolkit matrix is not
numerically re-scored. Acceptance authorizes the Qt 6 Quick visible-shell
direction; it does not convert the spike into production code or establish the
still-unobserved multi-output, mixed-DPI, real-GPU, alternate-WM, and complete
screen-reader behavior. Those limitations remain PD1 validation work.
