# ADR 0006: Shared theme token model

- **Status:** Proposed
- **Date:** 2026-07-15
- **Owners:** Prismdrake maintainers

## Context

Prismdrake Lustre and Prismdrake Forge need meaningfully different presentation
without separate shell implementations or half-applied profile state.
Accessibility preferences must override both profiles without becoming another
fork.

## Decision drivers

- One semantic source of truth and common component code.
- Strict, versioned, data-only themes.
- Atomic immutable snapshots.
- Non-blur and reduced-effects fallback for every material.
- First-class focus, contrast, motion, and target-size behavior.

## Considered options

1. Versioned JSON primitive, semantic, component, and accessibility token layers.
2. Profile-specific constants embedded in QML or C++.
3. Executable theme scripts or general plugins.

JSON is machine-validated and toolkit-neutral. Embedded constants cause profile
drift and mixed generations. Executable themes create an unnecessary code and
security boundary and are excluded for 1.0.

## Decision

Propose version-1 JSON token documents with shared primitive, semantic,
component, accessibility, and capability-fallback groups. Resolve base plus one
profile and user overrides into one immutable, generation-tagged snapshot.
Require semantic/component key parity between Lustre and Forge. Require every
blur-capable material to carry a non-blur fallback.

Themes are data-only. Unknown versions, missing keys, unsafe references, or
invalid values reject a candidate without replacing the prior valid generation.

## Consequences

The implementation needs a strict parser, resolver, generation publisher, and
negative tests. Profile values remain independently tunable while schema and
validator parity prevent structural forks. JSON is authoring-oriented rather
than a public runtime ABI.

## Validation or evidence

PD0 checks JSON parsing, supported versions, exact profile identity, semantic
and component parity, focus/contrast accessibility keys, and material fallbacks.
Static mockups demonstrate the intended relationship without claiming
production rendering.

## Revisit conditions

Revisit if a PD1 resolver cannot express a required accessible state or if
measured runtime needs justify a deterministically generated format. Any new
format must retain the JSON source contract or provide an explicit migration.

## References

- [Theme token contract](../design/theme-tokens.md)
- [Visual language](../design/visual-language.md)
- [Accessibility](../design/accessibility.md)
- [Project specification section 14](../PRISMDRAKE_PROJECT_SPECIFICATION.md#14-visual-system-and-profiles)
