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
- [Configuration](architecture/configuration.md)
- [Compatibility matrix](architecture/compatibility.md)
- [Glasswyrm integration](architecture/glasswyrm-integration.md)
- [Failure behavior and fallbacks](architecture/failure-and-fallbacks.md)
- [Toolkit evaluation](research/toolkit-evaluation.md)
- [PD1 toolkit spike](research/pd1-toolkit-spike.md)
- [PD1 Gentoo dependency evidence](research/pd1-gentoo-dependency-evidence.md)
- [Gentoo reference VM](development/gentoo-vm.md)
- [Gentoo local repository](packaging/gentoo-local-repository.md)

## Machine-readable contracts

- [Configuration examples](../examples/config/lustre.toml)
- [Capability examples](../examples/capabilities/x11-standard.json)
- [Draft interfaces](../interfaces/README.md)
- [JSON schemas](../schemas/prismdrake-config.schema.json)
- [Theme token documents](../themes/base.tokens.json)

Run `make validate` from the repository root to check repository contracts and
local links.
