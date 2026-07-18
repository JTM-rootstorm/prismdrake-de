# PD1 completed milestone tracker

PD1 is an X11 shell skeleton and settings foundation, not a daily-use desktop.
The maintainer activated this milestone on 2026-07-16 and its exact Gentoo
reference lifecycle completed on 2026-07-18. PD2 has not yet been activated.
The archived execution plan and the package-lifecycle evidence retain the
requirement, test, fault, and fallback record for each completed work package.

Qt 6.11 is the minimum supported visible-shell toolkit version. The Gentoo
reference environment supplies the authoritative Qt-bound build, compiler,
formatting, QML-lint, and integration evidence. GitHub Actions remains a
repository-contract lane while its Ubuntu system Qt is below that floor.
Hosted product-build automation may be added later on a supported system
runner, but it is not a PD1 exit requirement when the recorded Gentoo reference
matrix satisfies the product gate.

## Active workstreams

1. **Build and language scaffolding:** implement the Accepted CMake, CTest, and
   C++20 standards, compiler policy, dependency manifests, formatting, and
   display-free test entry points.
2. **Session environment prototype:** start and stop an init-neutral development
   session with bounded supervision and a safe exit path.
3. **Settings loader:** parse untrusted TOML, validate version 1, write
   atomically, preserve defaults, and publish immutable generation snapshots.
4. **Profile switching:** resolve base plus Lustre or Forge tokens and switch one
   complete generation without changing accessibility preferences.
5. **Basic panel window:** create a development-only shell surface under Xorg or
   Xephyr with standard dock type and work-area reservation.
6. **EWMH task-model proof:** mirror active-window and client-list lifecycle,
   reject stale identifiers, and send state requests through the WM.
7. **Launcher model:** discover desktop entries incrementally without a shell,
   validate execution fields without implicit shell invocation, and support
   cancellation.
8. **Notification presentation proof:** render a bounded synthetic presentation
   model without yet claiming a production notification service.
9. **Deterministic visual harness:** exercise Lustre, Forge, high contrast,
   reduced motion, disabled transparency, and missing blur with controlled time.
10. **Dependency and packaging verification:** capture actual Gentoo-visible
    direct/transitive dependencies and confirm optional adapter isolation.
11. **Accessibility smoke tests:** verify keyboard traversal, visible focus,
    accessible roles/names/states, text scaling, and minimum targets.
12. **Glasswyrm capability shim:** consume only the standard fallback snapshot;
    candidate native `GW_*` protocols remain unimplemented until separately
    Accepted by Glasswyrm.

## Current implementation boundary

The Experimental tree now contains the shared Lustre/Forge theme projection,
passive launcher and notification presentation adapters, an actionable task
presentation, one token-driven and keyboard-operable panel component, and a
Qt/X11 window host that applies the documented primary-output bottom-dock
policy through standard EWMH properties,
and an event-driven controller that mirrors authoritative EWMH task state and
sends only checked standard WM requests. A live asynchronous settings client
now consumes complete owner-epoch-scoped snapshots through the Experimental
internal D-Bus contract and publishes only canonical typed state.
The Experimental `prismdrake-shell` composition root connects that settings
epoch to the shared theme projection, panel and launcher Quick views,
asynchronous launcher controller, EWMH task controller, and standards-only
panel window host. Settings-owner loss removes only the presentation epoch; a
later complete owner epoch rebuilds it without terminating the shell process.
Task buttons expose a code-native generic glyph and one bounded in-panel action
surface. Keyboard Menu or Shift+F10 and pointer secondary activation open the
exact target's accessible Minimize and Close actions; every request still flows
through the checked EWMH controller and is confirmed only from a later
authoritative observation.
An init-neutral Experimental session executable validates its environment and
display, starts settingsd before the shell, waits for bounded settings
readiness, then waits on a private exact-child channel until the shell has one
complete panel presentation epoch. It applies component-specific restart
budgets, enters one observable safe-mode launch, and performs exact-PID reverse
bounded shutdown without touching the window manager or unrelated
applications.
The three Experimental processes, their read-only data, and a standard X11
session entry now share one CMake install contract. The exact Portage-installed
artifact passed package tests, installed AT-SPI and end-to-end replay, unmerge
preservation, and an ordinary byte-identical reinstall. The production build
tree also has bounded live AT-SPI metadata, forward/reverse focus, exact task
activation, minimization, and close evidence, plus supervised settings-owner
loss and complete presentation-epoch recovery on the reference guest. This is
still a development prototype, not a complete or daily-use shell.

The deterministic visual candidate suites satisfy the PD1 harness outcome;
promotion to reviewed golden images and a cross-version tolerance policy remain
PD3 work. The panel's accessible **Applications** action is the bounded PD1
launcher entry. A global shortcut is deferred to PD2 because `PD-INPUT-002`
requires coordination with the authoritative WM or an Accepted negotiated
interface.

## Activation gate

PD1 may begin only when:

- [x] ADR 0001 project identity is Accepted.
- [x] ADR 0002 component/process model has owner approval.
- [x] ADR 0003 visible shell toolkit and language direction has owner approval.
- [x] ADR 0004 configuration format and locations has owner approval.
- [x] ADR 0006 theme-token model has owner approval.
- [x] ADR 0008 build, language, and testing baseline has owner approval.
- [x] Theme-token schema, Lustre, Forge, and accessibility validation passes.
- [x] Required low-fidelity mockups and compatibility matrix exist.
- [x] Glasswyrm documents label native names and wire behavior as draft.
- [x] `make validate` and repository CI pass on the reviewed pre-activation
  revision.
- [x] Maintainer review finds no unresolved contradiction among the specification,
  README, ADRs, schemas, examples, and interface drafts.
- [x] PD1 workstreams are converted into bounded work packages with testable
  acceptance criteria and named requirement IDs in the maintainer-approved
  execution plan.

The activation gate is complete. Completion of the gate authorizes prototype
implementation; it does not satisfy the PD1 exit criteria or convert
experimental evidence into production support.

## Completion record

PD1's exit gate passed at
`e747480ad6a8b2d6ea59beb931d0d80797881ca9`. GCC 15.3.0 and Clang 22.1.8
warnings-as-errors builds each passed 565 registrations; formatting, all three
Qt 6.11 QML lint targets, repository validation, `pkgcheck`, both Portage
resolutions, package tests, installed runtime demonstrations, unmerge
preservation, and ordinary reinstall passed. The detailed hashes and remaining
later-milestone boundary are recorded in the
[PD1 Gentoo package lifecycle evidence](../research/pd1-portage-lifecycle-evidence.md).

## Required PD1 validation posture

PD1 issues should name display-free unit tests first, then isolated D-Bus or
X11 integration under Xvfb/Xephyr as appropriate. Toolkit acceptance evidence
must cover keyboard, AT-SPI, contrast/fallback, motion, target size, multi-output
and scaling, deterministic rendering, restart behavior, and dependency capture.
