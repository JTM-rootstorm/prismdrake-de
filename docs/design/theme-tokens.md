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

## Version-1 validation boundary

The display-free parser accepts complete JSON documents only. It rejects
comments, duplicate keys, malformed UTF-8, unknown keys, unsupported versions,
wrong layer identity, missing fixed tokens, non-finite or out-of-range values,
and invalid colors. JSON Schema `integer` fields use mathematical integer
semantics, so `400`, `400.0`, and `4e2` are equivalent when they are in range.

The operational input bounds are 1 MiB per document, 16 nested containers,
4096 total values and containers, 256 entries per object, 64 array items, and
256 Unicode code points per string. A SAX pass enforces allocation-sensitive
bounds and duplicate-key rejection before the typed document is constructed.
Diagnostics use canonical schema paths and logical packaged-source identifiers;
they do not reflect rejected keys, values, source excerpts, or filesystem paths.

Primitive map keys are lowercase `snake_case`, at most 64 characters, and each
map contains at most 256 entries. Primitive and layout metrics are bounded from
zero through 65535; focus width starts at 1 and target size starts at 24.
Font-family lists contain at most 64 bounded strings. Semantic metric groups
have exact version-1 keys:

- spacing: `unit_px`, `control_gap_px`, and `content_margin_px`;
- panel: `height_px` and `icon_size_px`;
- decoration: `titlebar_height_px` and `border_width_px`;
- icon: `small_px`, `medium_px`, and `large_px`;
- focus: `width_px` and `offset_px`; and
- targets: `minimum_px`.

Version 1 contains literal typed values. It has no token-reference expression or
asset-path field. An attempted path or asset key is therefore an unknown key;
the resolver does not invent reference or asset-loading semantics.

Layer identity is part of the schema: base and accessibility documents use null
profile identity, while profile documents use exactly `lustre` with
`Prismdrake Lustre` or `forge` with `Prismdrake Forge`. Reduced motion declares
zero duration and disabled transparency declares forced opaque fallback in
version 1. An `opaque` material fallback must also carry full color alpha and
opacity 1; an `alpha` fallback may retain its declared bounded opacity.

## Resolution and generation

Resolution is schema-directed rather than a generic recursive JSON merge:

1. Validate the complete base, Lustre, Forge, and accessibility documents and
   their exact identities.
2. Start primitive maps from the base and overlay selected-profile map entries
   by key. Replace the font-family list with the selected profile's list.
3. Replace each fixed semantic group and component style with the selected
   profile's complete group.
4. Apply high-contrast colors, materials, border and focus values selectively;
   retain profile geometry while raising component border widths. The resolver
   verifies opaque primary/muted text, selection, focus, active/inactive border,
   and status colors against panel and elevated surfaces at the declared
   effective contrast ratio. Selection, focus, border, and status colors must
   also remain pairwise distinct. Reduced motion, strong focus, and minimum
   target size remain independent controls.
5. Scale font sizes and durations with checked finite arithmetic. Semantic
   millisecond durations use round-to-nearest integer. Reduced motion forces
   every nonessential duration to zero.
6. Apply a user accent only to primitive `accent` and semantic `selection`.
   High contrast suppresses the accent with a stable warning so status and
   focus distinctions are not weakened.
7. Select each material's declared fallback when transparency is disabled,
   blur quality is off, or blur is unavailable. The packaged accessibility
   override forces both the resolved color alpha and opacity to full opacity
   when transparency is disabled. Missing thumbnails use the application icon,
   title, and state presentation.
8. Apply safe mode last: force opaque material fallbacks and zero motion.

The result is an immutable, generationless candidate containing schema and
profile identity, logical packaged sources, resolved primitive, semantic, and
component tokens, effective accessibility state, capability fallbacks,
resolved material presentation, thumbnail policy, and stable warnings. The
combined settings/theme publisher assigns one generation only after both
candidates validate and the complete bounded runtime payload serializes;
failed resolution or serialization neither mutates a prior candidate nor
advances publication. The runtime payload preserves every typed resolved theme
field and uses logical source identifiers rather than paths.

Logical source provenance includes the packaged accessibility layer whenever
high contrast, reduced motion, disabled transparency, or strong focus consumes
its declarations. Source paths and user paths are never published.

Lustre and Forge must declare identical semantic and component key sets. A
profile may change values but not remove a required state. User accessibility
preferences survive a profile change.

## Materials and effects

Each panel, launcher, notification, and menu material contains tint, opacity, a
blur request, and a complete non-blur fallback. The request describes intent;
the compositor chooses or rejects execution. Prismdrake never captures the
desktop to simulate blur. Disabled transparency selects fallback materials
without weakening focus, borders, text contrast, or state cues.
