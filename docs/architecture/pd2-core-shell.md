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
| Outputs and surfaces | One registry reconciles bounded RandR and Qt screen observations, creates one desktop per active output, applies the configured panel-output policy, and transfers or dismisses global transient surfaces after topology changes. A dedicated output worker publishes its cancelable X11 operations before reply-bearing RandR setup, coalesces owner-thread delivery to the latest complete topology, and interrupts reply-bearing work during teardown. Dedicated bounded host workers own checked desktop and dock EWMH property transactions; Qt view staging and generation-checked publication remain GUI-thread work. | `PD-OUTPUT-001` through `PD-OUTPUT-004`, `PD-PANEL-001`, `PD-DESKTOP-001`, `PD-PERF-001`, `PD-PERF-004` |
| Panel and tasks | Shared panel QML presents ordered pins and running tasks, deterministic `always`, `never`, and `when_full` grouping, explicit active/minimized/urgent/modal state, group navigation, launch, activate, minimize, close, and pin actions. A dedicated task worker owns reply-bearing X11 refreshes, events, and checked EWMH requests to the active WM; event delivery and client invalidations coalesce within fixed bounds. | `PD-PANEL-001` through `PD-PANEL-005`, `PD-PANEL-008`, `PD-WIN-001` through `PD-WIN-007`, `PD-PERF-001`, `PD-PERF-004` |
| Launcher | The local XDG application catalog is monitored without polling. Dedicated workers own bounded recursive discovery, watch replacement, catalog refresh, search, launch, and icon lookup; owner-thread publication remains generation-checked. Active collection and at most one latest successor replace an unbounded recovery backlog. The launcher supports desktop actions, typed D-Bus activation, direct execution without an implicit shell where allowed, icons with fallback, pins, and Prismdrake destinations. | `PD-LAUNCH-001` through `PD-LAUNCH-005`, `PD-LAUNCH-007`, `PD-LAUNCH-008`, `PD-INT-007`, `PD-SEC-002`, `PD-PERF-001`, `PD-PERF-004`, `PD-PERF-006` |
| Desktop and wallpaper | A non-focusable standard X11 desktop surface is hosted per active output. One bounded decoded wallpaper snapshot is laid out in the configured mode on every output; candidates are scoped to the active settings-owner epoch and generation, while the last complete mapped desktop remains visible during an owner gap or replacement decode. Invalid or missing images use the configured solid color. | `PD-DESKTOP-001` through `PD-DESKTOP-003`, `PD-DESKTOP-005`, `PD-DESKTOP-006` |
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

The task-controller owner thread keeps presentation policy and immutable
generation publication. One dedicated `TaskIoWorker` owns its X11 connection,
blocks on the X11 event descriptor and a private wake descriptor, drains task
events, performs reply-bearing refreshes, and sends checked activate, minimize,
and close requests. Refresh hints and worker-to-owner event delivery are
replaceable and coalesced. Per-window invalidations are deduplicated up to the
task-model bound; overflow degrades to invalidating every cached client before
the next complete authoritative refresh instead of growing an unbounded list.
The connection-owning operations object is reachable by teardown before its
reply-bearing event and model setup begins. Setup, refresh, and request work each
force a bounded follow-up event drain before the worker sleeps; an
examination-limited batch forces one further bounded drain so events already
buffered inside libxcb cannot wait indefinitely for a descriptor edge. Action
work has a fixed queue bound and is rejected when its applied refresh token or
window incarnation is stale. Completions return through queued owner-thread
callbacks, and teardown cancels descriptor waits before joining the worker.
This keeps the path event-driven without transferring window-state authority
from the active WM.

This behavior implements the PD2 portions of `PD-PANEL-002` through
`PD-PANEL-005`, `PD-WIN-003` through `PD-WIN-007`, `PD-INPUT-001`, and
`PD-A11Y-001` through `PD-A11Y-009`, plus `PD-PERF-001`, `PD-PERF-004`, and
`PD-REL-007`.

## Launcher behavior and privacy

Launcher search is local, bounded, deterministic, incrementally replaceable,
and does not persist queries. Application-root events are coalesced into a
complete catalog refresh; deletion, recreation, or watch exhaustion requests a
bounded rescan instead of unbounded polling. Desktop-entry arguments are parsed
through the closed execution contract and are never passed to an implicit
shell. Terminal-only entries remain unavailable when no terminal policy exists.

One dedicated collection worker owns recursive application-root traversal and
filesystem-watch creation, registration, replacement, and removal. Its hard
work budget counts every visited node, including irrelevant files, while a
separate watch-capacity bound limits retained paths. Replacement generations
discard stale collection results. While one collection is active, recovery
requests replace one latest successor generation rather than enqueueing one
dispatch per event burst. Discovery and batched watch changes observe
cooperative stop requests, and teardown stops and joins the worker. The owner
thread receives only coalesced change notifications, generation-checked
results, and a bounded fail-soft recovery schedule.

PD2 has no recent-file, online-search, package-search, or document-index
provider. It does not store launcher history. Those limits implement the
privacy boundary in `PD-LAUNCH-006`, `PD-SEC-002`, `PD-SEC-008`, and
`PD-PERF-001`, `PD-PERF-004`, `PD-PERF-006`, and `PD-REL-007`.

## Desktop and wallpaper behavior

The desktop surface is presentation only. It does not accept focus, appear in
the task strip, provide icons, own root-window input, or implement file
operations. The active WM remains responsible for stacking and ordinary root
behavior. PD2 uses the same wallpaper source, mode, and background fallback on
every active output. The loader bounds the source file, image dimensions,
decoded allocation, and stale completion before publishing one immutable
generation. The runtime accepts a decoded candidate only when both its captured
settings-owner epoch and settings generation remain active. Owner loss clears
the candidate gate, but does not unmap the host's last complete desktop while a
new owner and replacement decode are pending.

Panel and desktop hosts stage their `QQuickView` objects on the GUI thread, but
each host's dedicated bounded X11-property worker owns a separate X11
connection, atom discovery, and checked dock or desktop property transaction.
Only a queued successful completion makes staged views visible and advances the
host generation. Superseded and removed views are destroyed on the GUI thread;
destroying their `QWindow` destroys the X11 window and its properties together,
so no delayed cleanup transaction can target a numeric XID after server reuse.
An in-flight replacement remains alive until its exact worker transaction
completes for the same reason. The runtime then uses the matching publication
callback as the atomic commit point for the complete settings, theme,
output-composition, or desktop-controller generation. A wallpaper-only update
may retain the same topology generation; its newer settings and wallpaper
state is still applied as one complete desktop update.

A failed property transaction never publishes a partial candidate. An existing
complete surface generation remains visible while the runtime enters its
bounded fail-closed shutdown path; an initial failure exposes no candidate
surface. Panel focusability changes use the same transaction boundary: the
replacement dock is published before the prior dock's `QWindow` is destroyed,
and focus is routed into a child control only after the exact replacement is
accepted.
Stale replacement completions are discarded without advancing presentation
state. These mechanics do not give Prismdrake authority over WM focus,
stacking, work areas, or window lifecycle, and they make no change to
Glasswyrm ownership or native interfaces.

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
