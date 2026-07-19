# Notification service contract

This Accepted PD2 design refines the `prismdrake-notifyd` boundary from
[ADR 0002](../adr/0002-component-and-process-model.md) and
[ADR 0009](../adr/0009-pd2-state-and-control-contracts.md). The service and its
Prismdrake-native interface are Accepted PD2 implementation targets at
Experimental stability; they are not yet implemented by this design record and
are not a stable wire or storage ABI.

## Process and authority

`prismdrake-notifyd` is a separate toolkit-neutral process. It owns:

- the session-bus name `org.freedesktop.Notifications`;
- live notification identifiers, replacement, timeout, sender, action, and
  close lifecycle;
- the effective runtime do-not-disturb override;
- disabled, session, or persistent history policy and records;
- notification-center owner epochs, complete state snapshots, and generations;
  and
- readiness and bounded service-name reacquisition behavior.

It renders no UI and has no Qt, GTK, GNOME desktop-stack, X11, compositor, or
Glasswyrm dependency. `prismdrake-shell` is a replaceable presentation client.
A healthy notifyd keeps accepting notifications across shell restart. A notifyd
startup or name-ownership failure degrades only notification functionality and
must not create a session restart loop.

## Freedesktop Notifications 1.3

PD2 implements version 1.3 of the freedesktop Notifications specification with
the standard methods `GetCapabilities`, `Notify`, `CloseNotification`, and
`GetServerInformation`, plus `NotificationClosed` and `ActionInvoked` signals.
`GetServerInformation` reports specification version `1.3`.

Advertised capabilities are conservative:

```text
actions
body
persistence
```

The service does not advertise body markup, hyperlinks, images, multiple icons,
sound, or action icons in PD2. Summary, body, application name, action labels,
icon names, and hint strings are untrusted plain text. They are never executed,
treated as markup, or included in diagnostics.

The standard `Notify` method requires an `a{sv}` hints dictionary. That
dictionary is a standards-required exception to Prismdrake's typed-interface
rule, not a precedent for native APIs. The daemon bounds the complete D-Bus
message, dictionary entry count, nesting, strings, byte arrays, action pairs,
and every supported decoded value before policy is applied. Supported hints are
read through an explicit type-and-range allowlist. Unsupported, well-formed
hints are ignored; malformed or oversized input returns a D-Bus failure without
altering notification state. No caller-supplied path is opened as part of PD2
notification handling.

These rules implement `PD-NOTIFY-001`, `PD-NOTIFY-002`, `PD-NOTIFY-006`,
`PD-SEC-001`, `PD-SEC-004`, `PD-SEC-011`, and `PD-SEC-012`.

## Live identity and replacement

The freedesktop return value and `replaces_id` use unsigned 32-bit live
notification IDs. ID zero means no replacement request and is never allocated
as a live ID. Any current live ID is a valid replacement target, regardless of
which session-bus caller supplied it. A successful replacement retains the live
ID and history `record_id`, atomically replaces the content, advances
`content_generation`, and transfers sender/action routing to the replacing
caller's unique name. It emits no close signal for the replaced content. An
unknown or expired nonzero ID creates a new notification using that requested
nonzero value as its live ID and returns the same value, as required by the
standard contract. Subsequent server allocation skips every current live ID,
including IDs introduced through such requests.

History uses a separate unsigned 64-bit `record_id`. The two ID spaces are not
interchangeable. Each record also has a `content_generation`; a successful
replacement increments that generation before a complete notification-center
state is published. Shell views use `(record_id, content_generation)` as their
stable presentation command identity and never use an XID or JavaScript numeric
surrogate.

## Sender and action lifecycle

The daemon records the caller's D-Bus unique name for each live notification.
Actions are accepted only while that unique name is still owned and the
supplied record generation is current. Invoking a valid action emits the
standard `ActionInvoked` signal for the live unsigned 32-bit notification ID and
action key. Action identifiers are opaque bounded strings, never commands.

When the sender unique name disappears, all of its actions become disabled.
They remain disabled after notifyd restart or history restoration. The daemon
does not activate a desktop entry, executable, or shell command as a substitute
for a dead sender. An invocation against dead, replaced, dismissed, cleared, or
otherwise stale content returns a typed redacted error and has no side effect.

Expiration uses a monotonic clock. Non-expiring notifications remain live until
closed or dismissed. `NotificationClosed` uses the standard close-reason
semantics. The daemon removes the unsigned 32-bit ID from its live lookup before
emitting `NotificationClosed`, so a reentrant use of that ID cannot replace
already-closed content. The daemon never replays an uncertain action or close
signal after an owner restart.

## Do-not-disturb and urgency

The settings snapshot supplies a configured do-not-disturb baseline. The daemon
may hold one ephemeral effective override for its current owner epoch, as
defined in [PD2 state ownership](pd2-state-ownership.md#do-not-disturb-authority).

- Critical notifications bypass do-not-disturb and remain eligible for a
  transient presentation.
- Normal and low-urgency notifications remain eligible for configured history
  but do not create a transient while do-not-disturb is effective.
- Do-not-disturb never changes sender close/action semantics or silently deletes
  a history-eligible notification.
- `SetDoNotDisturb` updates only notifyd state and never writes configuration.
- An accepted complete settings generation clears the ephemeral override and
  makes its configured baseline effective.

This is the documented critical-information policy required by
`PD-NOTIFY-004`.

## History and privacy

The configured history mode has these exact effects:

| Mode | In-memory history | Persistent writes |
|---|---|---|
| `disabled` | none | none |
| `session` | bounded current owner lifetime | none |
| `persistent` | bounded | private atomic file |

Persistent history is opt-in and stored at
`$XDG_STATE_HOME/prismdrake/notifications/history-v1.json`. It contains at most
256 records, occupies at most 8 MiB when serialized, and uses mode `0600` where
supported. It never contains decoded image pixels. Transient notifications are
not persisted. Clear is an atomic complete-state operation. Persistence failure
does not reject a live notification and emits one redacted diagnostic.

Each persisted record is closed and contains `record_id`,
`content_generation`, nullable `application_name`, nullable `desktop_file_id`,
plain `summary` and `body`, `urgency`, `received_at_unix_ms`, `dismissible`, and
up to eight action ID/label pairs. Application names are limited to 128 Unicode
code points, summaries to 256, bodies to 4096, action IDs to 64, and action
labels to 128. Desktop-file identifiers are limited to 4096 code points and end
in `.desktop`. The file contains no live notification ID, sender name, path, or
image field.

Logs, errors, test evidence, and crash diagnostics exclude summaries, bodies,
application names, action identifiers and labels, image paths, sender-provided
paths, and raw hint values. History retention and clear controls are exposed to
the shell, satisfying `PD-NOTIFY-003`, `PD-SEC-007`, and `PD-SEC-008`.

## Experimental notification-center interface

The daemon exports `org.prismdrake.NotificationCenter1` at
`/org/prismdrake/NotificationCenter1`. Every method validates that the caller's
effective UID matches the service UID. Calls use bounded timeouts and typed
errors whose messages contain no sender content.

```text
GetCurrentState(schema_version: u)
  -> generation: t, state_json: ay
SetDoNotDisturb(enabled: b)
  -> generation: t
ClearHistory()
  -> generation: t
Dismiss(record_id: t, content_generation: t)
  -> generation: t
InvokeAction(record_id: t, content_generation: t, action_id: s)
  -> generation: t

StateChanged(generation: t, changed_domains: as)
```

`GetCurrentState` returns one schema-versioned, bounded, complete immutable JSON
snapshot. The nonzero generation is scoped to the current well-known-name owner
epoch. A mutation either validates and publishes one complete higher generation
or fails without consuming a generation. `Dismiss` and `InvokeAction` reject a
stale `content_generation`. `StateChanged` is only a fetch hint; clients fetch
and atomically replace complete state.

Notification-center state adds the effective do-not-disturb value, nullable
runtime override, configured history mode, and presentation state to the closed
history records. A live record may carry its unsigned 32-bit freedesktop
notification ID and `transient` state. Each normalized action adds an `enabled`
flag derived from sender lifecycle. No shell client derives these fields from
history content.

Clients subscribe for owner and state changes before their first fetch. On name
loss they clear live action capability and generation assumptions. After
reacquisition they fetch a complete snapshot. No generic key mutation, sender
content lookup, arbitrary D-Bus forwarding, or consumer rollback method exists.

## Shell presentation boundary

The shell receives only normalized bounded state and presents all text as
literal text. It provides keyboard and assistive-technology access to urgency,
history, action availability, dismissal, clearing, and do-not-disturb state.
Transient placement follows [surface placement](surface-placement.md). The
shell does not persist history, infer sender life, allocate IDs, or implement
notification policy.

Relevant requirements: `PD-NOTIFY-001` through `PD-NOTIFY-007`, `PD-A11Y-001`
through `PD-A11Y-009`, `PD-REL-006`, `PD-REL-007`, `PD-REL-008`, and
`PD-API-001` through `PD-API-005`.
