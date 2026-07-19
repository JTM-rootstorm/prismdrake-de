# Dependency manifests

Prismdrake records dependency intent in strict version-1 JSON manifests
under [`manifests/dependencies/`](../../manifests/dependencies/). The manifests
support `PD-DEP-001` through `PD-DEP-008`; they do not replace CMake discovery,
distribution metadata, license review, or measurements from built targets.

## Contract

[`prismdrake-dependency-manifest.schema.json`](../../schemas/prismdrake-dependency-manifest.schema.json)
defines the complete allowed shape. Unknown keys, missing fields, unsupported
schema versions, malformed component names, and malformed Gentoo atoms fail
repository validation.

Each component declares whether it is implemented, experimental, planned, or
repository foundation work. `runtime_dependency_state` separates measured
runtime metadata from `planned_unmeasured` intent and dependencies that are not
runtime-applicable. A planned component manifest is therefore a reviewable
boundary, not a claim that its target or package already exists.

Dependency entries distinguish:

- contract-validation, development, build, test, mandatory-runtime, and
  optional-runtime scope;
- tool, direct-link/use, and transitive relationships;
- observed, planned, and implemented requirements;
- supported minimum versions from versions merely observed in the reference
  environment;
- Gentoo packaging and license-review state; and
- optional-feature fallback behavior.

`version.declared_minimum` records a build or packaging constraint, while
`version.verified_minimum` is the oldest version actually covered by component
tests. They are intentionally separate: the foundation scaffold declares CMake
3.24, but only CMake 4.3.3-r1 is currently observed, so 3.24 is not presented as
a tested lower bound. `version.observed` records exact environment evidence
without turning that version into a minimum. An `observed_reference` value is
not equivalent to `verified_component`, and an `unverified` dependency must not
carry a verified or observed version.

## Current boundary

- `prismdrake-foundation` is an implemented internal C++ library. Its manifest
  records repository and build/test tooling, including system GoogleTest 1.17.0
  used by the foundation tests and the observed GCC 15.3.0 and Clang 22.1.8
  compiler packages. These entries are not installed runtime dependencies.
- The candidate visual-baseline harness requires the system fontconfig
  `fc-match` tool only when tests are enabled. It locks and records the generic
  theme font's resolved family and source for each configured test environment;
  fontconfig is not a Prismdrake runtime dependency.
- The PD1 performance-evidence harness reuses fontconfig and the existing Qt
  software-rendering test stack for deterministic cadence collection. Its
  startup collector uses test-only Xvfb, Openbox, `xev`, `xprop`, `stdbuf`, and
  `dbus-run-session`; the event endpoint does not poll either X11 or the runtime
  directory. The optional root-owned Gentoo VM wakeup procedure uses Linux
  `perf` scheduler tracepoints. These are measurement tools, not Prismdrake
  runtime dependencies, and a missing tracepoint blocks only that evidence.
- The PD2 WP14 live-observer lane adds test-only `x11-apps/xwininfo` for
  authoritative window-tree and geometry inspection, `x11-apps/xwd` for
  bounded private surface fingerprints, and `x11-misc/xtrace` for bounded X11
  protocol traces. Gentoo installs the protocol tracer as
  `/usr/bin/x11trace`; `/usr/bin/xtrace` belongs to glibc and traces dynamic
  loader activity, so it is not a substitute. Both atoms remain under the
  product ebuild's `test` dependency and the development metapackage's `x11`
  tooling layer. None is linked by or required to run Prismdrake.
- `prismdrake-settingsd` is an Experimental implemented service. Its boundary
  records direct system toml++ 3.4.0 configuration parsing, header-only
  nlohmann JSON 3.12.0-r1 theme/runtime serialization, and the sd-bus provider.
  Gentoo prefers and directly links basu 0.2.1; the retired historical Ubuntu
  lane used libsystemd as the API-compatible provider, and CMake selects exactly
  one. Current validation and milestone evidence come from local and Gentoo
  procedures rather than a hosted CI requirement. The D-Bus broker is a
  mandatory runtime service/tool rather than a directly linked libdbus client.
  The complete Gentoo dynamic closure is measured in the
  [settings-service evidence](../research/pd1-settings-service-evidence.md);
  supported provider and broker minima remain unmeasured.
- `prismdrake-x11` is an Experimental internal library. It directly links
  system core XCB 1.17.0 for transport, atoms, properties, EWMH discovery, and
  observational root events. A separate direct `xcb-randr` 1.17.0 link from
  the same Gentoo `x11-libs/libxcb` package supplies bounded RandR negotiation,
  output discovery, primary-output queries, and topology-change event
  selection through `libxcb-randr.so.0`. Xvfb 21.1.24 supplies isolated
  real-server tests. Optional test-only Openbox 3.6.1-r11 and xprop 1.2.8 add a
  bounded isolated-WM lane for protocol readiness and WM-applied work-area
  verification; neither is a runtime dependency. These observed versions are
  not supported minima. Checked standard window-manager requests and dock/strut
  publication are implemented. The bounded EWMH task mirror, metadata decoder,
  immutable model generations, stale-record removal, and observation-based
  request confirmation are also implemented without another link dependency.
  The bounded production task presentation now provides activation, minimize,
  and close actions. PD2 adds pin and grouping policy above the same
  window-manager-authoritative X11 boundary without another X11 dependency.
- `prismdrake-session` is an Experimental init-neutral supervisor. It directly
  uses the selected sd-bus provider for bounded settings and notification
  readiness and the internal X11 adapter for display readiness, then owns only
  the exact settingsd, notifyd, and shell children it launches. A dependency-free
  private Linux event-descriptor contract bounds post-exec readiness to each
  exact child and one complete panel presentation epoch. Gentoo selects basu;
  the development harness supplies isolated D-Bus lifecycle and restart tests.
  Supported provider and broker minima remain open. A standard X11 session entry
  and the five PD2 executables (`prismdrake-session`, `prismdrake-settingsd`,
  `prismdrake-notifyd`, `prismdrake-shell`, and `prismdrake-shellctl`) share one
  CMake install contract; their final installed closure remains a WP16 evidence
  result rather than a source-tree claim.
- `prismdrake-shell` is Experimental. Its immutable settings/theme projection
  directly uses Qt Core and GUI; its asynchronous launcher controller, passive
  launcher and notification adapters, and actionable task adapter use Qt Core;
  and its
  compiled panel, launcher, and notification modules use QML, Quick, and Quick
  Controls. Its icon resolver directly uses system Qt SVG for bounded raster
  and SVG icon loading; the product dependency retains the Qt 6.11 floor.
  The shell's expanded-state accessibility adapter subclasses Qt Quick's
  complete accessible-item implementation because Qt 6.11 does not expose the
  `expanded`, `collapsed`, or `expandable` state bits through the public QML
  `Accessible` attachment. This narrow adapter uses the matching system Qt
  Quick private development interface; it does not pin Prismdrake to Qt 6.11.1,
  but Prismdrake must be rebuilt against the installed `qtdeclarative` build
  when that private ABI changes. The Gentoo product ebuild records that rebuild
  relationship with the `:6=` slot operator.
  System Qt Linguist tools compile committed long-string and right-to-left
  pseudo-locale catalogs at build time. The resulting QM files are installed as
  shell data; qttools is not a runtime dependency. A missing catalog fails soft
  to source strings, and the loader accepts only bounded language tags beneath
  its trusted packaged directory.
  The panel window host and event-driven task controller use Qt GUI/Core and the
  internal standards-only X11 adapter; Quick Test is test-only. The live
  snapshot client directly uses the selected sd-bus provider and session D-Bus,
  while its canonical inverse parser includes the
  system nlohmann JSON header. Qt 6.11 is the maintainer-selected declared
  minimum. The Gentoo reference environment verifies Qt 6.11.1 and supplies
  qtdeclarative 6.11.1-r1. Older hosted Qt versions do not provide product
  compatibility evidence. The Experimental shell executable composes those
  existing typed boundaries without another mandatory library dependency.
  Its installed dynamic links, package ownership, AT-SPI behavior, unmerge, and
  ordinary reinstall are measured on Gentoo. Exact host and VM coverage is
  recorded in the [panel-shell evidence](../research/pd1-panel-shell-evidence.md)
  and [package lifecycle evidence](../research/pd1-portage-lifecycle-evidence.md).
- `prismdrake-notifyd` is an Experimental toolkit-neutral core service. Its
  bounded history and settings payload handling includes the system nlohmann
  JSON header, and its freedesktop Notifications and Experimental
  NotificationCenter1 services directly use the selected sd-bus provider and
  user session D-Bus. It does not require libnotify. Its final installed PD2
  runtime closure remains unmeasured.
- `prismdrake-shellctl` is an Experimental non-resident toolkit-neutral client.
  Its closed Shell1 and Session1 command grammar directly uses the selected
  sd-bus provider and user session D-Bus without a GUI toolkit. Its final
  installed PD2 runtime closure remains unmeasured.
- The settings service uses a mutex-protected immutable snapshot pointer and a
  single bounded worker; it introduced no atomic-storage dependency. Its
  installed Gentoo boundary is measured, while supported lower provider and
  broker versions remain explicitly unresolved rather than invented claims.

Mandatory core runtime entries may not name GNOME Shell, Mutter,
`gnome-settings-daemon`, `gnome-control-center`, or libadwaita. GTK itself is
not forbidden, and separately packaged optional adapters retain their own
review boundary. Validation also rejects duplicate entries, verified minimum
versions without component evidence, reference evidence without an observed
version, and optional runtime dependencies without a fallback.

## Updating a manifest

Update the consuming component manifest in the same change as its build,
packaging, and documentation boundary. Record exact evidence in the appropriate
research or build report, then run:

```sh
make validate
```

Do not infer a dependency from an installed package or transitive library alone.
Only record a mandatory runtime dependency when project code or an Accepted
component contract directly requires it. Keep `verified_minimum` unset until
the supported range is tested, and label declared but unverified constraints
separately.
