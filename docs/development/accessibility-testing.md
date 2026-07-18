# Live accessibility testing

PD1-WP13 includes a bounded live AT-SPI lane around the production
`prismdrake-settingsd` and `prismdrake-shell` executables. It verifies the
toolkit-to-platform bridge separately from the display-free QML tests and the
candidate visual captures. The lane is an assistive-technology smoke test, not
a claim of complete screen-reader support.

## Covered behavior

The harness creates a disposable Xvfb display, Openbox instance, D-Bus session,
AT-SPI bus, XDG directory set, and one synthetic desktop entry. It then checks:

- the panel launcher and diagnostics controls have non-empty names,
  descriptions, push-button roles, `Press` actions, and enabled/focusable
  states;
- invoking the panel launcher's AT-SPI action opens the production launcher;
- the launcher pane, editable search field, and deterministic fixture result
  expose their expected roles and descriptions;
- focus starts in search, moves forward to the result, and returns to search
  with reverse traversal;
- Escape returns focus to the panel launcher; and
- panel Tab and Backtab traversal moves to diagnostics and back to the launcher.

This covers the platform-bridge portion of `PD-A11Y-001` through
`PD-A11Y-003`, supports the regression posture in `PD-A11Y-011` and
`PD-A11Y-012`, and exercises the launcher part of `PD-LAUNCH-005`. The shared
QML and visual lanes remain responsible for visible focus, minimum targets,
profile/fallback parity, text scale, reduced motion, and non-color cues.

The integrated PD1 demonstration extends this coverage to the production task
presentation. It verifies that the exact shell and AT-SPI owners agree, then
exercises keyboard minimization and reactivation, pointer secondary-menu entry,
AT-SPI Close, and keyboard Close against two private fixture windows. Later
EWMH observations and exact process disappearance confirm the requests; the
shell never becomes authoritative for window state.

## Build-tree command

Configure and build the two real processes, then run the focused contract and
live cases:

```bash
cmake -S . -B build/accessibility -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON
cmake --build build/accessibility --parallel 2 --target \
  prismdrake-settingsd prismdrake-shell
ctest --test-dir build/accessibility \
  -R 'AccessibilityEvidenceContractTest|LiveAtspiAccessibilityTest' \
  --output-on-failure
```

To run the strict task-action contract and live extension from the same exact
build tree:

```bash
cmake --build build/accessibility --parallel 2 --target \
  prismdrake-session prismdrake-settingsd prismdrake-shell \
  prismdrake-controlled-window-fixture
ctest --test-dir build/accessibility \
  -R 'Pd1DevelopmentDemoContractTest|Pd1DevelopmentDemonstrationTest' \
  --output-on-failure
```

This extension additionally requires `xprop`. Its live evidence is written as
`tests/integration/pd1-development-demo-evidence.json` under the selected build
directory. The file must remain private and is not a visual artifact.

The live case requires `Xvfb`, Openbox, `xdotool`, `dbus-run-session`,
`gdbus`, Python GObject introspection, and the `Atspi` 2.0 typelib. CMake
registers an explicit skipped result when this optional inspection environment
is incomplete. The reference Gentoo package is
`app-accessibility/at-spi2-core`; the project `x11` development layer supplies
the complete guest tool set.

For a product ebuild with `USE=test`, the non-optional live-lane build
dependencies are:

```text
app-accessibility/at-spi2-core[X,introspection]
dev-libs/glib
dev-python/pygobject[${PYTHON_USEDEP}]
gnome-base/gsettings-desktop-schemas
sys-apps/dbus
x11-base/xorg-server[xvfb]
x11-misc/xdotool
x11-wm/openbox
```

The ebuild must also retain `${PYTHON_DEPS}` and the existing Qt accessibility,
D-Bus, GUI, and X USE requirements. On Gentoo, `dev-libs/glib` owns `gdbus`,
while `at-spi2-core[introspection]` owns the `Atspi` typelib and
`dev-python/pygobject` supplies Python `gi`. The schema package is explicit
because it is runtime data used by the accessibility bus but is not pulled by
the `at-spi2-core` ebuild. A clean `USE=test` build is expected to satisfy this
gate, configure with `-DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON`, and run the live
case. The option fails configuration when any inspection dependency is missing,
so a package test cannot silently convert the required lane into a skip.

## Installed-artifact command

The same driver accepts installed executable paths. Run it from an exact source
revision matching the installed package:

```bash
python3 tests/accessibility/live_atspi.py \
  --dbus-run-session /usr/bin/dbus-run-session \
  --gdbus /usr/bin/gdbus \
  --openbox /usr/bin/openbox \
  --xdotool /usr/bin/xdotool \
  --xvfb /usr/bin/Xvfb \
  --settingsd /usr/bin/prismdrake-settingsd \
  --shell /usr/bin/prismdrake-shell \
  --output /absolute/private/evidence/live-atspi-evidence.json
```

The destination path must be absolute. The driver uses private temporary
directories with a mode-0700 runtime directory, bounds startup and traversal,
and performs exact-child termination with a bounded kill fallback. It fails
closed when an executable, service, application, control, role, description,
action, state, focus transition, or fixture result is missing.

## Evidence contract and privacy

[`atspi-evidence.schema.json`](../../tests/accessibility/atspi-evidence.schema.json)
defines the strict version-one output. The semantic validator additionally
requires seven ordered phases and their exact focus targets. Evidence includes
only fixed environment identifiers, the fixture profile/count, allow-listed
control metadata, and boolean states. It deliberately excludes the D-Bus and
AT-SPI addresses, display number, PIDs, X11 IDs, filesystem paths, arbitrary
desktop entries, window titles, user text, and the unrestricted accessibility
tree.

Validate the evidence-code failure paths without a display using:

```bash
python3 tests/accessibility/test_live_atspi.py
```

## Remaining boundary

This lane does not operate a screen reader, inspect natural-language speech,
test real user application names or window titles, cover a production
notification service, or establish multi-output, mixed-scale, alternate-WM,
or non-English behavior. Those claims require separate evidence. A skipped
live lane is also not a pass; release or milestone review must cite a completed
reference-environment run when the platform bridge is in scope.
