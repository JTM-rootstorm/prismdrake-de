# Prismdrake milestones and PD0 exit review

Milestone labels organize work; they are not release versions or promises of a
date. The project is currently in **PD0: identity, contracts, and repository
foundation**. This roadmap follows the project-wide specification's PD0–PD7
sequence, which has precedence over the earlier compressed sequence in the PD0
execution brief.

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
- [x] Toolkit research provides an evidence-based Proposed recommendation.

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
- [ ] Maintainers have accepted the component/process, toolkit, configuration,
  and token ADRs required for PD1.
- [x] Canonical local `make validate` passes on the completed PD0 tree.
- [ ] Repository CI is green on the reviewed PD0 revision.
- [ ] Maintainers have reviewed and approved the PD1 scope below.

Unchecked items are explicit owner/validation gates, not implied future work.

## Owner review checklist

- [ ] Accept or revise component names and process boundaries in ADR 0002.
- [ ] Accept or revise Qt 6 Quick and modern C++ direction in ADR 0003.
- [ ] Accept or revise TOML, XDG locations, snapshot semantics, and the draft
  D-Bus scope in ADR 0004.
- [ ] Accept or revise the shared JSON token model in ADR 0006.
- [ ] Review the visible distinction and accessibility defaults for Lustre and
  Forge.
- [ ] Review candidate Glasswyrm capability placeholders while keeping their
  signatures and transport unapproved.
- [ ] Confirm mandatory/optional dependency boundaries and the PD1 scope.

Status changes require maintainer review; the existence of PD0 files or future
implementation does not make a Proposed ADR Accepted.
