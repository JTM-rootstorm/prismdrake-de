# PD2 core-shell implementation status

This document maps the current source implementation to the Accepted PD2
contracts. It describes code that exists in the repository; it is not an
installed-reference result, a stable-interface promise, a production-readiness
claim, or the PD2 completion record. Final outcomes belong in the milestone
tracker after the Gentoo lifecycle and installed demonstrations pass.

All Qt-bound surfaces require Qt 6.11 or newer. All Prismdrake-native D-Bus,
storage, C++, QML, and evidence formats remain Experimental.

## Status vocabulary

| Status | Meaning in this document |
|---|---|
| Implemented | Source, build registration, and focused tests exist for the stated behavior; this does not assert the final installed demonstration passed. |
| Experimental | Implemented or accepted for PD2, but compatibility and production behavior are not stable. |
| Deferred | Intentionally assigned to a later milestone or a separate accepted design. |
| Unsupported | Outside the PD2 contract and unavailable in ordinary installed behavior. |

## Implemented core

| Area | Current source behavior | Requirements |
|---|---|---|
| Outputs and surfaces | One registry reconciles bounded RandR and Qt screen observations, creates one desktop per active output, applies the configured panel-output policy, and transfers or dismisses global transient surfaces after topology changes. | `PD-OUTPUT-001` through `PD-OUTPUT-004`, `PD-PANEL-001`, `PD-DESKTOP-001` |
| Panel and tasks | Shared panel QML presents ordered pins and running tasks, deterministic `always`, `never`, and `when_full` grouping, explicit active/minimized/urgent/modal state, group navigation, launch, activate, minimize, close, and pin actions. Window requests remain checked EWMH requests to the active WM. | `PD-PANEL-001` through `PD-PANEL-005`, `PD-PANEL-008`, `PD-WIN-001` through `PD-WIN-007` |
| Launcher | The local XDG application catalog is monitored without polling, publishes complete generations, supports bounded cancellable search, desktop actions, typed D-Bus activation, direct execution without an implicit shell where allowed, icons with fallback, pins, and Prismdrake destinations. | `PD-LAUNCH-001` through `PD-LAUNCH-005`, `PD-LAUNCH-007`, `PD-LAUNCH-008`, `PD-INT-007`, `PD-SEC-002`, `PD-PERF-006` |
| Desktop and wallpaper | A non-focusable standard X11 desktop surface is hosted per active output. One bounded decoded wallpaper snapshot is laid out in the configured mode on every output; invalid or missing images use the configured solid color. | `PD-DESKTOP-001` through `PD-DESKTOP-003`, `PD-DESKTOP-005`, `PD-DESKTOP-006` |
| Notifications | Toolkit-neutral notifyd owns freedesktop Notifications 1.3, replacement, timeout, sender/action lifecycle, disabled/session/private persistent history, effective do-not-disturb state, and complete Experimental notification-center snapshots. The shell mirrors those snapshots into transient and history surfaces. | `PD-NOTIFY-001` through `PD-NOTIFY-007`, `PD-SEC-007`, `PD-SEC-011`, `PD-SEC-012` |
| Quick settings | The installed surface exposes only Prismdrake-owned operations: active profile and runtime profile switch, effective do-not-disturb, notification-center opening, logout request, and bounded status text. Unavailable adapters remain visibly disabled. | `PD-QUICK-001` through `PD-QUICK-006` |
| Control and session | Experimental Shell1 exposes fixed presentation commands; Session1 owns one same-user logout transaction. The init-neutral supervisor starts settingsd, notifyd, and shell in dependency order and stops only those exact children in reverse order after confirmation. | `PD-SESSION-001` through `PD-SESSION-006`, logout-only `PD-POWER-001`, `PD-API-001` through `PD-API-005` |
| Configuration and private state | Settingsd accepts source configuration versions 1 and 2, normalizes to one complete runtime snapshot version 2, and publishes atomic generations. Pin state, notification state, and notification history use separate strict version-one schemas and owners. | `PD-CONFIG-001` through `PD-CONFIG-010`, `PD-SEC-001`, `PD-REL-008` |

The authoritative details remain in
[state ownership](pd2-state-ownership.md),
[configuration](configuration.md),
[application identity](application-identity.md),
[notification service](notification-service.md),
[surface placement](surface-placement.md), and
[shell and session control](shell-control.md). This status summary does not
replace those contracts.

## Panel and grouping behavior

Pins are ordered desktop-file identifiers, not processes, paths, window IDs, or
commands. A pinned application with no running window is a launch target. A
single running member activates or minimizes according to the authoritative
task observation. Multiple members open an accessible group list, and close is
always an explicit action. Unknown applications retain per-window operability
instead of collapsing into one guessed application. Grouping changes preserve
typed presentation identities and never make the shell authoritative for
focus, stacking, minimization, or lifecycle.

This behavior implements the PD2 portions of `PD-PANEL-002` through
`PD-PANEL-005`, `PD-WIN-003` through `PD-WIN-007`, `PD-INPUT-001`, and
`PD-A11Y-001` through `PD-A11Y-009`.

## Launcher behavior and privacy

Launcher search is local, bounded, deterministic, incrementally replaceable,
and does not persist queries. Application-root events are coalesced into a
complete catalog refresh; deletion, recreation, or watch exhaustion requests a
bounded rescan instead of unbounded polling. Desktop-entry arguments are parsed
through the closed execution contract and are never passed to an implicit
shell. Terminal-only entries remain unavailable when no terminal policy exists.

PD2 has no recent-file, online-search, package-search, or document-index
provider. It does not store launcher history. Those limits implement the
privacy boundary in `PD-LAUNCH-006`, `PD-SEC-002`, `PD-SEC-008`, and
`PD-PERF-006`.

## Desktop and wallpaper behavior

The desktop surface is presentation only. It does not accept focus, appear in
the task strip, provide icons, own root-window input, or implement file
operations. The active WM remains responsible for stacking and ordinary root
behavior. PD2 uses the same wallpaper source, mode, and background fallback on
every active output. The loader bounds the source file, image dimensions,
decoded allocation, and stale completion before publishing one immutable
generation.

Per-output wallpaper selection and desktop icons are Deferred. Desktop file
operations are Unsupported in PD2. This boundary implements
`PD-DESKTOP-001` through `PD-DESKTOP-006` without claiming `PD-DESKTOP-004`.

## Notification history behavior

Notifyd is the only authority for live IDs, history record IDs, sender action
liveness, retention, history generations, and the effective do-not-disturb
override. The shell is a replaceable client. Persistent history is opt-in,
private, bounded, atomic, and excludes image pixels, live sender identity, and
transient records. Session history never writes to disk; disabled history
retains no records. Malformed persistent input is retained for diagnosis while
the live service starts with empty history. Clearing history is a complete
atomic mutation.

Normal and low-urgency notifications do not create a transient while effective
do-not-disturb is enabled, although eligible records still follow configured
history policy. Critical notifications bypass do-not-disturb. Actions from a
dead sender are disabled and never become a desktop-entry or command fallback.
These rules implement `PD-NOTIFY-003` through `PD-NOTIFY-007`, `PD-SEC-007`,
and `PD-REL-004`.

## Quick-settings limits

The ordinary installed surface contains no audio, networking, Bluetooth,
battery, brightness, power, package, arbitrary D-Bus, or shell-command
adapter. Test fixtures may model unavailable external adapters only to verify
layout and failure behavior; they are not installed functionality. A profile
request completes only after a correlated complete settings generation, and a
do-not-disturb request completes only after a correlated notification-state
generation. Logout remains a Session1 request rather than a shell-owned action.

Reboot, shutdown, suspend, hibernate, secure locking, and privileged operations
are Unsupported in PD2. Real external-service adapters are Deferred. This is
the implementation boundary for `PD-QUICK-001` through `PD-QUICK-006` and the
logout-only scope of `PD-POWER-001`.

## Accessibility and localization posture

Panel, launcher, grouped tasks, notification cards and center, quick settings,
and logout confirmation expose deterministic keyboard routes and typed
accessible names, roles, states, descriptions, and actions. Shared theme state
provides high contrast, reduced motion, disabled transparency, text scaling,
focus emphasis, and minimum targets. Pseudo-locale catalogs are packaged for
expansion and right-to-left exercise. These are implemented test surfaces, not
a claim of complete screen-reader or language support.

The live and display-free procedures are documented in
[accessibility testing](../development/accessibility-testing.md). They address
the PD2 implementation portions of `PD-A11Y-001` through `PD-A11Y-012` and
`PD-I18N-001` through `PD-I18N-007`.

## Exit-evidence status

| Evidence area | Current status |
|---|---|
| Source and contract coverage | Implemented. Focused suites and strict negative fixtures cover the integrated behavior and evidence formats. |
| Installed demonstration | Implemented harness only. Its 48 ordered checks require one source-bound Gentoo run before they become milestone evidence. |
| Accessibility and localization | Display-free and exact-observer machinery is implemented. The required live AT-SPI matrix remains pending; a full screen-reader workflow is not claimed. |
| Performance | Source-bound collectors and exact runtime endpoints are implemented. Reference measurements and budget comparison remain pending. |
| Portage lifecycle | Exact build provenance, installed ownership, harness manifest, unmerge preservation, and reinstall contracts are implemented. The real lifecycle record remains pending. |

GitHub Actions is not an exit-gate substitute or requirement. The Gentoo
reference matrix and canonical local validation commands remain authoritative.

## Deferred and unsupported boundaries

- StatusNotifierItem and XEmbed tray integration are Deferred.
- Full physical mixed-DPI hardening, alternate-WM qualification, real-GPU
  qualification, and production visual baselines are Deferred.
- Production Lustre and Forge polish, toolkit style packages, portals,
  control-center pages, production icons, and final animation behavior belong
  to PD3 or later.
- Per-output task filtering and per-output wallpaper selection are Deferred.
- Wayland, shell-owned global shortcuts, compositor blur execution,
  shell-side capture, thumbnails, and every Glasswyrm-native `GW_*` protocol
  are Unsupported in PD2.
- Stable D-Bus, storage, C++, QML, package, or evidence compatibility is
  Unsupported until separately accepted.
- The repository remains Experimental and does not claim a secure lock screen,
  privileged system action, production-ready appearance, or complete desktop
  compatibility.

These boundaries preserve `PD-GW-001` through `PD-GW-010`, `PD-PLAT-001`
through `PD-PLAT-005`, `PD-SEC-003` through `PD-SEC-006`, and the PD3 handoff
defined by the current milestone plan.
