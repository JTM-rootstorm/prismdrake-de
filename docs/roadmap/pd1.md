# PD1 candidate backlog and Definition of Ready

PD1 is an X11 shell skeleton and settings foundation, not a daily-use desktop.
Work must stay behind accepted build, component, toolkit, configuration, and
token decisions. Each issue should cite `PD-*` requirements and include
testable acceptance criteria, fault behavior, and fallback behavior.

## Candidate workstreams

1. **Build and language scaffolding:** accept CMake and C++ standards, compiler
   policy, dependency manifests, formatting, and display-free test entry points.
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

## Definition of Ready

PD1 may begin only when:

- [x] ADR 0001 project identity is Accepted.
- [ ] ADR 0002 component/process model has owner approval.
- [ ] ADR 0003 visible shell toolkit and language direction has owner approval.
- [ ] ADR 0004 configuration format and locations has owner approval.
- [ ] ADR 0006 theme-token model has owner approval.
- [x] Theme-token schema, Lustre, Forge, and accessibility validation passes.
- [x] Required low-fidelity mockups and compatibility matrix exist.
- [x] Glasswyrm documents label native names and wire behavior as draft.
- [ ] `make validate` and repository CI pass on the reviewed PD0 revision.
- [ ] Maintainer review finds no unresolved contradiction among the specification,
  README, ADRs, schemas, examples, and interface drafts.
- [ ] PD1 workstreams are converted into bounded issues with testable acceptance
  criteria and named requirement IDs.

The unchecked decisions are real gates. Research spikes may be isolated and
removable, but production scaffolding must not assume their outcome.

## Required PD1 validation posture

PD1 issues should name display-free unit tests first, then isolated D-Bus or
X11 integration under Xvfb/Xephyr as appropriate. Toolkit acceptance evidence
must cover keyboard, AT-SPI, contrast/fallback, motion, target size, multi-output
and scaling, deterministic rendering, restart behavior, and dependency capture.
