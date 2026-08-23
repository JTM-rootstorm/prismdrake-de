# PD2 active milestone tracker

Implementation sequencing is now owned by the workspace-root
`PRISMDRAKE_RUST_FIRST_AND_WYRROOT_PORTING_PLAN.md`. This tracker remains
authoritative for the PD2 behavior and exit obligations carried into that
transition; it no longer directs a separate C++-first completion pass.
Initial Wyrmroot integration follows approximate functional Rust parity with
the current C++ product. Full installed PD2 closure remains required for a PD2
completion claim, but it is not the entry condition for that non-graphical port.

Prismdrake entered **PD2: Core desktop shell** on 2026-07-18 after the
maintainer selected the version 1.0.0 PD2 execution plan. PD1 completed on the
Gentoo reference environment before activation. PD2 remains Experimental: its
interfaces may change, its visual system is not production-ready, and it is not
a daily-use desktop release.

Qt 6.11 is the minimum supported version for every Qt-bound Prismdrake
component. The Gentoo reference environment provides authoritative product
build, runtime, X11, D-Bus, accessibility, and packaging evidence. Repository
contract validation on older hosted Qt versions is not product compatibility
evidence and is not a PD2 exit requirement.

## Activation baseline

| Field | Value |
|---|---|
| Activation date | 2026-07-18 |
| Required and actual starting revision | `7b68d9eff54f5a3b89267c8c41ee49df64a6a659` |
| Starting branch | `main` |
| Tracked user changes | None |
| Preserved untracked working areas | `vm-passthrough/`; active coordination plan is workspace-root `PRISMDRAKE_RUST_FIRST_AND_WYRROOT_PORTING_PLAN.md` |
| Local contract validation | Passed; 39 negative rejection fixtures |
| Required Qt floor | 6.11 |

The maintainer approved the PD2 execution plan, including its scope, non-goals,
configuration and state ownership, notification-service boundary,
multi-output surface policy, Experimental shell and session control, and
wave-based parallel execution model. This approval does not stabilize any API
or approve candidate `GW_*` protocols.

### Reference baseline evidence

The exact pre-PD2 revision was replayed from an isolated source archive in the
maintainer-provided Gentoo VM before feature work:

| Subject | Observed baseline |
|---|---|
| Gentoo | Base System 2.18, amd64 23.0 systemd profile |
| Qt | 6.11.1 (`qtdeclarative` 6.11.1-r1) |
| GCC | 15.3.0 |
| Clang | 22.1.8 |
| CMake | 4.3.3-r1 |
| Portage | 3.0.81.2 |
| Xorg | 21.1.24 |
| Openbox | 3.6.1-r11 |
| AT-SPI | 2.58.6 |

Both warnings-as-errors compiler builds completed 321 build steps and
registered 565 tests. Each suite passed 564 tests with zero failures and the
same documented root-only permission test skipped. Formatting, all repository
contracts and 39 negative fixtures, `pkgcheck`, and live-ebuild resolution with
the Qt 6.11 floor also passed.

### Wave 0 status

- [x] Baseline and user work recorded and preserved.
- [x] PD2 status activated without changing the Experimental maturity claim.
- [x] Root format registration replaced by automatic source-tree-only target
  registration, excluding generated build outputs.
- [x] Gentoo and shell-runtime test registration moved behind feature-local
  CMake entry points without changing the 565-test manifest.
- [x] Post-sharding GCC and Clang suites repeated the 564-pass, one-skip,
  zero-failure baseline.
- [x] Post-sharding format check, all three QML lint targets, and a
  `BUILD_TESTING=OFF` runtime-only build passed.
- [x] PD2 configuration, state, and Experimental IPC contracts are frozen.

The Wave 0 contract freeze is recorded by Accepted ADR 0009 and the strict
configuration-v2, runtime-snapshot-v2, pin-state-v1, notification-history-v1,
and notification-state-v1 schemas. Experimental `Shell1`, `Session1`, and
`NotificationCenter1` use fixed same-user methods, generation or transaction
guards, bounded payloads, and redacted typed errors. One shared fail-closed
peer-credential helper now supplies the EUID policy used by settingsd and by
the PD2 services that follow.

The exact post-freeze source was replayed in the Gentoo VM with GCC 15.3.0 and
Clang 22.1.8. Both warnings-as-errors builds registered 571 tests and passed
570 with zero failures; the same root-only permission test remained the sole
documented skip. Formatting, 61 contract-negative paths, and `pkgcheck` also
passed. The local immutable branch `codex/pd2-wave1-base` identifies this
reviewed worker base; it is not pushed.

### Wave 1 implementation status

The parallel Wave 1 foundations are implemented and passed their combined
integration gate before the Wave 2 worker base was frozen:

- RandR and Qt observations reconcile into stable named-output generations,
  and one registry owns desktop, panel, and transient placement for every
  valid output.
- `prismdrake-notifyd` implements the freedesktop Notifications 1.3 service,
  bounded session or opt-in persistent history, sender and action lifecycle,
  do-not-disturb policy, and the Experimental notification-center interface.
  Settings refresh is asynchronous and retries with bounded backoff so a
  missing or slow settings owner cannot block notification dispatch.
- Application identity follows the Accepted deterministic association order;
  desktop-file identifiers, icon-theme discovery, and pin persistence have
  explicit input, work, storage, and no-follow filesystem bounds.
- Experimental shell and session control use fixed typed commands, same-user
  authorization, owner epochs, and a bounded logout transaction. Runtime bus
  ownership and process composition remain coordinator-owned Wave 3 work.
- Wallpaper state uses immutable generations, bounded local regular-file
  reads, worker-side Qt image decoding, solid fallback, stale-load
  cancellation, and deterministic center, fit, fill, stretch, and tile
  geometry. Wave 2 now supplies the corresponding per-output presentation.

No Wave 1 implementation introduces a native `GW_*` path, compositor blur,
desktop icons, an implicit command shell, a mandatory GNOME desktop-stack
dependency, or an external system-service authority.

The combined source registered 651 tests. A clean GCC 15.3 warnings-as-errors
host build passed 640 tests with zero failures and 11 explicit environment or
opt-in skips. Its staged install contained the expected 21 files with no
missing, unexpected, duplicate, or prefix-violating entry. The Gentoo
reference VM repeated all 380 GCC build steps and passed 648 tests with zero
failures and three explicit skips. Clang 22.1.8 repeated the same 380 build
steps, 648 passes, and three skips. Formatting, 61 contract-negative paths,
and Portage QA passed.

### Wave 2 implementation status

The five core-surface lanes are implemented and passed their combined
integration gate before the Wave 3 worker base was frozen:

- The panel presents ordered pins and grouped tasks, bounded group actions, a
  localized clock, and output-aware placement. Production and development
  affordances are separate explicit inputs, so grouped task state cannot
  accidentally enable diagnostic controls.
- The launcher uses the bounded application catalog and typed activation path,
  monitors desktop-entry changes, prefers standard D-Bus activation, and uses
  direct argument-vector launch only for validated fallback entries. Pin
  mutations remain owned by the persistent pin store.
- The notification center and transient stack mirror complete typed notifyd
  state. History, actions, close requests, do-not-disturb state, service loss,
  and presentation identity remain behind the notification authority boundary.
- One non-focusable desktop surface is staged per valid output. Wallpaper and
  topology generations apply atomically; conflicting generations and a failed
  output-window replacement leave the previous complete desktop visible.
- Quick settings exposes only the five Prismdrake-owned PD2 capabilities.
  Settings and notification mutations are correlated with their exact owner
  epoch and resulting generation, so unrelated publications cannot complete or
  reject a pending request.

This checkpoint does not yet compose the modules into the installed shell or
claim a usable desktop session. Runtime ownership, cross-surface focus,
session supervision, live multi-output recovery, and installed lifecycle proof
remain Wave 3 and later work. It adds no native `GW_*` path, compositor blur,
fake audio, networking, Bluetooth, power, or lock controls, desktop icons, or
new external-service authority.

The combined source registered 701 tests. A clean GCC warnings-as-errors host
build passed all 701 with zero failures and 13 explicit environment or opt-in
skips. Its staged install contained the expected 21 files. Formatting, all QML
lint targets, and 61 contract-negative paths passed. The exact checkpoint was
then replayed in the Gentoo reference VM. GCC 15.3.0 and Clang 22.1.8 each
completed 478 warnings-as-errors build steps and passed all 701 tests with zero
failures and the same three explicit skips: unavailable live AT-SPI test
dependencies, the root-only ownership rejection, and the opt-in development
demonstration. Formatting, QML lint, and 61 contract-negative paths passed
there as well. The exact overlay also passed `pkgdev manifest` and `pkgcheck`.

### Integrated source checkpoint

Subsequent work composes the PD2 modules into the Experimental shell and
session paths described by the
[core-shell implementation status](../architecture/pd2-core-shell.md). The
repository now also contains strict, source-bound collectors and validators for
the two-output installed workflow, live accessibility, runtime performance,
and the final Portage lifecycle. The installed package records its exact source
revision in `/usr/share/prismdrake/build-provenance.json`; preflight attestation
binds that record, the installed executables and ownership set, and the complete
reviewed demonstration-harness manifest.

The installed workflow contract contains 48 ordered checks, including exact
accessible descriptions and states, reverse traversal, output-migration focus,
and focus recovery after settingsd, notifyd, and shell restarts. The performance
observer maps each declared metric to a distinct runtime endpoint and binds its
records to the source revision. These are implemented evidence mechanisms and
synthetic rejection tests, not results from the final reference run.

No PD2 completion claim is made at this checkpoint. The full Gentoo compiler,
test, sanitizer, live multi-output, AT-SPI, performance, Portage install,
unmerge-preservation, and ordinary-reinstall matrix below remains required.

## Active workstreams

1. **Shared foundation:** parallel-friendly build/test registration,
   configuration version 2, runtime snapshot version 2, state schemas, and
   Experimental IPC contracts.
2. **Output ownership:** coherent RandR/Qt topology generations, per-output
   surface placement, panel policy, and output migration.
3. **Notification authority:** toolkit-neutral freedesktop service, bounded
   history, sender lifecycle, do-not-disturb, and immutable shell snapshots.
4. **Application identity:** deterministic desktop-entry association, bounded
   icon resolution, and ordered persistent pins.
5. **Session control:** fixed same-user shell commands, logout coordination,
   bounded shutdown, and notification-daemon supervision.
6. **Desktop:** bounded local wallpaper decoding and one non-focusable desktop
   surface per active output.
7. **Core surfaces:** grouped panel tasks, production launcher, notification
   center, and typed quick settings for Prismdrake-owned state only.
8. **Integration:** cross-surface focus, output/service restart recovery, Tier B
   and Tier C workflows, accessibility, internationalization, and performance.
9. **Gentoo lifecycle:** QA, dependency resolution, package tests, installed
   demonstration, user-state preservation, unmerge, and reinstall.

## Architecture boundary

The active window manager remains authoritative for focus, stacking, window
state, workspaces, and window actions. Glasswyrm remains authoritative for
composition, blur, capture, thumbnails, output policy, and native capabilities.
PD2 implements standards-first X11 and freedesktop behavior and does not add a
native `GW_*` protocol, shell-side blur, secure locking, portals, desktop file
operations, or real external audio/network/power adapters.

## Exit gate

PD2 completes only when Portage-installed artifacts let a developer perform the
documented launch, pin, group, switch, minimize, close, notify, history,
do-not-disturb, profile-switch, output-migration, restart-recovery, and logout
workflow in the Gentoo reference VM. The same bounded core workflow must work
without Glasswyrm-native capabilities and without compositor blur.

Completion also requires GCC and Clang warnings-as-errors builds, the full
CTest and sanitizer matrix, QML lint, formatting, contract validation, live
multi-output and AT-SPI evidence, Portage QA and package tests, unmerge
preservation, and reinstall. Skips must be explicit; unobserved checks are not
reported as passing.

## Deferred boundary

PD3 retains production Lustre and Forge styling, reviewed visual goldens,
wallpaper contrast certification, GTK and Qt application integration,
appearance controls, and later mixed-scale visual hardening. PD2 does not claim
those outcomes or a stable public interface.

Relevant requirements: `PD-PANEL-001` through `PD-PANEL-005`, `PD-PANEL-008`,
`PD-LAUNCH-001` through `PD-LAUNCH-005`, `PD-LAUNCH-007`, `PD-LAUNCH-008`,
`PD-DESKTOP-001` through `PD-DESKTOP-003`, `PD-DESKTOP-005`,
`PD-DESKTOP-006`, `PD-NOTIFY-001` through `PD-NOTIFY-007`,
`PD-QUICK-001` through `PD-QUICK-006`, `PD-SESSION-001` through
`PD-SESSION-006`, `PD-OUTPUT-001` through `PD-OUTPUT-004`, and the applicable
cross-cutting accessibility, security, reliability, packaging, API, testing,
and documentation requirements.
