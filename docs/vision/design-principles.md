# Design principles

## Familiar and discoverable

Use conventional desktop hierarchy, predictable placement, visible state, and
direct manipulation. Keyboard focus order must be deterministic and primary
workflows must not depend on hover or hidden gestures.

## Original Prismdrake identity

Prismatic facets, restrained draconic cues, luminous edges, and tactile
surfaces provide a recognizable visual language. Originality applies to
geometry, icons, motion, sounds, names, and assets—not only file provenance.

## Depth with clarity

Lustre may use layered alpha and compositor-provided blur; Forge may use
stronger borders and dimensional surfaces. Text contrast, focus, urgency, and
control state remain readable over real backgrounds. Decoration never outranks
information.

## Fast feedback

Input acknowledgment and essential actions take priority over decorative
motion. Animation is brief, interruptible, and non-blocking. Reduced-motion
behavior removes or replaces nonessential movement.

## Accessibility is functional correctness

Keyboard operation, accessible semantics, visible focus, text scaling, high
contrast, disabled transparency, minimum target sizes, and non-color state cues
are baseline requirements shared by both profiles.

## Graceful effects fallback

Every blur request has alpha-tinted and opaque materials. Missing thumbnails
fall back to icons and titles. Optional capabilities and services degrade only
their feature and must not prevent session startup.

## Coherence across toolkits

GTK and Qt applications should receive coherent fonts, icons, cursors,
settings, and optional style adapters. Prismdrake does not promise pixel-level
control over applications that intentionally own their presentation.
