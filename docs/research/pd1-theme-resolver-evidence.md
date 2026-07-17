# PD1 theme-resolver evidence

**Date:** 2026-07-16

**Reference environment:** `prismdrake-vm` Gentoo guest

**Source revision:** `d6bd51b`

## Scope

This report records validation of the display-free PD1 version-1 JSON theme
parser and schema-directed immutable resolver. The implementation supplies
measured evidence for `PD1-007` and supports `PD-CONFIG-001`, `PD-CONFIG-002`,
`PD-CONFIG-004`, `PD-A11Y-002`, `PD-A11Y-004` through `PD-A11Y-006`,
`PD-A11Y-008`, `PD-SEC-001`, `PD-SEC-005`, `PD-SEC-008`, `PD-SEC-012`,
`PD-GW-003`, `PD-GW-004`, and `PD-GW-008` at the theme-candidate boundary.

This slice does not publish a generation. Atomic combined settings/theme
generation assignment, service reload retention, and proof that a profile
switch cannot expose mixed generations remain work for `PD1-008` in WP6. It
also does not claim rendered wallpaper contrast or production UI accessibility.

## Dependency selection

The parser uses system nlohmann JSON through its CMake package configuration.
The Gentoo guest supplied `dev-cpp/nlohmann_json-3.12.0-r1`, with pkg-config
reporting 3.12.0. The dependency is header-only: runtime linkage inspection of
the complete test executable showed no nlohmann JSON shared library.

CMake declares 3.11 as the unverified minimum. A build configured with
`CMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON` failed with actionable Gentoo
package guidance. No configure or build step downloaded or vendored source.

## Contract and parser behavior

The parser performs a SAX preflight before DOM construction. It rejects
duplicate keys, malformed or non-UTF-8 JSON, comments, non-finite numbers,
unsupported versions, wrong layer/profile identity, unknown keys, incomplete
fixed groups, unsafe asset-like fields, invalid colors, invalid material
fallbacks, invisible focus, undersized targets, excessive numeric values, and
documents outside the published byte, nesting, node, object, array, and Unicode
limits.

Version-1 token values remain data-only. There is no executable theme behavior,
asset-path field, or reference-expression language. Diagnostics contain stable
canonical schema paths and recovery guidance without reflecting rejected keys,
values, source excerpts, user paths, or secret-like input.

The repository validator now applies strict JSON loading, mathematical JSON
integer semantics, the schema's `oneOf`, property-name and map-size limits, and
27 negative self-test paths. Runtime parser tests separately cover exact
boundaries and allocation-sensitive adversarial documents.

## Resolution behavior

The resolver uses the documented mixed overlay:

- Base primitive maps initialize the candidate and profile entries overlay by
  key; the profile font list replaces the base list.
- Complete selected-profile semantic groups and component styles replace the
  corresponding base groups.
- High contrast selectively applies packaged colors, materials, borders,
  focus, and component-border emphasis without replacing profile geometry.
- Reduced motion, disabled transparency, strong focus, text scale, animation
  scale, and minimum target size remain independent across profile changes.
- Text, selection, focus, border, and status colors in high-contrast mode must
  be opaque, state-distinct, and meet the effective declared ratio against both
  panel and elevated surfaces.
- Disabled transparency and safe mode force both fallback color alpha and
  separate opacity to full opacity. Missing blur preserves a declared alpha
  fallback when opacity is allowed.
- Missing thumbnails select the stable application-icon, title, and state
  presentation. Prismdrake records blur intent but never executes or simulates
  compositor blur.
- Safe mode applies after ordinary configuration and capability resolution.

The output is a complete immutable generationless candidate with logical
packaged-source provenance and deterministic warning order. Failed resolution
does not mutate a previously retained candidate.

## Gentoo guest validation

Guest-local source and build directories under `/var/tmp` used revision
`d6bd51b`; compilation did not run on the shared mount.

- GCC 15.3.0 Debug with warnings as errors built successfully. All 96 CTest
  registrations ran; 95 passed and the root-inapplicable permission-denied case
  reported an explicit skip. That exact case then passed separately as the
  unprivileged `nobody` user. The root-only foreign-owner protection passed in
  the main suite.
- Clang 22.1.8 with warnings as errors and Clang-Tidy built successfully. The
  same 96-test result passed with no static-analysis finding.
- A separate Clang 22.1.8 ASan and UBSan build passed the suite with
  `ASAN_OPTIONS=detect_leaks=1` and stack-trace-enabled UBSan.
- GCC release builds passed with link-time optimization enabled and with
  `BUILD_TESTING=OFF`.
- `format-check`, `make validate`, the missing-dependency diagnostic, and
  runtime linkage inspection passed.

These checks validate the parser/resolver boundary and its deterministic
fallback behavior. Combined generation publication and live consumer behavior
remain intentionally deferred to WP6 and later integration work.
