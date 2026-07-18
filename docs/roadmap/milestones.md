# Prismdrake milestones and PD1 status

Milestone labels organize work; they are not release versions or promises of a
date. **PD1: X11 shell skeleton and settings foundation completed on
2026-07-18; PD2 has not yet been activated.** PD1 was activated by maintainer
approval on 2026-07-16 after the PD0 decisions, dependency boundary, milestone
scope, and non-goals were reviewed. This roadmap follows the project-wide
specification's PD0–PD7 sequence.

## Milestone sequence

| Milestone | Scope | Exit evidence |
|---|---|---|
| PD0 | Canonical identity, architecture/config/theme/integration contracts, original low-fidelity mockups, validation, and CI | Owner approval for decisions needed by production code; no contract contradictions; `make validate` passes |
| PD1 | Accepted build/language scaffolding, X11 shell skeleton, session/settings prototypes, EWMH task model, runtime profile switching, deterministic UI harness | Shell starts/exits reliably in a development X11 session and core models are display-free where practical |
| PD2 | Usable core shell: panel/tasks, launcher/search, desktop, notifications, quick-settings framework, multi-output and keyboard baseline | Common launch/switch/minimize/close/notify/logout workflows work with Tier B fallbacks |
| PD3 | Production Lustre/Forge visual system, accessibility variants, GTK/Qt integration, appearance controls, golden and contrast tests | Both profiles are coherent, original, switchable, and usable without blur |
| PD4 | Separately Accepted Glasswyrm-native roles, blur, thumbnails, workspace/decor coordination, and diagnostics | Negotiation and mismatch fallbacks pass while Tier B remains functional |
| PD5 | Daily-use settings/service adapters, portals, isolated authorization, recovery, and secure locking only if reviewed | Common workflows avoid a terminal and security-sensitive paths pass focused review |
| PD6 | Accessibility, reliability, localization, performance, multi-output, packaging, and upgrade hardening | Documented reference environments meet the beta quality bar |
| PD7 | Stable supported interfaces, complete documentation, security review, migration/rollback, packaging, and release evidence | All mandatory 1.0 criteria pass or have explicit maintainer waivers |

Post-1.0 research may cover Wayland, sandboxed extensions, companion
applications, additional profiles, remote desktop, or multi-seat. None is a
current commitment.

## PD0 Definition of Done

### Identity and architecture

- [x] Canonical product/profile names, prefixes, namespace, repository, and
  generic `GW_*` family are documented.
- [x] Accepted identity and original-asset ADRs preserve owner-locked decisions.
- [x] Context/component diagrams and single-owner state boundaries exist.
- [x] Process startup, shutdown, crash recovery, safe mode, and fallbacks are
  specified without requiring systemd.
- [x] Dependency policy excludes mandatory GNOME desktop-stack components.
- [x] Toolkit research and the isolated Gentoo VM spike provide evidence for
  the Accepted Qt 6 Quick direction.

### Configuration, design, and integration

- [x] Strict version-1 configuration schema and Lustre, Forge, and accessible
  TOML examples exist.
- [x] Draft `org.prismdrake.Settings1` XML and atomic snapshot behavior are
  documented without an ABI stability claim.
- [x] Shared base, profile, and accessibility token documents define semantic
  parity and non-blur fallbacks.
- [x] Original editable low-fidelity SVGs cover required shell surfaces and
  accessibility fallback.
- [x] GTK, Qt, X11/freedesktop, and optional Glasswyrm behavior is mapped with
  explicit fallbacks.
- [x] Capability examples are detected-state documents, not protocol packets.

### Repository quality

- [x] README and documentation index expose current maturity and major contracts.
- [x] Contribution, security, issue, pull-request, and asset provenance guidance
  exists.
- [x] Maintainers have accepted the component/process, toolkit, configuration,
  and token ADRs required for PD1.
- [x] Canonical local `make validate` passes on the completed PD0 tree.
- [x] Repository CI is green on the reviewed pre-activation revision.
- [x] Maintainers have reviewed and approved the PD1 scope and non-goals.

## Activation record

- [x] Component names and process boundaries in ADR 0002 were accepted.
- [x] Qt 6 Quick and modern C++ direction in ADR 0003 was accepted.
- [x] TOML, XDG locations, snapshot semantics, and the explicitly draft
  D-Bus scope in ADR 0004 were accepted without stabilizing the interface.
- [x] The shared JSON token model in ADR 0006 was accepted.
- [x] The CMake, CTest, C++20, compiler, and testing baseline in ADR 0008 was
  accepted.
- [x] Mandatory and optional dependency boundaries, Gentoo development layers,
  local repository, development metapackage, future live-ebuild scope, and PD1
  scope/non-goals were approved.

The accepted decisions authorize prototype implementation; they do not make
Prismdrake usable, stabilize draft interfaces, accept candidate Glasswyrm
protocols, or waive PD1 validation requirements.

## PD1 completion record

- [x] The Experimental session, settings daemon, and shell configure and build
  with GCC and Clang under the Accepted C++20 warnings policy.
- [x] The full reference suite passes under Qt 6.11, including isolated X11,
  D-Bus, accessibility, visual-candidate, failure, and performance lanes.
- [x] The exact Gentoo package passes QA, both USE resolutions, package tests,
  installed AT-SPI and end-to-end replay, unmerge preservation, and ordinary
  reinstall with byte-identical executables.
- [x] The standard X11/freedesktop boundary remains authoritative; no native
  Glasswyrm protocol or shell-side compositor behavior is claimed.
- [x] PD2 behavior and the remaining Experimental limitations stay explicitly
  deferred.

The exact revision, artifact hashes, measured runtime linkage, and privacy
boundary are recorded in the
[PD1 Gentoo package lifecycle evidence](../research/pd1-portage-lifecycle-evidence.md).

## Continuing owner review after PD1

These items remain owner-review concerns for later milestones. They did not
block PD1 activation or completion:

- [ ] Review the visible distinction and accessibility defaults for Lustre and
  Forge.
- [ ] Review candidate Glasswyrm capability placeholders while keeping their
  signatures and transport unapproved.
