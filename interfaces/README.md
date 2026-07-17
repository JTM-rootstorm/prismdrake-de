# Experimental interfaces

Prismdrake's PD1 interfaces are implemented prototypes. They are explicitly
Experimental and have no compatibility or ABI guarantee until a separate
stability decision is Accepted.

| Interface | Audience | PD1 status | Compatibility |
|---|---|---|---|
| [`org.prismdrake.Settings1`](dbus/org.prismdrake.Settings1.xml) | Same-user session clients | Experimental implementation | None before separate acceptance |
| `org.prismdrake.SettingsSnapshot1` | Internal Prismdrake consumers | Experimental implementation | None before separate acceptance |

Both interfaces are exported at `/org/prismdrake/Settings1` by the owner of
`org.prismdrake.Settings1`. The service validates the D-Bus sender's effective
UID and fails closed when credentials are absent or do not match the service
UID. PD1 uses no privileged or cross-user settings operation.

## Complete snapshot delivery

`org.prismdrake.SettingsSnapshot1.GetCurrentSnapshot` accepts the requested
snapshot schema version and returns one generation plus one complete immutable
byte-array copy. Version 1 is canonical UTF-8 JSON governed by the strict
[`prismdrake-runtime-snapshot` schema](../schemas/prismdrake-runtime-snapshot.schema.json)
and limited to 1 MiB. The outer generation and embedded generation are equal.
There is no chunking, per-domain fetch, caller-supplied path, arbitrary object
export, or `a{sv}` extension channel.

The reply belongs to the caller. Later service publications do not mutate it.
The service retains its current and immediately previous typed and serialized
snapshots; consumer-held copies may live longer. Unknown schema versions are
rejected without changing service state.

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
| `UnsupportedSnapshotVersion` | Requested snapshot schema is not version 1 |
| `NoSnapshot` | No complete authoritative snapshot is available |
| `Busy` | The one bounded worker slot is occupied |
| `TimedOut` | Work ended before publication at its server deadline |
| `NotAuthorized` | Sender credentials are missing or not same-user |
| `ServiceStopping` | Shutdown or owner-epoch loss rejects new work |
| `Internal` | A fixed redacted internal failure occurred |

Error text is fixed and bounded to 256 bytes. It never interpolates input,
parser text, filesystem paths, or secrets. Wrong wire signatures may use the
standard `org.freedesktop.DBus.Error.InvalidArgs` response.

Public Prismdrake interfaces use `org.prismdrake.*`. Generic Glasswyrm-native
`GW_*` proposals are owned and accepted separately by Glasswyrm and are not
D-Bus interfaces in this tree.
