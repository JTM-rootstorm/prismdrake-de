# ADR 0008: Build, language, and testing baseline

- **Status:** Proposed
- **Date:** 2026-07-16
- **Owners:** Prismdrake maintainers

## Context

PD1 needs a reproducible compiled-code baseline before production targets are
created. The repository currently has contract validation but no accepted build
generator, C++ standard, test framework, compiler policy, installation model, or
boundary rule for exceptions. Those choices must be explicit so a developer's
installed packages do not silently select product features or create hidden
runtime dependencies.

This record decides only the build, language, and testing baseline. It does not
accept Qt 6 Quick or any other visible-shell toolkit; that decision remains with
[ADR 0003](0003-shell-toolkit.md) after its required evidence spike.

## Decision drivers

- Reproducible GCC and Clang builds on supported Linux environments.
- Display-free unit tests and first-class integration with repository CI.
- System-packaged dependencies with no network downloads during configure or
  build.
- Explicit optional features and actionable dependency failures.
- Reviewable installation behavior for prototype artifacts.
- Clear error conversion at process, C, X11, and D-Bus boundaries.
- Tooling that is available in the Gentoo reference environment and practical
  for distribution packaging.

## Considered options

1. CMake, CTest, C++20, and system-packaged GoogleTest.
2. Meson, its test runner, C++20, and a system-packaged C++ test framework.
3. A handwritten Make build with standalone test executables.
4. CMake with dependencies downloaded through `FetchContent` or an equivalent
   configure-time mechanism.

CMake and CTest align with the project specification's leading proposal, are
well supported by Linux distribution packaging, and provide target-scoped
dependency and test integration. Meson is credible but would diverge from that
proposal without a demonstrated project benefit. A handwritten build would
make feature detection, installation, and multi-compiler testing unnecessarily
project-specific. Configure-time dependency downloads undermine reproducible
and offline distribution builds.

GoogleTest is proposed for PD1 because it provides mature assertions, fixtures,
parameterized tests, and direct CTest discovery. It must be supplied by the
system package manager. A header-only or vendored framework would reduce one
package dependency but would add source ownership, update, and license-tracking
work to the repository.

## Decision

Propose CMake as the build generator and CTest as the canonical compiled-test
runner. Retain `make validate` as the contract-validation entry point and expose
an equivalent CMake/CTest path once compiled targets exist; neither path may
silently omit the other's required checks.

Use C++20 for Prismdrake C++ targets. Set `CXX_STANDARD 20`,
`CXX_STANDARD_REQUIRED YES`, and `CXX_EXTENSIONS NO` on a project-owned interface
target so the policy is explicit and consistently inherited. Do not rely on a
compiler's default language mode.

Support GCC and Clang on Linux. The Gentoo reference build uses its reviewed
system compiler, while CI must configure, compile, and run display-free tests
with both compiler families before PD1 exits. Minimum supported compiler and
CMake versions must be based on tested package evidence and recorded in build
and dependency documentation; configuration must fail with an actionable
message when the selected toolchain lacks required C++20 behavior.

Use system-packaged GoogleTest for C++ unit tests and register tests with CTest,
using CMake's GoogleTest integration where discovery is appropriate. Test code
and GoogleTest must not be linked into installed runtime targets. Configure must
report a clear missing-dependency error when tests are explicitly enabled and
GoogleTest is unavailable. The build must not download or silently vendor a test
framework.

Formatting uses a repository-pinned `.clang-format` policy once implementation
code is introduced. A formatting check must be available without rewriting
files. Clang-Tidy is an explicit developer or CI feature, disabled by default
for ordinary builds, and applies only to project-owned targets. Tool versions
used for authoritative CI formatting or analysis must be recorded so results
are reproducible.

Project-owned C++ targets enable a common warning baseline for GCC and Clang:
`-Wall`, `-Wextra`, and `-Wpedantic`. Additional warnings are added only after
the existing tree is reviewed for signal and portability. Warnings-as-errors is
an explicit CI option, not an unconditional release or downstream-packaging
policy, and never applies to third-party headers or imported targets.

Provide opt-in AddressSanitizer plus UndefinedBehaviorSanitizer builds for
supported GCC and Clang debug configurations. Sanitizer flags remain
target-scoped, apply consistently at compile and link time, and are never added
to normal installed artifacts. ThreadSanitizer and other specialized tools may
be added separately after their environment and incompatibilities are tested.

Discover dependencies through CMake package configuration, standard CMake find
modules, or `pkg-config` as appropriate to the upstream package. Every optional
feature has a documented cache option and deterministic default. A feature must
not turn on merely because an unrelated package happens to be installed.
Required dependencies fail configuration with the component, feature, expected
form, and recovery action. Network fetching and hidden vendoring are prohibited
for normal builds.

Use `GNUInstallDirs` and relative, prefix-aware destinations for installable
prototype artifacts. Installation must support `DESTDIR`, must not write user
configuration, and must not require root during compilation or testing. The
isolated toolkit spike remains non-installed. Production prototype targets are
installable only after the applicable activation gates are approved, and PD1
does not promise a stable public C++ ABI.

Generate version headers and diagnostic version strings at configure time from
one tracked root `VERSION` file. The product version is independent of the
project specification version and interface schema versions. Optional source
revision metadata may augment developer diagnostics when available, but builds
from release archives must remain valid without Git and reproducible builds must
be able to omit non-deterministic metadata.

C++ exceptions are permitted within an implementation target; this proposal
does not require `-fno-exceptions`. Expected operational failures should use
explicit result types or other checked domain results where practical. No
exception may cross a process entry point, C ABI, X11 callback, D-Bus handler,
or other foreign-code boundary. Boundary adapters must catch internal and
third-party exceptions, preserve actionable redacted context, and convert them
to typed D-Bus errors, C-compatible status values, non-fatal X11 diagnostics, or
defined process exit status as appropriate. Destructors and cleanup paths must
not emit exceptions across those boundaries.

## Consequences

The project gains one canonical compiled build and test path, explicit compiler
semantics, distribution-friendly dependency discovery, and opt-in quality
tooling. Supporting two compiler families and maintaining formatter, analyzer,
and sanitizer paths adds CI work. System-packaged GoogleTest adds a test-only
dependency but keeps third-party source and updates outside the Prismdrake tree.

Feature options, warnings, sanitizer flags, installation destinations, and
version generation require focused CMake helpers rather than directory-global
flags. Package and CI evidence must establish tested minimum tool versions; this
ADR intentionally does not invent unmeasured compatibility claims.

Accepting this ADR would not accept ADR 0003, make Qt a dependency, activate
production PD1 scaffolding by itself, or stabilize a public C++ ABI.

## Validation or evidence

Before acceptance, validate the proposal in the Gentoo reference VM by:

- configuring and compiling a representative C++20 target with GCC and Clang;
- running GoogleTest through CTest from the system package;
- proving test-disabled configuration does not require GoogleTest;
- exercising formatting check, opt-in Clang-Tidy, warnings-as-errors, and
  AddressSanitizer plus UndefinedBehaviorSanitizer configurations;
- configuring with a required dependency missing and reviewing the diagnostic;
- installing into a staged `DESTDIR` and inspecting the result; and
- building from a source archive without Git metadata.

Record exact tool and package versions, commands, results, and every unsupported
or skipped path. Contract validation must remain green throughout.

## Revisit conditions

Revisit if Gentoo package evidence shows that the proposed tools cannot satisfy
the required compiler, test, sanitizer, or offline-build behavior; if a second
supported platform requires a different generator contract; or if measured
maintenance cost materially favors another system-packaged test framework.

Revisit version generation before the first release artifact and public ABI
policy before installing public C++ headers or promising binary compatibility.

## References

- [Project specification section 12](../PRISMDRAKE_PROJECT_SPECIFICATION.md#12-technology-and-dependency-policy)
- [Project specification testing requirements](../PRISMDRAKE_PROJECT_SPECIFICATION.md#25-testing-and-continuous-integration)
- [Dependency policy](../architecture/dependency-policy.md)
- [ADR 0003: Visible shell toolkit and language direction](0003-shell-toolkit.md)
- [PD1 candidate backlog](../roadmap/pd1.md)
