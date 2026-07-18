# Multi-output shell-surface placement

This Accepted PD2 design freezes shell placement under
[ADR 0009](../adr/0009-pd2-state-and-control-contracts.md). It defines a
standards-first X11 baseline and does not accept or implement a Glasswyrm-native
shell-role protocol.

## Authority and typed observations

The active window manager and X11 server remain authoritative for output,
focus, stacking, work-area, and global-binding policy. `prismdrake-shell` owns
only the placement of its own desktop, panel, launcher, notification,
notification-center, quick-settings, and logout-confirmation surfaces.

The shell registry consumes immutable topology observations containing:

- a nonzero observation generation;
- a bounded stable RandR output name;
- enabled, connected, and primary state;
- logical geometry with checked signed coordinates and positive dimensions;
- rotation and transform state where available;
- Qt 6.11 `QScreen` geometry and finite positive device pixel ratio; and
- an explicit reconciliation status.

Output names and geometry are untrusted. Qt screens are reconciled with
validated RandR outputs by stable name and geometry. Ambiguous or inconsistent
matches remain unavailable with a redacted diagnostic; the shell never guesses
from array order, physical millimetres, process names, or a compositor name.
Consumers receive one complete topology snapshot and do not combine fields from
different observations.

The surface registry uses typed kinds: `desktop`, `panel`, `launcher`,
`transient_notifications`, `notification_center`, `quick_settings`, and
`logout_confirmation`. A placement binds one surface instance to one active
output identity and logical geometry generation. Output removal invalidates
that binding before a replacement placement is published.

## Desktop policy

- Create exactly one complete non-focusable desktop surface for every active
  output after one valid topology observation.
- Use the same configured wallpaper source and scaling mode on every output in
  PD2. Per-output wallpaper selection is Deferred.
- Destroy only the matching desktop when an output is removed.
- Do not capture input intended for applications or window-manager operations.
- Keep desktop icons disabled in PD2; the desktop owns no file-operation policy.
- Missing, invalid, oversized, or undecodable wallpaper data uses the configured
  solid background without affecting other outputs.

This satisfies `PD-DESKTOP-001` through `PD-DESKTOP-003`, `PD-DESKTOP-005`,
and `PD-DESKTOP-006` without moving composition into Prismdrake.

## Panel policy

Configuration version 2 selects one of these policies:

| Policy | Placement |
|---|---|
| `primary` | one panel on the current active primary output |
| `all` | one independently focusable panel on every active output |
| `named` | panels on configured active output names; current primary fallback when none match |

Names in the `named` list are ordered and unique. Disconnected names remain
configuration, not active placements. A primary-output change migrates the
`primary` panel without restarting the shell. Under `all`, output add and remove
only create or destroy the matching instance. Under `named`, hotplug reevaluates
the complete policy atomically.

PD2 fully supports bottom-edge panels. Any other accepted configuration value
that lacks complete layout support produces one actionable diagnostic and a
safe bottom-edge fallback. Panels reserve work area using the standards X11
path. Every panel may show the complete task set; per-output task filtering is
Deferred. Pins are shared, while keyboard focus and invocation origin are
instance-local.

These rules address `PD-PANEL-001`, `PD-PANEL-004`, `PD-PANEL-005`, and
`PD-OUTPUT-001` through `PD-OUTPUT-004`.

## Transient surface policy

### Launcher

- Panel invocation opens on the invoking panel's output.
- `Shell1.ToggleLauncher("")` resolves the current primary output.
- Only one launcher is visible globally.
- Invocation on another output transfers the existing launcher coherently.
- Removing its output transfers it to the new primary output, or dismisses it
  when no active output remains.

### Transient notifications

- The stack uses the current primary output.
- Cards stack within bounded geometry and never rely on thumbnails or native
  compositor capability for placement.
- A primary change or output removal moves the complete stack to the current
  primary output.
- With no active output, notifyd state remains authoritative and presentation is
  deferred until a valid output exists.

### Notification center and quick settings

- Panel invocation opens on that panel's output.
- Shell-control invocation with an empty output name resolves current primary.
- Only one instance of each is visible globally.
- Invocation on another output transfers the existing instance and focus
  origin.
- Output removal transfers the surface to current primary or dismisses it when
  no output remains.

### Logout confirmation

Logout confirmation is modal and may have only one active presentation for the
session transaction. It appears on the current primary output. A primary change
or output removal transfers the same transaction presentation; it never creates
a second session transaction.

## Focus routing

One shell-owned focus router orders transient presentation:

1. modal logout confirmation;
2. task or launcher action menu;
3. quick settings or notification center;
4. launcher;
5. transient notification action surface;
6. panel.

Only one shell-owned popup family is active per output unless it is an explicit
child menu. Opening a conflicting family dismisses it or transfers focus before
the new surface becomes active. Escape returns focus to the invoking control
when that control and output still exist. Otherwise the router uses an
equivalent primary-output control or returns focus through the window-manager
path.

Tab and Backtab order is deterministic. Stable C++ presentation identity, not
XIDs or JavaScript numeric values, survives model replacement. Shell1 requests
deliberate focus through the accepted window-manager path; Prismdrake does not
override WM focus policy or install global keyboard grabs.

These rules preserve `PD-INPUT-001` through `PD-INPUT-004` and make keyboard
recovery part of output-hotplug correctness.

## Scale and coordinate conversion

Qt 6.11 logical geometry and device pixel ratio are the Qt-surface sizing
inputs. Pixel conversion is explicit, checked, and performed relative to the
matched output; global logical coordinates are never multiplied by an unrelated
screen's ratio. Physical millimetres do not determine scale.

PD2 live acceptance requires two same-scale outputs. Display-free tests cover
different device pixel ratios, negative output origins, add/remove, primary
change, and logical/native coordinate conversion. Full physical mixed-DPI
hardening remains Deferred; this limitation does not permit blurry text or
inconsistent input geometry in cases PD2 claims to support.

## Failure and fallback

- A malformed topology observation is rejected without disturbing the last
  complete valid placement generation.
- Losing one output destroys or transfers only surfaces bound to it.
- Losing settings invalidates policy-dependent surfaces but preserves current
  safe placement until a complete generation is available.
- Shell restart reconstructs placement from current topology and configuration.
- Missing native Glasswyrm capability changes no placement rule; the standards
  path remains authoritative.
- Missing blur selects alpha or opaque material and never invokes shell-side
  capture.

Relevant requirements: `PD-COMP-003`, `PD-REL-001`, `PD-REL-005`,
`PD-REL-006`, `PD-PERF-001`, `PD-PERF-004`, `PD-GW-001` through `PD-GW-010`,
and `PD-API-001` through `PD-API-005`.
