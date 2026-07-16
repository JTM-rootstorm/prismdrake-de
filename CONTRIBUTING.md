# Contributing to Prismdrake

Prismdrake is in PD0. Contributions should strengthen identity, architecture,
schemas, interfaces, design contracts, validation, or bounded roadmap work.
Production desktop implementation belongs to a later milestone after the
relevant Proposed decisions receive maintainer approval.

## Before making a change

Read, in order:

1. [`AGENTS.md`](AGENTS.md) for repository-wide working rules.
2. The [project specification](Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md).
3. The [documentation index](docs/index.md).
4. Relevant Accepted ADRs, contracts, and current milestone criteria.

Keep changes narrow. Cite affected `PD-*` requirement identifiers in commits or
pull requests when practical. Do not turn Proposed choices into settled facts.

## Decision status

- **Proposed** decisions are under review and are not authoritative.
- **Accepted** decisions are approved and authoritative.
- **Rejected** decisions were evaluated and intentionally not selected.
- **Superseded** decisions were replaced by a named newer ADR.
- **Deprecated** decisions remain for history but are no longer recommended.

Material changes to ownership, dependencies, public interfaces, security
boundaries, or product identity require an ADR and a coherent specification
update.

## Validation

Run the complete PD0 contract suite before requesting review:

```sh
make validate
```

The command must pass from a clean checkout. If a relevant check cannot run,
record the exact command, error, and environmental limitation.

## Dependencies and interfaces

Every new mandatory runtime dependency needs justification and component
ownership. Core operation must not require GNOME Shell, Mutter,
`gnome-settings-daemon`, `gnome-control-center`, libadwaita, both GTK and Qt, or
systemd as the sole supervisor. Draft D-Bus and `GW_*` material must remain
clearly labeled and versioned.

## Asset provenance and originality

Every contributed visual, sound, font, wallpaper, cursor, or icon must be
original or have a compatible license. Include its author, source, license,
required attribution, and modifications in the same change. Do not contribute
Microsoft-provided assets or recreate a proprietary interface pixel for pixel.
Code-generated geometry is still subject to originality review.

Before adding an asset, confirm:

- its source and author are known;
- its license permits repository distribution and modification;
- required notices are included;
- no proprietary font binary is committed without explicit license review; and
- its design is recognizably Prismdrake rather than a traced or copied work.

If provenance is uncertain, remove or replace the asset with an original,
generic placeholder and record the reason in the review description.

## Pull requests

Describe scope, requirements, decision status, dependency impact,
accessibility, security and privacy, standards and Glasswyrm behavior,
fallbacks, and exact validation results. Do not mix unrelated cleanup with a
contract or feature change.

