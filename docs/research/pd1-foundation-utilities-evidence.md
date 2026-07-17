# PD1 foundation-utilities evidence

**Date:** 2026-07-16

**Reference environment:** `prismdrake-vm` Gentoo guest

**Source revision:** `c6bcfec`

## Scope

This report records validation of the display-free PD1 foundation utilities
introduced by `3d16a7e` and documented by `c6bcfec`. It supplies measured
evidence for the utility boundary of `PD-CONFIG-001`, `PD-CONFIG-003`,
`PD-CONFIG-010`, `PD-SEC-001`, `PD-SEC-008`, `PD-OBS-001`, `PD-OBS-006`,
`PD-OBS-007`, `PD-REL-007`, and `PD-REL-008`. It does not claim that a shell,
settings service, or complete configuration pipeline exists yet.

## Host validation

The host workspace passed:

- `make validate`.
- GCC 15.3.0 Debug configure and build with warnings as errors.
- Clang 22.1.8 Debug configure and build with warnings as errors.
- All 46 CTest registrations; the foreign-owner fixture was explicitly skipped
  because the developer process is not root.
- Clang 22.1.8 ASan and UBSan with leak detection disabled only for the host
  sandbox's ptrace limitation.
- GCC link-time optimization.
- Clang-Tidy with the repository checks and no findings.
- The `format-check` target and `git diff --check`.

## Gentoo guest validation

Guest-local build trees under `/var/tmp` used the source shared at
`/mnt/shared/prismdrake-de`.

GCC 15.3.0 passed a Debug build with warnings as errors, all 46 CTest
registrations, the formatting target, and `make validate`. Clang 22.1.8 passed
the same build and CTest suite with Clang-Tidy enabled. A second Clang build
passed the complete suite with ASan, UBSan, and leak detection enabled.

The ordinary `prismdrake` user cannot change test-file ownership, so CTest
reports `AtomicFileTest.RejectsDestinationOwnedByAnotherUser` as an explicit
skip. The same built test was then run directly as guest root and passed,
confirming that atomic replacement rejects a destination owned by another
user while retaining its previous contents.

## Negative and recovery coverage

The suite exercises:

- Missing, relative, malformed, incorrectly owned, non-private, non-directory,
  and symbolic-link XDG runtime conditions without disclosing private paths.
- Missing, permission-denied, oversized, non-regular, empty, exact-limit, and
  binary bounded reads.
- Atomic creation and replacement, permission establishment and preservation,
  destination and parent-chain symlink rejection, foreign ownership, filename
  exhaustion, and temporary-file cleanup.
- A forced partial payload write using a scoped file-size limit. The operation
  fails before rename, removes its temporary file, and preserves the previous
  valid destination.
- Closed structured diagnostics, bounded rendering, generation zero and
  overflow, deterministic monotonic time, concurrent cancellation, and stable
  process exit mappings.

No GUI toolkit, display server, D-Bus session, network service, or global
mutable singleton is required by this slice.
