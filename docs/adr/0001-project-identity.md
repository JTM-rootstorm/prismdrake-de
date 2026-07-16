# ADR 0001: Project identity

- **Status:** Accepted
- **Date:** 2026-07-15
- **Owners:** Prismdrake maintainers

## Context

PD0 must establish stable names before packages, processes, interfaces, and
visual assets create compatibility obligations. The project also needs a clear
identity separate from Glasswyrm and from proprietary desktop products that
inspired broad interaction qualities.

## Decision drivers

- Satisfy `PD-ID-001` through `PD-ID-010`.
- Keep user-facing and machine-facing names unambiguous.
- Preserve separation between Prismdrake and Glasswyrm ownership.
- Establish original, licensable profile identities.

## Considered options

1. Use the Prismdrake names locked by the maintainer and project specification.
2. Use informal abbreviations or historical product references.
3. Defer naming until implementation.

Only the first option provides an authoritative vocabulary without trademark,
namespace, or migration risk.

## Decision

The canonical product name is **Prismdrake Desktop Environment**, shortened to
**Prismdrake**. Its translucent profile is **Prismdrake Lustre** and its classic
profile is **Prismdrake Forge**. Their machine identifiers are `lustre` and
`forge`.

Packages and executables use `prismdrake-*`. Public D-Bus names use
`org.prismdrake.*`. Glasswyrm-native interfaces use generic `GW_*` names and do
not contain Prismdrake branding. The canonical repository is
`JTM-rootstorm/prismdrake-de`. The repository retains its committed GPL-3.0
license.

“Aero,” “Luna,” “Windows,” and other Microsoft product names are not Prismdrake
component, profile, theme, or feature names.

## Consequences

All documentation, schemas, packages, and interfaces must use these exact
names. New public names require maintainer approval and, when architectural, a
new Accepted ADR. Familiarity must be expressed through original Prismdrake
work rather than proprietary names or assets.

## Validation or evidence

The values are owner-locked by the project specification, section 4. PD0
validation checks canonical display names, profile identifiers, namespaces, and
the repository identity where structurally required.

## Revisit conditions

A legal conflict or explicit maintainer-directed rebrand would require a
coordinated specification, migration, and compatibility decision.

## References

- [`PD-ID-001` through `PD-ID-010`](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#4-canonical-identity)
- [Naming contract](../vision/naming.md)
- [Product vision](../vision/product.md)
