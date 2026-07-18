# PD1 development panel-shell evidence

## Status and scope

This report records measured evidence for the Experimental PD1 development
panel. It is not evidence of a usable or production-ready desktop shell.

The implemented slice contains:

- one immutable launcher presentation over matching catalog and search
  snapshots;
- one shared Lustre/Forge panel component over a complete theme generation and
  authoritative task presentation;
- deterministic keyboard traversal, focus recovery, explicit active, inactive,
  minimized, urgent, and modal cues, and typed activation intents;
- plain-text rendering of untrusted task metadata;
- one Qt/X11 host for the documented bottom-edge primary-output policy;
- checked standard `_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STRUT`, and
  `_NET_WM_STRUT_PARTIAL` publication before the window is shown;
- one event-driven EWMH task controller that publishes complete observations
  and sends checked activate, minimize, and close requests for the exact current
  task lifetime and generation;
- one asynchronous settings client that invalidates owner epochs and publishes
  only strict, complete, canonical typed settings/theme snapshots; and
- one `prismdrake-shell` composition root that creates panel and launcher views
  only from a complete settings epoch and connects them to the existing
  launcher, task, window-host, and theme boundaries.

The host reuses Prismdrake's existing X11 connection, RandR topology, output
selection, dock-publication, and root-event boundaries. It does not take focus,
stacking, workspace, output, or compositor authority from the window manager.

## Requirements covered

This slice supplies PD1 evidence for `PD-PANEL-001`, `PD-PANEL-003`,
`PD-PANEL-004`, and `PD-PANEL-005`; `PD-THEME-001` through `PD-THEME-003` and
`PD-THEME-007` through `PD-THEME-009`; and `PD-A11Y-001` through
`PD-A11Y-004` plus `PD-A11Y-006` through `PD-A11Y-009`. It preserves the
desktop-entry no-shell boundary in `PD-SEC-002` by emitting a typed launch
intent instead of expanding or executing `Exec` in the presentation layer.

This prototype does not complete the full 1.0 `PD-PANEL-002` contract. Pinned
applications, grouping, and production context actions are outside this bounded
panel slice. Wallpaper contrast, live assistive-technology inspection, and the
remaining visual matrix also remain explicitly open below.

## Host validation

The host Gentoo development environment uses Qt 6.11.1. The reviewed slice
passed:

- warnings-as-errors builds for the launcher, panel, and window targets;
- 9 launcher-presentation tests;
- 9 panel Quick Test cases with no QML warnings;
- 8 display-free panel-window controller tests;
- 7 display-free task-controller tests plus one deterministic no-Xvfb skip;
- 4 strict runtime-snapshot parser tests and 5 isolated-bus client cases;
- 4 display-free shell-runtime lifecycle tests, 2 inherited-environment tests,
  and 2 Qt owner-thread termination-signal bridge tests;
- panel and notification `qmllint` targets;
- the repository C++ format target;
- `make validate`, including 39 negative contract fixtures; and
- `git diff --check`.

The two panel-window host registrations skip deterministically on this host
because Xvfb is unavailable. They are exercised in the reference VM instead of
being reported as host passes.

## Gentoo VM validation

The exact staged source is archived as
`prismdrake-wp9-window-staged-v3.tar.gz` with SHA-256
`e46c36dcc1384da0cb4bd733301bef768abf2cddc323d14bde4bbe9c06120cf2`.
Its validation log is `prismdrake-wp9-window-validation-v3.log` with SHA-256
`e8ca40949af48d6589a44e446356a2c17ede8f386c417c393fe6671ebf54228e`.

On the Gentoo reference VM:

| Toolchain and lane | Result |
|---|---:|
| GCC 15.3, focused `PanelWindow` CTest selection | 10/10 passed |
| Clang 22.1.8, focused `PanelWindow` CTest selection | 10/10 passed |
| Bare Xvfb Qt/X11 host lane | passed under both toolchains |
| Xvfb plus Openbox Qt/X11 host lane | passed under both toolchains |
| Existing Openbox `_NET_WORKAREA` integration case | 1/1 passed |

The host lanes verify the exact dock type and strut arrays, bottom-edge
geometry, absence of startup focus eligibility, deliberate keyboard access and
release, and a validated runtime height change. The existing Openbox case
independently verifies that the window manager applies the published reservation
to `_NET_WORKAREA`.

The isolated Xvfb harness also passed five consecutive bare starts and five
consecutive Openbox starts using the server's built-in font path. Observed
startup durations were 0.059 to 0.102 seconds. This avoids dependency on a cold
distribution font catalogue while retaining the bounded startup timeout.

The integrated `prismdrake-shell` composition also built on the Gentoo VM and
passed all 4 runtime-state tests. A live Xvfb/Openbox smoke mapped the panel,
transferred focus into the launcher and back on Escape, removed the complete
presentation epoch when settingsd lost its D-Bus name, and remapped it after a
new owner published generation 1. Terminating the complete X server remains a
Qt XCB QPA process-fatal boundary with status 1 before application callbacks;
the session supervisor therefore treats that process exit as restartable rather
than claiming in-process X-transport recovery.

`ldd` on the validated Gentoo shell target resolved the selected `libbasu`,
Qt 6 Core/GUI/QML/Quick/Quick Controls libraries, `libxcb`, `libxcb-randr`, and
the system transitive graphics, font, C++ runtime, D-Bus, and Qt support
libraries without a missing entry. This measures the built executable boundary;
it does not substitute for the later emerged-package content and dependency
closure audit.

## Accessibility and fallback behavior

The component uses the same QML tree for Lustre and Forge. Tests cover complete
profile-generation replacement, high contrast, reduced motion, disabled
transparency, the opaque material fallback, minimum targets, accessible names,
roles, descriptions, checked state, deterministic Tab and Backtab exits, task
removal, and authoritative task reorder. Color is not the only active or urgent
cue.

No animation is required by this slice, so reduced motion introduces no delayed
input. No compositor blur is executed. Missing blur or disabled transparency is
represented by the complete settings generation's fallback material.

## Security and failure behavior

QML receives no desktop-entry path, `Exec` value, raw catalog index, X11 window
identifier, task lifetime, or numeric model generation. Launcher and task
actions are typed C++ intents from the current coherent publication. Displayed
task strings use `Text.PlainText`.

Malformed, stale, conflicting, reentrant, cross-thread, and oversized model
publications retain the previous coherent presentation. The window host stages
topology and height changes, publishes checked dock state before geometry, and
commits only after the matching X11 state succeeds. A failed update attempts one
bounded rollback to the previous applied placement; if coherence cannot be
restored, the host hides the panel and clears its applied state.

The task controller has one sole `RootEventStream` on its own X11 connection,
coalesces each bounded event batch into at most one refresh, invalidates observed
XID incarnations before rebuilding state, and disables cached request
capabilities until a complete observation and fresh WM capability check both
succeed. Pending requests are capped at 64, deduplicated by lifetime and action,
expire after eight newer observations, and are confirmed or refused only from
authoritative task snapshots. It never writes WM-owned state directly.

The settings client installs D-Bus matches before querying the current owner,
drives sd-bus through bounded Qt socket/monotonic-timeout dispatch, clears its
cache on every owner gap, and refetches a complete snapshot after acquisition or
generation hints. Snapshot replies are limited to 1 MiB, reconstructed into
typed settings/theme state, and accepted only when the outer generation,
embedded generation, closed schema, and canonical serialized bytes agree.
Malformed, duplicate, deep, oversized, noncanonical, stale, conflicting, and
unknown content retains the prior complete snapshot. No transport JSON is
exposed to QML.

## Explicit remaining gaps

- The built `prismdrake-shell` executable is not installed by the Experimental
  Gentoo package yet, and its complete installed package closure is unmeasured.
- No Accepted WM/session shortcut contract currently provides global launcher
  entry; the wired launcher remains reachable through the panel surface.
- X-server-loss handling is implemented as notifier disable, panel hide, and a
  queued shutdown callback, but an induced-loss test is deferred. Killing Qt's
  sole platform X server can abort `QGuiApplication` before callback dispatch;
  a deterministic test requires a broader transport seam and must not be
  simulated with a misleading pass.
- Reviewed visual baselines, mixed-scale captures, right-to-left evidence, and
  live AT-SPI inspection remain WP13 work.
- Complete dynamic runtime closure and installed-package behavior remain WP15
  work.
