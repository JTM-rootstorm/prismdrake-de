# PD1 build and toolchain evidence

- **Status:** Observed production-foundation evidence; PD1 packaging and runtime work remain incomplete
- **Date:** 2026-07-16
- **Repository checkpoint:** `d1e2972b6e955076f82012e328cb42e966cd090f`
- **Reference environment:** `prismdrake-vm`, protected by snapshot `prismdrake-pd1-stage0-20260716`

This report records the first production C++ target under Accepted ADR 0008.
It covers `PD-DEP-005` through `PD-DEP-008`, `PD-TEST-001`, `PD-TEST-007`,
`PD-TEST-008`, `PD-OBS-001`, and `PD-SEC-009`. It does not claim a usable
desktop, an installed runtime, a public C++ ABI, or a verified lower-bound tool
version.

## Observed environment

The hardened guest verifier completed with zero failures and zero warnings.
The guest had approximately 7.7 GiB memory and 6.2 GiB free on `/`; the source
was a clean committed checkout on read-write virtiofs, while every compiler
output directory was guest-local under `/var/tmp`.

| Tool or package | Observed version |
|---|---|
| CMake | 4.3.3-r1 |
| Ninja | 1.13.2-r1 |
| GCC | 15.3.0 |
| Clang | 22.1.8 |
| GoogleTest | 1.17.0 |
| Python | 3.14.6 |

The build declares CMake 3.24 as its configuration constraint, but that exact
lower bound has not been tested and is not recorded as a verified minimum.

## Compiler and test matrix

The following shape was run as the locked, unprivileged `prismdrake` guest
user for both `g++` and `clang++`, with separate build directories:

```sh
cmake -S . -B /var/tmp/prismdrake-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DPRISMDRAKE_WARNINGS_AS_ERRORS=ON
cmake --build /var/tmp/prismdrake-build --parallel 2
ctest --test-dir /var/tmp/prismdrake-build --output-on-failure
```

Both compilers configured and built the internal `prismdrake-foundation`
static library and its test executable with C++20 extensions disabled. All
three registered CTest cases passed for each compiler: the two GoogleTest
`BuildInfo` cases and the aggregate of 32 hermetic Gentoo VM-tool tests.
`format-check` also passed without modifying source.

## Explicit quality configurations

The reference VM successfully configured, built, and tested:

- AddressSanitizer plus UndefinedBehaviorSanitizer with Clang, including the
  ordinary leak-detection behavior;
- checked LTO with GCC;
- opt-in Clang-Tidy with Clang and the repository policy;
- warnings-as-errors with GCC and Clang;
- developer overrides enabled, proving the generated diagnostic bit follows
  the explicit option; and
- `BUILD_TESTING=OFF` while GoogleTest discovery was forcibly disabled.

An explicit `BUILD_TESTING=ON` configuration with
`CMAKE_DISABLE_FIND_PACKAGE_GTest=ON` failed as required and included the
project recovery message naming the system package and `BUILD_TESTING=OFF`
alternative. No dependency was downloaded or vendored.

The host repeated the compiler, formatting, LTO, Clang-Tidy, tests-off, and
sanitizer builds. Host LeakSanitizer initialization was blocked by the managed
sandbox's ptrace boundary, so the host-only sanitizer test used
`ASAN_OPTIONS=detect_leaks=0`. The guest then passed the same sanitizer tests
without that override; no project configuration disables leak detection.

## Archive behavior

A `git archive` of the checkpoint was extracted into `/var/tmp`, the absence
of `.git` was asserted, and the extracted tree configured, built, and passed
CTest. Product version generation therefore depends on the tracked `VERSION`
file rather than Git metadata or the project specification version.

## Remaining boundary

- GitHub Actions must still exercise this checkpoint after publication.
- CMake 3.24 and minimum compiler/test-framework versions remain declared or
  unresolved rather than verified lower bounds.
- No install target exists yet, so staged `DESTDIR` inspection belongs to the
  later product-target and ebuild work package.
- The foundation target has no Qt, X11, D-Bus, TOML, JSON, or installed runtime
  dependency. Those dependencies must be attached to the components that
  actually use them.
