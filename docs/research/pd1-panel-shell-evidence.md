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
  `_NET_WM_STRUT_PARTIAL` publication before the window is shown; and
- one event-driven EWMH task controller that publishes complete observations
  and sends checked activate, minimize, and close requests for the exact current
  task lifetime and generation.

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

## Explicit remaining gaps

- There is no installed `prismdrake-shell` executable or live settings-snapshot
  client yet.
- Presentation adapters, the task controller, and the window host are not wired
  into one long-running shell process yet.
- X-server-loss handling is implemented as notifier disable, panel hide, and a
  queued shutdown callback, but an induced-loss test is deferred. Killing Qt's
  sole platform X server can abort `QGuiApplication` before callback dispatch;
  a deterministic test requires a broader transport seam and must not be
  simulated with a misleading pass.
- Reviewed visual baselines, mixed-scale captures, right-to-left evidence, and
  live AT-SPI inspection remain WP13 work.
- Complete dynamic runtime closure and installed-package behavior remain WP15
  work.
