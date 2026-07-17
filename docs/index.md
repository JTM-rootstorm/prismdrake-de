# Prismdrake documentation

Prismdrake is in PD1. Prototype implementation is active under the Accepted
architecture decisions and approved PD1 scope, but these documents do not
describe a usable or production-ready desktop release.

## Governing material

- [Project-wide product and technical specification](PRISMDRAKE_PROJECT_SPECIFICATION.md)
- [Architecture Decision Records](adr/README.md)
- [Milestones and PD1 status](roadmap/milestones.md)
- [Active PD1 milestone tracker](roadmap/pd1.md)

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
- [Dependency manifests](build/dependencies.md)
- [Foundation utilities](build/foundation-utilities.md)
- [Configuration](architecture/configuration.md)
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
- [Gentoo reference VM](development/gentoo-vm.md)
- [Gentoo local repository](packaging/gentoo-local-repository.md)

## Machine-readable contracts

- [Configuration examples](../examples/config/lustre.toml)
- [Capability examples](../examples/capabilities/x11-standard.json)
- [Experimental interfaces](../interfaces/README.md)
- [JSON schemas](../schemas/prismdrake-config.schema.json)
- [Experimental runtime snapshot schema](../schemas/prismdrake-runtime-snapshot.schema.json)
- [Dependency-manifest schema](../schemas/prismdrake-dependency-manifest.schema.json)
- [Foundation dependency manifest](../manifests/dependencies/prismdrake-foundation.json)
- [X11 dependency manifest](../manifests/dependencies/prismdrake-x11.json)
- [Theme token documents](../themes/base.tokens.json)

Run `make validate` from the repository root to check repository contracts and
local links.
