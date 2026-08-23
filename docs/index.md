# Prismdrake documentation

Prismdrake completed PD1 on the Gentoo reference environment and entered
**PD2: Core desktop shell** on 2026-07-18. The tree contains an integrated
Experimental PD2 core-shell implementation, while its final installed Gentoo
exit record is still pending. These documents do not describe a
production-ready desktop release or stable public interfaces.

## Governing material

- [Project-wide product and technical specification](PRISMDRAKE_PROJECT_SPECIFICATION.md)
- [Architecture Decision Records](adr/README.md)
- [Rust-first transition decision](adr/0010-rust-first-transition.md)
- [Milestones and current status](roadmap/milestones.md)
- [Active PD2 milestone tracker](roadmap/pd2.md)
- [Completed PD1 milestone tracker](roadmap/pd1.md)

## Product and design

- [Product vision](vision/product.md)
- [Naming contract](vision/naming.md)
- [Design principles](vision/design-principles.md)
- [Non-goals](vision/non-goals.md)
- [Visual language](design/visual-language.md)
- [Theme tokens](design/theme-tokens.md)
- [Accessibility](design/accessibility.md)

## Architecture and integration

- [Architecture overview](architecture/overview.md)
- [Component model](architecture/component-model.md)
- [Process model](architecture/process-model.md)
- [Dependency policy](architecture/dependency-policy.md)
- [Rust-first transition baseline](architecture/rust-first-transition-baseline.md)
- [Dependency manifests](build/dependencies.md)
- [Rust toolchain and workspace proof](build/rust-toolchain.md)
- [Foundation utilities](build/foundation-utilities.md)
- [Configuration version 2](architecture/configuration.md)
- [PD2 state ownership](architecture/pd2-state-ownership.md)
- [Application identity and pin state](architecture/application-identity.md)
- [Notification service and history](architecture/notification-service.md)
- [Surface-placement policy](architecture/surface-placement.md)
- [Shell and session control](architecture/shell-control.md)
- [Compatibility matrix](architecture/compatibility.md)
- [Glasswyrm integration](architecture/glasswyrm-integration.md)
- [Failure behavior and fallbacks](architecture/failure-and-fallbacks.md)
- [Toolkit evaluation](research/toolkit-evaluation.md)
- [PD1 toolkit spike](research/pd1-toolkit-spike.md)
- [PD1 Gentoo dependency evidence](research/pd1-gentoo-dependency-evidence.md)
- [PD1 build and toolchain evidence](research/pd1-build-toolchain-evidence.md)
- [PD1 foundation-utilities evidence](research/pd1-foundation-utilities-evidence.md)
- [PD1 configuration-loader evidence](research/pd1-configuration-loader-evidence.md)
- [PD1 theme-resolver evidence](research/pd1-theme-resolver-evidence.md)
- [PD1 settings-service evidence](research/pd1-settings-service-evidence.md)
- [PD1 X11 connection evidence](research/pd1-x11-connection-evidence.md)
- [PD1 X11 capability and event evidence](research/pd1-x11-capability-event-evidence.md)
- [PD1 X11 output topology evidence](research/pd1-x11-output-topology-evidence.md)
- [PD1 X11 dock and window-manager request evidence](research/pd1-x11-dock-request-evidence.md)
- [PD1 X11 task-model evidence](research/pd1-x11-task-model-evidence.md)
- [PD1 launcher-model evidence](research/pd1-launcher-model-evidence.md)
- [PD1 synthetic notification-presentation evidence](research/pd1-notification-presentation-evidence.md)
- [PD1 panel-shell evidence](research/pd1-panel-shell-evidence.md)
- [PD1 installed-shell accessibility evidence](research/pd1-accessibility-evidence.md)
- [PD1 performance-evidence method](research/pd1-performance-evidence-method.md)
- [PD2 performance and accessibility evidence contract](research/pd2-performance-accessibility-evidence-contract.md)
- [PD2 core-shell implementation status](architecture/pd2-core-shell.md)
- [PD1 Gentoo package lifecycle evidence](research/pd1-portage-lifecycle-evidence.md)
- [Gentoo reference VM](development/gentoo-vm.md)
- [Gentoo local repository](packaging/gentoo-local-repository.md)

## Testing

- [Validation tiers and selectors](build/validation-tiers.md)
- [Validation-record evidence envelope](build/evidence-records.md)
- [Build and test commands](build/testing.md)
- [PD1 X11 development harness](development/pd1-x11-harness.md)
- [PD2 installed multi-output demonstration harness](development/pd2-installed-demonstration.md)
- [PD1 diagnostics](development/diagnostics.md)
- [Live accessibility testing](development/accessibility-testing.md)
- [Deterministic visual baseline candidates](testing/visual-baselines.md)

## Machine-readable contracts

- [Configuration examples](../examples/config/lustre.toml)
- [Capability examples](../examples/capabilities/x11-standard.json)
- [Experimental interfaces](../interfaces/README.md)
- [JSON schemas](../schemas/prismdrake-config.schema.json)
- [Experimental runtime snapshot schema](../schemas/prismdrake-runtime-snapshot.schema.json)
- [Pin-state schema](../schemas/prismdrake-pin-state.schema.json)
- [Notification-history schema](../schemas/prismdrake-notification-history.schema.json)
- [Notification-state schema](../schemas/prismdrake-notification-state.schema.json)
- [Dependency-manifest schema](../schemas/prismdrake-dependency-manifest.schema.json)
- [Validation-record envelope schema](../schemas/prismdrake-validation-record.schema.json)
- [Foundation dependency manifest](../manifests/dependencies/prismdrake-foundation.json)
- [Settings service dependency manifest](../manifests/dependencies/prismdrake-settingsd.json)
- [Session dependency manifest](../manifests/dependencies/prismdrake-session.json)
- [Shell dependency manifest](../manifests/dependencies/prismdrake-shell.json)
- [Notification service dependency manifest](../manifests/dependencies/prismdrake-notifyd.json)
- [Shell control dependency manifest](../manifests/dependencies/prismdrake-shellctl.json)
- [X11 dependency manifest](../manifests/dependencies/prismdrake-x11.json)
- [Theme token documents](../themes/base.tokens.json)

Run `make validate` from the repository root to check repository contracts and
local links.
