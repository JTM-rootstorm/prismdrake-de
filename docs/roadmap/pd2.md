# PD2 active milestone tracker

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
| Preserved untracked working areas | `Plans/`, `vm-passthrough/` |
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
- [ ] PD2 configuration, state, and Experimental IPC contracts are frozen.

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
