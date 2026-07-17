# PD1 configuration-loader evidence

**Date:** 2026-07-16

**Reference environment:** `prismdrake-vm` Gentoo guest

**Source revision:** `be7a370`

## Scope

This report records validation of the display-free PD1 version-1 configuration
parser, semantic validator, recovery loader, and narrow whole-document writer.
The implementation supplies measured evidence for `PD1-006`, `PD-CONFIG-001`
through `PD-CONFIG-004`, `PD-CONFIG-006`, `PD-CONFIG-007`, `PD-CONFIG-009`,
`PD-CONFIG-010`, `PD-SEC-008`, and `PD-REL-002` at the configuration boundary.
It does not claim that `prismdrake-settingsd`, D-Bus publication, theme
resolution, or atomic generation publication exists yet.

## Dependency selection

The module uses system toml++ 3.4.0 discovered through its CMake package
configuration. The Gentoo reference build links `libtomlplusplus.so.3` from the
system library directory. Configuration with
`CMAKE_DISABLE_FIND_PACKAGE_tomlplusplus=TRUE` fails with actionable package
guidance; the build does not download or vendor a parser.

## Host validation

The host workspace passed:

- `make validate` and `git diff --check`.
- GCC 15.3.0 Debug compilation with warnings as errors and the complete CTest
  suite.
- Clang 22.1.8 compilation with warnings as errors and Clang-Tidy with no
  findings.
- Clang 22.1.8 ASan and UBSan; host-only leak detection remained disabled for
  the sandbox's ptrace limitation.
- GCC release link-time optimization.
- The `format-check` target.
- A build with `PRISMDRAKE_ENABLE_DEVELOPER_OVERRIDES=ON`, including the
  developer-policy branch.

## Gentoo guest validation

Guest-local build trees under `/var/tmp` used revision `be7a370` from the source
shared at `/mnt/shared/prismdrake-de`.

GCC 15.3.0 passed a Debug build with warnings as errors, all 73 CTest
registrations, `format-check`, and `make validate`. Clang 22.1.8 passed the same
suite with warnings as errors and Clang-Tidy enabled. A second Clang build
passed all 73 registrations with ASan, UBSan, and leak detection enabled. The
one ordinary-user ownership test reported an explicit skip in each CTest run;
the same built test ran separately as guest root and passed.

A GCC developer-mode build passed the explicit developer-policy test. A
separate GCC release build with link-time optimization also completed. The
missing-toml++ configuration failure and dynamic linkage to
`libtomlplusplus.so.3` were verified in the guest.

## Contract and negative coverage

The display-free suite verifies:

- All committed Lustre, Forge, accessibility, and packaged-default documents
  parse into complete typed values.
- Syntax failures remain distinct from semantic validation and unsupported
  versions.
- Empty, malformed, duplicate-key, non-UTF-8, oversized, unknown-key,
  wrong-type, out-of-range, non-finite, invalid-color, invalid-profile,
  Unicode-length, excessive-array, and duplicate-array inputs fail safely.
- Diagnostics retain bounded canonical schema paths without echoing private
  paths, unknown keys, rejected values, or secret-like input.
- Production policy validates and then disables developer diagnostics and mock
  capabilities unless both build-time and caller policy explicitly enable
  them.
- Startup and reload distinguish missing, invalid, last-known-valid, and
  packaged-default sources without partially publishing a candidate.
- Invalid documents never replace the user file or last-known-valid state.
  Promotion revalidates retained TOML and verifies that it normalizes to the
  same published candidate before writing.
- The foundation atomic writer covers destination and parent symlinks, foreign
  ownership, permission preservation, interrupted writes, pre-rename cleanup,
  and distinct post-rename durability uncertainty.

Configuration parsing and recovery do not assign a generation. Theme
resolution and one-generation settings publication remain later PD1 work.
