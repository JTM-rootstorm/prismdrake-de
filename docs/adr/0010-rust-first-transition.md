# ADR 0010: Rust-first implementation transition

- **Status:** Accepted
- **Date:** 2026-08-23
- **Accepted:** 2026-08-23 by maintainer direction
- **Owners:** Prismdrake maintainers
- **Amends:** ADR 0003 language ownership and ADR 0008 build, language, and
  testing baseline
- **Does not supersede:** ADR 0009 state ownership and control contracts

## Context

Prismdrake's Experimental PD2 implementation currently uses C++20 for both
toolkit-neutral policy and Qt-bound integration. Qt 6.11 Quick/QML remains the
accepted visible-shell technology, while D-Bus and versioned file contracts
already separate several non-visual authorities from the shell process.

The workspace transition plan selects a Rust-first migration before final PD2
acceptance and before any general portability or Wyrmroot work. That plan is a
sequencing authority, not an Accepted repository architecture decision. A
repository ADR must define which earlier language and build decisions change,
which contracts remain fixed, and how a mixed Rust/Qt transition avoids
duplicate state authority.

## Decision drivers

- Keep Qt 6.11 Quick/QML and its accessibility support for visible surfaces.
- Prefer memory-safe toolkit-neutral implementation without hiding unsafe or
  native boundaries.
- Preserve the existing process, D-Bus, schema, state-ownership, fallback, and
  standards-first contracts during migration.
- Keep focused unit and contract tests fast while retaining complete
  GCC/Clang, sanitizer, package, accessibility, and installed-lifecycle gates.
- Support source archives and authoritative offline distribution builds with
  system-reviewed inputs.
- Make every cutover atomic, reversible, and observable, with exactly one
  authority for each state domain.
- Avoid speculative Wyrmroot APIs or dependence on the workspace Rust fork.

## Considered options

1. Adopt Rust as the default toolkit-neutral implementation language, retain
   Qt/QML and narrow justified C++/native integration, and migrate by vertical
   behavioral slices.
2. Complete and accept PD2 in C++ before beginning a separate Rust rewrite.
3. Rewrite the complete shell and all services in one Rust/Qt cutover.
4. Retain C++20 as the default implementation language indefinitely.

The first option reuses existing process contracts and lets every slice prove
parity before deleting its predecessor. A separate completed C++ cycle would
repeat expensive installed acceptance and maintain duplicate fixes. A single
mass cutover would obscure authority, rollback, and regression boundaries.
Keeping C++ remains viable but does not implement the maintainer-selected
Rust-first direction.

## Decision

Rust is Prismdrake's default language for toolkit-neutral policy, state
machines, parsers, services, validation semantics, and portable domain models.
Qt 6.11 Quick/QML remains the visible-shell technology. C++ remains permitted
for Qt-bound presentation and native integration where removal would add an
unproven bridge or reduce correctness. C and assembly require a documented
platform or ABI need.

The existing process model remains authoritative. Separate services communicate
through their versioned D-Bus contracts; they do not gain an in-process FFI
bridge merely because both endpoints contain Rust. ADR 0009 continues to own
settings, notification, shell, session, and persistent-state authority. Every
cutover must preserve executable names, bus names, object paths, interfaces,
typed errors, owner epochs, state schemas, and fallback behavior, and must leave
exactly one runtime and package owner for each authority.

Cargo owns Rust compilation, dependency resolution, Rust tests, and the tracked
`Cargo.lock`. CMake continues to own Qt, C++, QML, translations, and their
tests. Gentoo packaging and the project workflow front end coordinate these
systems without recursive duplicate builds. Authoritative package builds use
immutable reviewed sources, work without Git metadata, and run offline after a
separately controlled preparation step. The workspace `rust/` checkout is the
Wyrmroot toolchain fork and is not a Prismdrake dependency.

At most one in-process Rust/Qt bridge technology may be authoritative. Its
selection requires a bounded comparison and a recorded acceptance update. The
public boundary exposes only project-owned, versioned data with explicit
allocator, layout, lifetime, thread, cancellation, unwind, panic, exception,
and teardown contracts. Pure domain crates forbid unsafe code. Unsafe and FFI
are permitted only in allowlisted boundary modules with local safety rationale,
focused malformed-input tests, sanitizers, applicable Miri evidence, and the
required exact-diff security review.

Validation is divided into explicit T0 through T4 tiers. Ordinary format,
compile, unit, model, parser, and saved-evidence work must not silently invoke
Portage, a VM, hardware, or another external system. Evidence capture,
normalization, schema validation, semantic validation, and attestation remain
separate identities. Historical C++ evidence stays revision-bound provenance
and does not become Rust acceptance evidence.

Migration proceeds through one authoritative vertical slice at a time. Tests
and rollback proof move before ownership; superseded C++ is deleted only after
the Rust slice passes parity and atomic-cutover gates. A failed cutover restores
the complete prior executable, activation, configuration, ownership, and
permission state without duplicate bus-name authority. Initial Wyrmroot
integration begins only after Rust covers approximately the current C++ product
behavior with the plan's functional parity gate. It does not wait for unrelated
Glasswyrm Rust-transition completion, but graphical Prismdrake still waits for
the accepted Glasswyrm and Qt/QPA contracts it actually consumes. Native
Glasswyrm expansion, PD3 work, and unrelated visual polish remain outside this
decision.

## Consequences

Prismdrake gains a mixed Rust/Qt build during migration and therefore carries
temporary integration and packaging complexity. The project must review Rust
dependencies, build scripts, procedural macros, unsafe code, native code, and
license compatibility as rigorously as its current system dependencies. Cargo
cannot download or select uncontrolled inputs during authoritative builds.

ADR 0003 continues to govern Qt 6.11 Quick/QML visible surfaces, accessibility,
and the separation of presentation from policy; this ADR amends its
C++-by-default language wording. ADR 0008 continues to govern CMake/CTest,
C++20 targets, GCC/Clang, system dependencies, warnings, sanitizers, install
behavior, and archive builds for retained native code; this ADR adds Cargo and
Rust gates rather than silently replacing applicable native gates.

This Accepted record changes repository architecture status and records Mike's
active 2026-08-23 direction to implement the Rust-first transition within the
active plan. Neither this ADR nor any other document authorizes mutation.
Implementation authority comes only from Mike's active request together with
the workspace policy that governs its scope. Every VM, package, remote, or other
external effect still requires Mike's active, exact authorization for its target
and operation under workspace rules, and VM or package operations remain owned
by the main/root coordinator. Phase implementation remains subject to the
plan's prerequisite, dependency, validation, and security gates.

## Validation or evidence

The behavioral oracle is Prismdrake revision
`98e19c79e0cdc9ccb980d35dc68d671e1d4fe520`; its tree matches historical
pre-signing revision `77a82492789fc46a9aee27343abb7357ac968ffb` but evidence
does not transfer between revisions. The checked-in PD2 CTest inventory records
838 registrations and a package selection of 834 after four separately tested
sandbox exclusions. These are migration inputs, not fresh passing results.

The maintainer accepted this decision on 2026-08-23 with the direction to bring
Rust to approximate functional parity with the current C++ work before starting
Wyrmroot integration instead of making Prismdrake wait for Glasswyrm migration.
The workspace transition plan's P0 component, contract, test, evidence,
dependency, and rollback maps remain the implementation baseline. Subsequent
work requires the phase-specific validation and security evidence named by the
plan; full installed PD2 closure remains a later release claim rather than the
precondition for initial non-graphical Wyrmroot integration.

## Revisit conditions

Revisit if the Gentoo toolchain or offline package path cannot support the
selected Rust dependencies; if the bridge cannot establish allocator, ABI,
threading, teardown, and failure safety; if migration creates permanent
duplicate authority; or if measured maintenance and correctness evidence favors
a different retained boundary.

Revisit the language boundary before adding another in-process bridge, exposing
a stable implementation ABI, beginning general portability conversion, or
consuming a nonstandard Rust toolchain.

## References

- [ADR 0003: Visible shell toolkit and language direction](0003-shell-toolkit.md)
- [ADR 0008: Build, language, and testing baseline](0008-build-language-and-testing-baseline.md)
- [ADR 0009: PD2 state ownership and control contracts](0009-pd2-state-and-control-contracts.md)
- [Project specification section 12](../PRISMDRAKE_PROJECT_SPECIFICATION.md#12-technology-and-dependency-policy)
- [Dependency policy](../architecture/dependency-policy.md)
- Workspace plan `PRISMDRAKE_RUST_FIRST_AND_WYRROOT_PORTING_PLAN.md`
