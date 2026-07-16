# ADR 0005: Standards baseline and Glasswyrm enhancements

- **Status:** Proposed
- **Date:** 2026-07-15
- **Owners:** Prismdrake maintainers

## Context

Prismdrake should work especially well with Glasswyrm without making private
capabilities the only path for fundamental desktop behavior or moving WM and
compositor ownership into the shell.

## Decision drivers

- A functional standards-capable X11 baseline.
- Explicit capability and version negotiation.
- Safe fallbacks for effects, thumbnails, workspaces, and decorations.
- Generic reusable Glasswyrm naming.
- Privacy review before capture or secure-lock features.

## Considered options

1. Standards baseline plus optional versioned native enhancements.
2. Require Glasswyrm and private Prismdrake-specific interfaces.
3. Use standards only and exclude compositor-native effects.

The second option prevents Tier B operation and creates ownership coupling. The
third cannot express reliable compositor blur or thumbnails. The first keeps
portable behavior while permitting separately reviewed enhancements.

## Decision

Propose the baseline, negotiation, and fallback model in the
[Glasswyrm integration contract](../architecture/glasswyrm-integration.md).
ICCCM, EWMH, XDG, D-Bus, XSettings, and freedesktop contracts provide suitable
baseline behavior. Optional native interfaces are explicit and versioned.

The generic `GW_*` family rule is already Accepted by owner-locked `PD-ID-008`.
Candidate names including `GW_SHELL_ROLE_V1`, `GW_BACKDROP_V1`,
`GW_WINDOW_THUMBNAIL_V1`, `GW_WORKSPACE_V1`, and `GW_DECORATION_V1` remain
Proposed placeholders; this ADR does not accept a wire protocol.

Glasswyrm remains authoritative for window state and effect execution.
Prismdrake sends intent and geometry. The compositor performs blur; shell-side
desktop screenshot blur is prohibited.

## Consequences

Every native feature needs a standards or reduced-feature path and lifecycle
handling for disappearance or version mismatch. Capability snapshots become
untrusted versioned input. Thumbnail work requires separate authorization,
privacy, lock-state, and revocation design.

## Validation or evidence

PD0 validates structural capability examples, generic versioned native names,
exact fallback keys, and the separation between examples and protocol packets.
No protocol implementation exists.

## Revisit conditions

Revisit detailed candidates when Glasswyrm reviews transport and semantics, or
when standards research provides an adequate portable equivalent. Update both
projects before any candidate becomes an implemented compatibility promise.

## References

- [Compatibility matrix](../architecture/compatibility.md)
- [Glasswyrm integration contract](../architecture/glasswyrm-integration.md)
- [Capability schema](../../schemas/prismdrake-capabilities.schema.json)
- [Project specification sections 17 and 18](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#17-gtk-qt-x11-and-freedesktop-integration)
