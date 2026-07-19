# Application identity and association

This Accepted PD2 design defines the shared identity contract used by the panel,
launcher, icon resolver, and pin state under
[ADR 0009](../adr/0009-pd2-state-and-control-contracts.md). It extends the PD1
WM_CLASS fallback without giving Prismdrake authority over window lifecycle.

## Typed identities

PD2 uses distinct domain types:

- `DesktopFileId`: a validated Desktop Entry Specification file identifier,
  ending in `.desktop`, derived from a bounded relative applications path;
- `ApplicationId`: a tagged association identity containing either one
  `DesktopFileId`, one bounded WM_CLASS fallback key, or the fixed
  `unknown-application` identity;
- `WindowIncarnationId`: the existing nonzero decoder-assigned identity for one
  observed XID lifetime; and
- `AssociationGeneration`: a nonzero generation for one complete immutable
  catalog-to-window association snapshot.

These types are not interchangeable. A PID, executable name, process name,
D-Bus unique name, window title, XID alone, icon name, or filesystem path is not
an `ApplicationId`. Pins contain only `DesktopFileId` values. Application
identity does not authorize launching, focusing, or signaling a process.

The display-free identity core exposes no Qt type. Qt-bound presentation
adapters consume immutable results.

## Inputs and validation

The association core consumes:

- one complete visible desktop-entry catalog generation, including retained
  bounded `StartupWMClass` values;
- one complete authoritative task observation containing validated
  `(XID, WindowIncarnationId)` and WM_CLASS instance/class fields; and
- an exact desktop-file identity supplied by a successful accepted activation
  path when that identity is available.

Desktop entries, WM_CLASS bytes, and activation metadata are untrusted. The
existing X11 decoder continues to require exactly two NUL-terminated Latin-1
WM_CLASS fields and rejects malformed or oversized properties. Desktop-entry
values use the parser's existing UTF-8, escape, size, and field constraints.
Stale XIDs never carry an association into a new incarnation.

## Normalized comparison

Association comparison uses a deliberately narrow normalization token:

1. Decode through the field's accepted UTF-8 or Latin-1 boundary.
2. Reject an empty, embedded-NUL, control-character, or oversized candidate.
3. Apply locale-independent ASCII case folding.
4. Preserve non-ASCII code points, punctuation, and internal whitespace exactly.

For desktop-file comparison, remove only the final `.desktop` suffix before
the same normalization. Do not remove vendor prefixes, collapse punctuation,
split words, inspect executable paths, or apply locale-sensitive folding. This
keeps matches deterministic and prevents a heuristic from silently treating two
applications as the same.

## Association order

Each window is associated in this exact order:

1. An exact `DesktopFileId` already supplied by an accepted activation path,
   provided that identifier exists in the current visible catalog.
2. An exact normalized `StartupWMClass` match to the WM_CLASS class field.
3. An exact normalized `StartupWMClass` match to the WM_CLASS instance field.
4. An exact normalized desktop-file identifier match to the WM_CLASS class,
   then instance field.
5. The existing bounded WM_CLASS class-or-instance fallback grouping key.
6. The fixed `unknown-application` identity.

At steps 2 through 4, more than one matching visible desktop entry is ambiguous.
The core does not select by discovery order, path precedence, popularity,
process name, or icon. It publishes the fixed `unknown-application` identity and
one redacted diagnostic containing only the evidence class, not candidate
paths, window titles, or raw metadata.

The complete association snapshot records an evidence enum for these six
classes. Consumers may present a generic state for weak or unknown association,
but must not reinterpret the evidence. Rebuilding against identical catalog and
window inputs produces identical associations.

This fulfills `PD-WIN-001`, `PD-WIN-002`, `PD-WIN-006`, and `PD-WIN-007`.

## Grouping and pin behavior

Running windows with the same desktop-entry-backed `ApplicationId` may form one
task group. A WM_CLASS fallback groups only the matching bounded fallback token.
`unknown-application` does not collapse every unknown window into an
indistinguishable application group; task lifetime remains available to keep
unassociated windows operable.

A pin refers to one visible `DesktopFileId`. It never stores the current XID,
title, association fallback, absolute desktop-entry path, executable, or launch
arguments. If the desktop entry disappears, the pin remains a bounded missing
identity with a generic icon and an unavailable launch state until the catalog
returns or the user removes it. The shell never guesses a replacement desktop
entry.

The toolkit-neutral `Prismdrake::pin-state` store remains the durable snapshot
boundary. The Qt shell reaches it only through an owner-scoped asynchronous
controller with one dedicated worker and at most one outstanding operation.
Initial load and complete-snapshot replacement therefore never validate, write,
or synchronize the pin file on the UI thread. A new generation is published on
the controller owner thread only after the durable replacement succeeds;
failure retains the prior complete generation, and initial load gates shell
readiness. Shutdown requests cancellation through the same bounded worker
boundary before joining it.

## Icon request contract

An immutable icon request contains:

- the associated desktop-file identity when available;
- the desktop entry's validated icon field;
- a requested logical size and finite positive device pixel ratio;
- the active icon-theme generation; and
- the fixed Prismdrake generic fallback identity.

Resolution follows the freedesktop icon-theme path through explicit bounded
lookup. Inside the icon worker, the Qt 6.11 metadata provider captures only the
reentrant active-theme name, fallback-theme name, and theme-search paths. The
worker then validates those values, reads bounded
`index.theme` files without following leaf symlinks, follows a bounded
inheritance chain, and selects from bounded directory and format candidates.
System Qt image readers decode supported raster data and Qt SVG provides SVG
decoding. A desktop-entry icon may be a validated absolute path; relative
paths, URLs, command text, and path traversal are rejected. Missing or invalid
input always reaches a guaranteed original generic Prismdrake fallback.

Theme metadata capture, filesystem discovery, and every decode stay on the
worker and have fixed search-root, path-byte, inheritance, source-byte,
decoded-dimension, frame-count, memory, and outstanding-request bounds. A
destroyed request context suppresses its callback, and the launcher rejects a
completion whose request or theme generation is no longer current. Cache keys
contain immutable request fields, device pixel ratio, and theme generation;
they contain no window title or PID. Cache replacement is bounded.

`_NET_WM_ICON` remains an optional later fallback within PD2. If enabled by a
separately reviewed implementation slice, it must reuse the existing strict X11
property bounds, select a bounded best-fit frame, decode off the GUI thread, and
never change application association. Native thumbnails and capture are not
icons and remain outside this contract.

These rules address `PD-INT-007`, `PD-SEC-001`, `PD-SEC-002`, `PD-SEC-008`,
`PD-PERF-001`, and `PD-PERF-006`.

## Lifecycle and failure behavior

- A catalog refresh publishes all associations atomically or retains the prior
  complete generation.
- Window destruction removes the matching incarnation promptly.
- Catalog owner loss or parse failure cannot transfer window authority to the
  shell.
- Ambiguity, missing icons, and missing desktop entries fail soft with generic
  presentation.
- Malformed metadata and decoder exhaustion fail closed at their parser bounds
  without dereferencing stale window state.
- No Glasswyrm capability is required; optional native enhancement does not
  change identity authority.

Relevant requirements: `PD-COMP-001`, `PD-PANEL-002`, `PD-PANEL-003`,
`PD-LAUNCH-001` through `PD-LAUNCH-003`, `PD-LAUNCH-007`, `PD-REL-005`,
`PD-REL-006`, and `PD-API-001` through `PD-API-005`.
