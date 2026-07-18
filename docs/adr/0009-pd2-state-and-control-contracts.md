# ADR 0009: PD2 state ownership and control contracts

- **Status:** Accepted
- **Date:** 2026-07-18
- **Owners:** Prismdrake maintainers
- **Supersedes:** ADR 0004 only for configuration- and runtime-schema version-specific portions

## Context

PD2 adds persistent shell pins, notification history, wallpaper selection,
multi-output surfaces, and coordinated shell and session control. Those domains
cross existing process boundaries. Without one accepted ownership and versioning
record, parallel implementation could create competing authorities, incompatible
state files, or unsafe generic IPC.

ADR 0004 established TOML, XDG locations, strict validation, atomic writes, and
immutable generation-tagged snapshots with a version-1 source shape. PD2 needs a
version-2 configuration and runtime snapshot while retaining those accepted
foundations.

## Decision drivers

- Preserve one authority for every state domain.
- Migrate version-1 configuration without destructive implicit rewrites.
- Keep persistent state bounded, private, atomic, and recoverable.
- Keep shell and session control typed, same-user, and time-bounded.
- Use the freedesktop notification standard without turning its required
  variant dictionary into a general Prismdrake API pattern.
- Make output removal, service loss, and process restart deterministic.
- Preserve Qt 6.11, standards-first X11 behavior, and Glasswyrm ownership.

## Considered options

1. Freeze the PD2 ownership, migration, surface, notification, and control
   contracts before dependent implementation.
2. Keep version 1 and store new settings in unrelated sidecar files.
3. Let each shell module persist its own partial state and coordinate through
   loosely typed messages.

The first option keeps one coherent versioned configuration and explicit state
owners. Sidecars would split configuration authority and complicate recovery.
Module-local persistence and generic messages would create mixed generations,
unclear lifecycle behavior, and a larger attack surface.

## Decision

Configuration version 2 is the canonical write shape. Readers accept versions 1
and 2, normalize version 1 to complete version-2 typed defaults in memory, and
never rewrite a version-1 user file merely because it was read. Explicit writes
emit version 2. The complete runtime snapshot also advances to version 2. TOML,
XDG paths, strict validation, preservation of original input until a successful
write, packaged recovery defaults, atomic publication, and generation semantics
from ADR 0004 remain Accepted.

`prismdrake-settingsd` owns configured settings, including the persisted
do-not-disturb baseline. `prismdrake-notifyd` owns the ephemeral effective
do-not-disturb override, which a complete settings generation or notifyd owner
restart clears, plus the freedesktop service, live notification identifiers,
notification history, history record identifiers, and notification-center
generations. `prismdrake-shell` owns pin persistence and all shell-surface
placement. The active window manager remains authoritative for focus, work
areas, global bindings, and output policy.

Pin state is a schema-versioned ordered list of at most 256 unique desktop-file
identifiers at
`$XDG_STATE_HOME/prismdrake/shell/pinned-applications-v1.json`. Notification
history is a schema-versioned private file at
`$XDG_STATE_HOME/prismdrake/notifications/history-v1.json`, limited to 256
records and 8 MiB, and written only in configured persistent mode. Notification
history uses an unsigned 64-bit `record_id`; the live freedesktop service uses
its separately scoped unsigned 32-bit notification ID. Neither identifier is an
XID, PID, or application identity.

`prismdrake-notifyd` implements the freedesktop Notifications specification
version 1.3. The standard's required `a{sv}` hints parameter is a bounded,
validated standards exception; it does not authorize variant dictionaries in
Prismdrake-native interfaces. Unsupported hints are ignored after their message
and container bounds are enforced. Sender text is plain untrusted text, actions
are tied to the sender's unique-name lifetime, and persisted records contain no
image pixels.

The Experimental `org.prismdrake.Session1`, `org.prismdrake.Shell1`, and
`org.prismdrake.NotificationCenter1` interfaces accept only same-user callers.
Session logout is a transaction: `RequestLogout` allocates a nonzero unsigned
64-bit transaction ID and only matching `ConfirmLogout` or `CancelLogout`
requests may advance it. Notification-center `Dismiss` and `InvokeAction`
require both an unsigned 64-bit history record ID and the record's content
generation, preventing actions against replaced or stale content. All methods
use bounded timeouts, typed redacted errors, owner-epoch invalidation, and no
arbitrary-key or arbitrary-command access. These interfaces remain Experimental
and provide no stable wire or ABI guarantee.

Surface placement follows an explicit per-surface policy. Every active output
has one non-focusable desktop. Panels use `primary`, `all`, or `named` policy.
Launcher, notification center, and quick settings have one global visible
instance each and remember their invoking output. Transient notifications use
the current primary output. Output removal transfers a transient surface to the
new primary output or dismisses it when no valid output remains. Qt 6.11 screen
geometry is reconciled with validated RandR observations by stable name and
geometry; physical millimetres are not a scale source.

Application identity is a typed desktop-entry association, not a process-name
guess. Exact activation identity and normalized `StartupWMClass`/`WM_CLASS`
matches precede deterministic desktop-file and bounded fallback grouping.
Ambiguity resolves to a generic unknown application with a redacted diagnostic.

## Consequences

PD2 readers and writers need explicit version dispatch, migration tests, strict
state schemas, atomic file handling, owner-epoch handling, and generation guards.
The notification daemon must remain alive independently of shell restarts and
toolkit-neutral. Shell control does not create a global keyboard-grab service;
bindings remain mediated by the active window manager.

The v2 change is deliberately source- and runtime-schema breaking while the
contracts remain Experimental. Version-1 input remains supported through
in-memory normalization, so users are not forced into an implicit migration.
Optional Glasswyrm protocols remain Proposed and are neither required nor
implemented by this decision.

## Validation or evidence

The maintainer approved the PD2 execution plan and its state-ownership,
multi-output, notification, and shell/session-control decisions on 2026-07-18.
Contract validation parses the accepted ADRs, verifies local links and canonical
names, and validates the accompanying versioned schemas and Experimental D-Bus
XML. Dependent implementation must add negative tests for version migration,
state bounds, same-user authorization, stale generations, malformed standard
hints, owner loss, and output removal.

## Revisit conditions

Revisit before stabilizing any D-Bus or file contract, adding per-output
wallpapers or task filtering, persisting the runtime do-not-disturb override,
supporting notification rich text or images, changing the freedesktop protocol
version, or introducing accepted Glasswyrm-native surface roles. A revision must
update interfaces, schemas, tests, migration behavior, and these ownership
documents together.

## References

- [PD2 state ownership](../architecture/pd2-state-ownership.md)
- [Configuration contract](../architecture/configuration.md)
- [Notification service](../architecture/notification-service.md)
- [Surface placement](../architecture/surface-placement.md)
- [Shell and session control](../architecture/shell-control.md)
- [Application identity](../architecture/application-identity.md)
- [Project specification sections 8 through 15](../PRISMDRAKE_PROJECT_SPECIFICATION.md#8-architecture-principles)
- [Project specification API policy](../PRISMDRAKE_PROJECT_SPECIFICATION.md#26-apis-schemas-and-compatibility-policy)
