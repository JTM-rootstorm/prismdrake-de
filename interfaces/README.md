# Experimental interfaces

Prismdrake's PD1 and PD2 interfaces are implemented or planned prototypes. They
are explicitly Experimental and have no compatibility or ABI guarantee until a
separate stability decision is Accepted.

| Interface | Audience | Status | Compatibility |
|---|---|---|---|
| [`org.prismdrake.Settings1`](dbus/org.prismdrake.Settings1.xml) | Same-user session clients | Experimental implementation | None before separate acceptance |
| `org.prismdrake.SettingsSnapshot1` | Internal Prismdrake consumers | Experimental implementation | None before separate acceptance |
| [`org.prismdrake.Shell1`](dbus/org.prismdrake.Shell1.xml) | Same-user shell-control clients | Experimental PD2 contract | None before separate acceptance |
| [`org.prismdrake.Session1`](dbus/org.prismdrake.Session1.xml) | Same-user logout clients | Experimental PD2 contract | None before separate acceptance |
| [`org.prismdrake.NotificationCenter1`](dbus/org.prismdrake.NotificationCenter1.xml) | Same-user shell clients | Experimental PD2 contract | None before separate acceptance |

The two settings interfaces are exported at `/org/prismdrake/Settings1` by the owner of
`org.prismdrake.Settings1`. The service validates the D-Bus sender's effective
UID and fails closed when credentials are absent or do not match the service
UID. PD1 uses no privileged or cross-user settings operation.

## Complete snapshot delivery

`org.prismdrake.SettingsSnapshot1.GetCurrentSnapshot` accepts the requested
snapshot schema version and returns one generation plus one complete immutable
byte-array copy. Version 2 is canonical UTF-8 JSON governed by the strict
[`prismdrake-runtime-snapshot` schema](../schemas/prismdrake-runtime-snapshot.schema.json)
and limited to 1 MiB. The outer generation and embedded generation are equal.
There is no chunking, per-domain fetch, caller-supplied path, arbitrary object
export, or `a{sv}` extension channel.

The reply belongs to the caller. Later service publications do not mutate it.
The service retains its current and immediately previous typed and serialized
snapshots; consumer-held copies may live longer. Version 1 and unknown schema
versions are rejected without changing service state.

The top-level profile is authoritative. The nested `settings` object contains
all normalized configuration domains except that deliberately non-duplicated
profile. Theme and configuration provenance use closed logical identifiers,
never filesystem paths. Original TOML, rejected input, parser excerpts, and
private values are absent.

## Owner epochs and generation

Generation identity is `(well-known-name ownership epoch, generation)`:

- generation zero means no authoritative snapshot is published;
- the first complete publication in one owner epoch is generation one;
- successful changed publications increase monotonically;
- validation, resolution, serialization, and no-op operations consume no
  generation;
- every owner loss, replacement, disconnect, or observed ownership gap
  invalidates client generation assumptions; and
- reacquisition begins a new epoch at generation one after a complete snapshot
  is available.

Clients subscribe to `NameOwnerChanged` before their initial fetch. They clear
their cache on every ownership transition or bus disconnect and fetch one
complete snapshot after reacquisition. A signal for generation N followed by a
fetch of complete generation N+1 is a valid race; the client accepts N+1. It
never merges domains from signals or retries an ambiguous mutation blindly.

## Operations and timeouts

Immediate getters have a 1-second server budget. Validation, reload, and profile
change have a 5-second server budget; clients should allow at least 6 seconds to
receive a typed timeout. One bounded mutation/validation operation may be in
flight. Additional work returns `Busy` rather than entering an unbounded queue.

A service-returned `TimedOut` guarantees no later publication from that call.
A client-side `org.freedesktop.DBus.Error.NoReply` is ambiguous: refetch the
current complete snapshot and do not blindly retry a mutation. There is no wire
cancellation method in Experimental version 1. Deadline, service shutdown, bus
loss, and owner-epoch loss cancel internal work before publication where
possible.

`RequestProfileChange` is a runtime-only override accepting exactly `lustre` or
`forge`. Reload and restart restore the configured profile. Same-profile changes
and equivalent reloads return the current generation without a signal.

## Closed signal identifiers

`changed_domains` is a unique, deterministic subset in this order:

```text
profile, appearance, panel, launcher, notifications, desktop,
integration, accessibility, keyboard, developer, theme
```

`validation_warnings` is a unique deterministic subset of:

```text
invalid_user_configuration
invalid_last_known_valid_configuration
last_known_valid_persistence_failed
```

Theme warning identifiers are carried inside the complete snapshot. PD1 has no
accepted restart-required domain policy, so `restart_required` remains false.

## Structured candidate diagnostics

`ValidateCandidate` returns at most 64 ordered tuples with this shape:

```text
(logical_source_id, field_path, diagnostic_code, expected_id, recovery_id)
```

The source, code, expected, and recovery identifiers are at most 64 Unicode code
points. A canonical field path is at most 256. The encoded diagnostic collection
is limited to 64 KiB. Invalid syntax/schema/semantics returns `valid=false` with
diagnostics; oversize or operational inability to validate returns a typed
D-Bus error. Candidate contents are never logged or reflected.

## Typed errors

Domain failures use these fixed names under
`org.prismdrake.Settings1.Error.*`:

| Error suffix | Meaning |
|---|---|
| `InvalidProfile` | Profile is not exactly `lustre` or `forge` |
| `ValidationFailed` | Reload or publication candidate is invalid |
| `TooLarge` | A bounded input or output exceeds its fixed limit |
| `UnsupportedSnapshotVersion` | Requested snapshot schema is not version 2 |
| `NoSnapshot` | No complete authoritative snapshot is available |
| `Busy` | The one bounded worker slot is occupied |
| `TimedOut` | Work ended before publication at its server deadline |
| `NotAuthorized` | Sender credentials are missing or not same-user |
| `ServiceStopping` | Shutdown or owner-epoch loss rejects new work |
| `Internal` | A fixed redacted internal failure occurred |

Error text is fixed and bounded to 256 bytes. It never interpolates input,
parser text, filesystem paths, or secrets. Wrong wire signatures may use the
standard `org.freedesktop.DBus.Error.InvalidArgs` response.

## PD2 shell control

`org.prismdrake.Shell1` is exported at `/org/prismdrake/Shell1` by
`prismdrake-shell`. Its four narrow methods open or focus an existing shell
surface; it is not a keyboard-grab service and owns no window-manager policy.
Each method accepts a bounded validated output name. An empty name selects the
current primary output. A named output that is unknown returns `InvalidOutput`;
one that disappears before the request is applied returns `OutputUnavailable`.
The shell may use the documented primary-output fallback only when doing so
cannot place the surface on an unintended output.

Immediate shell-control calls have a 1-second server budget. Domain failures
use fixed names under `org.prismdrake.Shell1.Error.*`: `InvalidOutput`,
`OutputUnavailable`, `Busy`, `TimedOut`, `NotAuthorized`, `ServiceStopping`,
and `Internal`.

Shell pins use the strict
[`prismdrake-pin-state` schema](../schemas/prismdrake-pin-state.schema.json).
The on-disk version-1 document is an ordered unique list of at most 256 bounded
desktop-file identifiers. It stores no executable path, arguments, query
history, window title, or owner-epoch generation. Pin and unpin operations
publish a complete runtime generation separately; a shell restart begins a new
owner epoch rather than trusting an old file generation.

## PD2 logout transactions

`org.prismdrake.Session1` is exported at `/org/prismdrake/Session1` by
`prismdrake-session`. It exposes only logout; PD2 adds no power, suspend,
hibernate, restart, lock, or privileged operation. The session process remains
authoritative for child shutdown. Shell confirmation is presentation, not
authorization.

`RequestLogout` allocates one nonzero 64-bit transaction identifier and moves
the state from `idle` to `confirmation_requested`. Only that exact identifier
can be passed to `ConfirmLogout` or `CancelLogout`. The exact state set is
`idle`, `confirmation_requested`, `confirmed`, `stopping_shell`,
`stopping_notifyd`, `stopping_settingsd`, `complete`, `cancelled`, and `failed`.
Cancellation publishes `cancelled` and then `idle`; confirmation advances
through the bounded component-stop states. A second request while a transaction
exists returns `Busy`. `GetSessionState` returns one complete pair; its
transaction identifier is zero only for the current `idle` state.
`SessionStateChanged` carries the transaction that caused the transition, so a
cancelled nonzero identifier may accompany both `cancelled` and the transition
back to `idle`.

Getters have a 1-second server budget and mutations have a 5-second server
budget. A service-returned `TimedOut` guarantees no later transition from that
call. A client-side `NoReply` is ambiguous; clients refetch state and never
blindly confirm, cancel, or repeat a transaction. Domain failures use fixed
names under `org.prismdrake.Session1.Error.*`: `NoTransaction`,
`TransactionMismatch`, `InvalidState`, `Busy`, `TimedOut`, `NotAuthorized`,
`ServiceStopping`, and `Internal`.

## PD2 notification-center state

`org.prismdrake.NotificationCenter1` is exported at
`/org/prismdrake/NotificationCenter1` by `prismdrake-notifyd`. It is a private
Prismdrake state interface, separate from the standard freedesktop notification
service. `GetCurrentState` accepts only state schema version 1 and returns one
complete canonical UTF-8 JSON snapshot governed by the strict
[`prismdrake-notification-state` schema](../schemas/prismdrake-notification-state.schema.json).
The encoded snapshot is limited to 8 MiB, contains at most 256 records, and has
an outer generation equal to the embedded generation.

The state exposes persistent 64-bit `record_id` and `content_generation`
identities. A live standard notification identifier is an optional unsigned
32-bit field and is never used as a durable history identity. `Dismiss` and
`InvokeAction` require both durable values so stale shell content cannot affect
a replacement. Action invocation also requires an exact bounded `action_id` and
succeeds only while that action is marked enabled and its sender remains live.
Persistent history is governed by the strict
[`prismdrake-notification-history` schema](../schemas/prismdrake-notification-history.schema.json),
is limited to 256 records and 8 MiB, and never contains live notification IDs,
D-Bus sender identities, filesystem paths, or image data.

The NotificationCenter owner-epoch and generation rules match the settings
snapshot rules: generation zero is unpublished, the first complete state in an
epoch is generation one, real changes advance monotonically, and no-ops or
failures consume no generation. Clients subscribe before fetching, invalidate
all state on every owner transition or bus disconnect, and fetch one complete
snapshot after reacquisition. They never merge `StateChanged` domains into an
old snapshot. The signal's `changed_domains` is a unique ordered subset of
`records`, `do_not_disturb`, and `history_mode`.

Getters have a 1-second server budget and mutations have a 5-second server
budget. One bounded mutation may be in flight; excess work returns `Busy`. A
service-returned `TimedOut` guarantees no later publication from that call. A
client-side `NoReply` is ambiguous and requires a complete refetch without a
blind mutation retry. Domain failures use fixed names under
`org.prismdrake.NotificationCenter1.Error.*`: `UnsupportedSchemaVersion`,
`NoState`, `StaleRecord`, `ActionUnavailable`, `InvalidAction`, `TooLarge`,
`Busy`, `TimedOut`, `NotAuthorized`, `ServiceStopping`, and `Internal`.

## Authorization, errors, and standard-service exception

Every Prismdrake interface in this directory is same-user or more restrictive.
The service resolves the D-Bus sender's effective UID for every call and fails
closed with `NotAuthorized` when credentials are absent or do not match the
service UID. All domain error messages are fixed, redacted, and limited to 256
bytes. Notification errors and diagnostics never include summaries, bodies,
action labels, sender identities, image data, or paths.

The standard `org.freedesktop.Notifications.Notify` method necessarily includes
the specification's `a{sv}` hints dictionary. That standards-owned method is
the sole documented exception to Prismdrake's ban on arbitrary variant maps and
is not duplicated as XML in this directory. Hints are input-only, bounded, and
handled through a closed policy; the exception does not permit `a{sv}` in any
`org.prismdrake.*` interface.

Public Prismdrake interfaces use `org.prismdrake.*`. Generic Glasswyrm-native
`GW_*` proposals are owned and accepted separately by Glasswyrm and are not
D-Bus interfaces in this tree.
