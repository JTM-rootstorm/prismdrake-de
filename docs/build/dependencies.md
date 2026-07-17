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
- `prismdrake-settingsd` is an Experimental implemented service. Its boundary
  records direct system toml++ 3.4.0 configuration parsing, header-only
  nlohmann JSON 3.12.0-r1 theme/runtime serialization, and the sd-bus provider.
  Gentoo prefers and directly links basu 0.2.1; Ubuntu CI uses libsystemd as the
  API-compatible provider, and CMake selects exactly one. The D-Bus broker is a
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
  The production task-strip UI remains unresolved PD1 scope.
- `prismdrake-session` remains a planned component manifest.
  `prismdrake-shell` is Experimental. Its immutable settings/theme projection
  directly uses Qt Core and GUI; its passive launcher, task, and notification
  adapters use Qt Core; and its compiled panel and notification modules use QML,
  Quick, and Quick Controls. The panel window host uses Qt GUI and the internal
  standards-only X11 adapter; Quick Test is test-only. Ubuntu 24.04 CI verifies
  Qt 6.4.2 as the oldest tested common-API component version, while Qt 6.11.1 is
  the currently observed host component version and Gentoo supplies
  qtdeclarative 6.11.1-r1. The shell executable, live settings-snapshot client,
  accessibility runtime closure, and complete dynamic dependency graph remain
  explicitly unresolved. Exact host and VM coverage is recorded in the
  [panel-shell evidence](../research/pd1-panel-shell-evidence.md).
- The settings service uses a mutex-protected immutable snapshot pointer and a
  single bounded worker; it introduced no atomic-storage dependency. Production
  runtime-closure evidence, accessibility linkage, and the supervisor
  executable remain explicit unresolved areas rather than invented package
  claims.

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
