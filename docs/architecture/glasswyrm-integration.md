# Proposed Glasswyrm integration contract

Prismdrake operates through a standards baseline and optionally negotiates
generic, versioned Glasswyrm capabilities. PD0 defines no wire protocol and
implements no native interface. Candidate names and signatures require separate
Glasswyrm acceptance.

## Standard baseline

- ICCCM and EWMH provide window lists, active-window state, basic workspace
  information, state-change requests, and dock work-area reservation where the
  active WM advertises them.
- XDG, D-Bus, XSettings, freedesktop notifications, portals, desktop entries,
  and icon themes provide desktop/application integration.
- Ordinary alpha or opaque surfaces render without native blur.
- Window previews use application icons, titles, and explicit states when no
  authorized thumbnail source exists.

This baseline is required for Tier B. Tier C uses the same workflow with opaque
or reduced-effects materials.

## Optional native enhancements

| Candidate placeholder | Intent | Required fallback |
|---|---|---|
| `GW_SHELL_ROLE_V1` | Identify bounded shell surface roles | Standard X11 window type and dock strut behavior |
| `GW_BACKDROP_V1` | Submit blur regions and visual parameters | Token-declared alpha or opaque material |
| `GW_WINDOW_THUMBNAIL_V1` | Request scoped, authorized window thumbnails | Application icon, title, and state |
| `GW_WORKSPACE_V1` | Present richer workspace identity and lifecycle | Basic EWMH indexed workspaces |
| `GW_DECORATION_V1` | Coordinate authoritative WM state with optional rendering | WM or client decoration/operation fallback |

Compositor diagnostics and future animation synchronization may be evaluated
later but have no candidate contract in PD0. Secure lock primitives remain
deferred until an accepted threat model.

## Negotiation rules

1. Discover capabilities through an explicit query or accepted protocol, never
   solely from a compositor process or executable name.
2. Validate the capability-set schema and explicit version.
3. Enable only known, supported versions and fields.
4. Apply the standard fallback for missing, older, malformed, or revoked
   capabilities.
5. Re-negotiate on owner/compositor restart and discard stale objects.
6. Expose sanitized capability diagnostics without window titles, private paths,
   or user content.

Unknown versions fail safely. Native interface names never contain Prismdrake.
The generic `GW_*` family is Accepted by `PD-ID-008`; the candidate names above,
their transport, signatures, lifetime, errors, and compatibility are Proposed.

## Effects and privacy

The shell sends effect intent, geometry, token-derived parameters, and a surface
identity. The compositor validates and executes blur and composition. Shell-side
desktop screenshot blur is prohibited.

Thumbnail or capture work cannot begin until authorization scope, requester and
target identity, lock-state behavior, revocation, lifetime, output protection,
and diagnostic redaction are accepted. Failure or denial selects icons and
titles without weakening the session.

Relevant requirements: `PD-PLAT-001` through `PD-PLAT-004` and `PD-GW-001`
through `PD-GW-010`.

