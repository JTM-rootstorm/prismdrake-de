# ADR 0007: Licensing and original asset policy

- **Status:** Accepted
- **Date:** 2026-07-15
- **Owners:** Prismdrake maintainers

## Context

Prismdrake draws on familiar desktop interaction qualities while requiring a
distinct identity and legally distributable source. PD0 mockups and later
themes need an enforceable boundary between inspiration and copying.

## Decision drivers

- Preserve `PD-ID-010` and the committed GPL-3.0 license.
- Build an original Prismdrake visual and auditory identity.
- Keep provenance and attribution reviewable from the repository.
- Avoid proprietary assets, fonts, and confusing product affiliation.

## Considered options

1. Original Prismdrake assets plus compatibly licensed third-party work with
   recorded provenance.
2. Reuse assets or traced geometry from proprietary desktop products.
3. Ship early assets without records and audit them near release.

Only the first option provides a reliable licensing and originality boundary.
Delayed audit makes replacement expensive; generated or traced copies remain
copies even when the file itself is newly created.

## Decision

Repository code remains GPL-3.0 under the committed [`LICENSE`](../../LICENSE).
Prismdrake visual assets are original work unless a compatible third-party
license and complete provenance record accompany them.

Do not commit Microsoft-provided icons, sounds, wallpapers, fonts, logos, or
copied UI artwork. Do not reproduce proprietary interfaces pixel for pixel
through hand drawing, tracing, generated imagery, or code-generated geometry.
Do not commit font binaries without explicit license review.

Third-party assets must record author, source, license, required attribution,
and modifications in the same change. Static mockups use generic names,
abstract original glyphs, and generic font-family declarations. Themes remain
data-only for 1.0 unless an Accepted security design changes that boundary.

## Consequences

Asset review includes provenance and visual originality. A questionable asset
is removed or replaced rather than retained pending later research. Required
license notices ship with the asset. Product descriptions avoid proprietary
profile or feature names and claims of being an implementation of another
desktop design.

## Validation or evidence

PD0 validation checks that mockups are editable SVG, use only generic font
families, and contain no linked font binaries. Contributor guidance provides a
provenance checklist. The existing license file is unchanged.

## Revisit conditions

The maintainer may approve a specific third-party asset after documented legal
and originality review. Repository license changes require explicit maintainer
direction and a project-wide compatibility assessment.

## References

- [Contributing: asset provenance and originality](../../CONTRIBUTING.md#asset-provenance-and-originality)
- [Visual language](../design/visual-language.md)
- [`PD-ID-010`](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#4-canonical-identity)
- [`PD-SCOPE-004`](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#62-core-10-non-goals)

