# Theme token contract

Theme data is governed by Accepted
[ADR 0006](../adr/0006-theme-token-model.md). The
[version-1 schema](../../schemas/prismdrake-theme-tokens.schema.json) defines a
common shape for base, profile, and accessibility layers.

## Layers

1. **Primitive tokens** contain raw colors, spacing, font families and sizes,
   durations, radii, and opacity steps.
2. **Semantic tokens** name meaning: panel/elevated/window surfaces, borders,
   text, selection, focus, status, materials, typography, motion, metrics, and
   target sizes.
3. **Component tokens** specialize task buttons, launcher tiles, title-bar
   buttons, notification cards, quick settings, tooltips, and menu items.
4. **Accessibility overrides** remain independent of profile identity and are
   applied after the selected profile.

The committed layers are:

- [`base.tokens.json`](../../themes/base.tokens.json): shared defaults;
- [`lustre.tokens.json`](../../themes/lustre.tokens.json): Prismdrake Lustre;
- [`forge.tokens.json`](../../themes/forge.tokens.json): Prismdrake Forge; and
- [`accessibility.tokens.json`](../../themes/accessibility.tokens.json): opaque,
  reduced-motion, high-contrast reference overrides.

## Resolution and generation

The PD1 resolver must validate every source; merge base, one profile, and user
accessibility overrides; resolve references; and emit an immutable
snapshot containing schema version, profile identity, generation, source files,
resolved primitive/semantic/component tokens, overrides, and capability
fallbacks. Unsupported versions, missing keys, invalid values, or unresolved
fallbacks reject the candidate before publication.

Lustre and Forge must declare identical semantic and component key sets. A
profile may change values but not remove a required state. User accessibility
preferences survive a profile change.

## Materials and effects

Each panel, launcher, notification, and menu material contains tint, opacity, a
blur request, and a complete non-blur fallback. The request describes intent;
the compositor chooses or rejects execution. Prismdrake never captures the
desktop to simulate blur. Disabled transparency selects fallback materials
without weakening focus, borders, text contrast, or state cues.
