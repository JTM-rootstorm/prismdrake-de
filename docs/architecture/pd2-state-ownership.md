# PD2 state ownership

This document is an Accepted PD2 design under
[ADR 0009](../adr/0009-pd2-state-and-control-contracts.md). It freezes the
authoritative owner, persistence boundary, and generation model used by PD2
workers. The file and IPC formats remain Experimental compatibility contracts.

## Ownership matrix

| Domain | Authority | Persistence | Mirrored by | Explicitly not authority |
|---|---|---|---|---|
| Configuration, profile, configured do-not-disturb baseline | `prismdrake-settingsd` | `$XDG_CONFIG_HOME/prismdrake/config.toml` | shell, notifyd, control clients | shell, notifyd |
| Settings and theme generation | `prismdrake-settingsd` | validated source and last-known-valid state | all consumers | individual views |
| Effective do-not-disturb override | `prismdrake-notifyd` | ephemeral for one notifyd owner epoch | shell notification center and quick settings | settingsd, shell |
| Live notifications and freedesktop IDs | `prismdrake-notifyd` | memory only | shell transient presentation | shell |
| Notification history, record IDs, and history generation | `prismdrake-notifyd` | disabled, session-only, or private persistent history | shell notification center | settingsd, shell |
| Ordered pinned applications and pin generation | `prismdrake-shell` | private shell state | panels and launcher | settingsd |
| Shell-surface placement and internal focus-routing state | `prismdrake-shell` | transient, reconstructed from topology and settings | individual surfaces | settingsd, WM |
| Output, focus, work-area, window, and global-binding policy | active window manager | external to Prismdrake | shell X11/Qt mirrors | every Prismdrake component |
| Application catalog identity | desktop-entry catalog plus deterministic shell association | catalog cache only where separately documented | panel and launcher models | PID or process name |
| Logout transaction and child shutdown | `prismdrake-session` | memory only | shell confirmation and shellctl | shell presentation |

A mirror may cache a complete immutable state, but it never becomes a second
authority. Owner loss invalidates generation comparisons for that owner. A
client clears dependent state and fetches a complete snapshot after the
well-known name is reacquired.

## Configuration and migration

Configuration version 2 is the canonical write shape. Readers accept exactly
versions 1 and 2:

- Version 2 is validated directly into complete typed values.
- Version 1 is validated using its original contract and normalized in memory
  to version-2 defaults.
- Reading version 1 never rewrites the user's file implicitly.
- An explicit successful write emits version 2 and preserves the original until
  the atomic replacement commits.
- Unknown versions fail before publication and leave the current generation
  authoritative.
- The runtime settings/theme snapshot uses schema version 2 even when its source
  was a version-1 document.

The migration adds the complete wallpaper source and panel output policy while
preserving existing appearance, launcher, notification, integration, keyboard,
and accessibility domains. Accessibility overrides remain independent from
profile defaults and survive normalization and profile switching.

These rules preserve `PD-CONFIG-001` through `PD-CONFIG-010`.

## Do-not-disturb authority

`prismdrake-settingsd` publishes the configured `notifications.do_not_disturb`
value as a baseline. `prismdrake-notifyd` derives an effective value from that
baseline and an optional ephemeral override:

```text
effective = ephemeral_override when present, otherwise configured_baseline
```

`NotificationCenter1.SetDoNotDisturb` changes only the notifyd-owned ephemeral
override and publishes a new notification-center generation. It does not write
`config.toml`. Every accepted complete settings generation clears an active
override and restores its newly configured baseline so an explicit
configuration reload takes effect deterministically. Losing or restarting the
notifyd owner also clears the override; the new owner initializes from the
latest complete configured baseline. Notification-center state exposes the
effective value and the nullable active override.

Critical notifications bypass do-not-disturb. Normal and low-urgency records
remain eligible for history but do not create transient presentation while the
effective value is enabled. This distinction satisfies `PD-NOTIFY-004` without
giving two processes authority over one effective value.

## Pin state

The shell exclusively owns:

```text
$XDG_STATE_HOME/prismdrake/shell/pinned-applications-v1.json
```

The on-disk schema contains only `schema_version = 1` and an ordered set of at
most 256 unique desktop-file identifiers, each at most 4096 Unicode code points
and ending in `.desktop`. It contains no runtime generation, XIDs, PIDs, window
titles, executable paths, arguments, or query history. Mutations validate a
complete candidate, write it atomically with same-user permissions, then
publish one complete nonzero runtime generation scoped to the shell owner
epoch. A malformed file is retained for diagnosis and recovery, and the shell
starts with an empty pin set rather than executing or guessing content.

## Notification state

The notification daemon exclusively owns live notification IDs, history record
IDs, retention, and history generations.

- A live freedesktop notification ID is an unsigned 32-bit identifier scoped to
  the service owner and replacement lifecycle.
- A history `record_id` is a distinct unsigned 64-bit identifier. It is never
  accepted where a live freedesktop ID is required.
- A record's `content_generation` changes when replacement changes action- or
  presentation-relevant content.
- Notification-center mutations carry `record_id` and
  `content_generation`; stale content is rejected with a typed redacted error.
- The complete notification-center state has a nonzero generation scoped to one
  notifyd well-known-name owner epoch.

Persistent history is opt-in. Its path is:

```text
$XDG_STATE_HOME/prismdrake/notifications/history-v1.json
```

The file is limited to 256 records and 8 MiB, uses mode `0600` where supported,
and is replaced atomically. Session mode never writes history. Disabled mode
retains none. Transient records and image pixels are never persisted. A history
write failure does not reject a live notification and produces only one
redacted diagnostic. Clearing history is a complete atomic mutation.

Durable restoration is gated by the first accepted settings observation in a
notifyd owner epoch. Persistent mode restores one valid startup file only when
the live record set is still empty, and performs that restoration before any
empty persistence write. Missing or malformed input is not rewritten. Live
records take precedence, restored action targets are disabled, and a later mode
change cannot reuse stale startup state.

These rules address `PD-NOTIFY-001` through `PD-NOTIFY-007`, `PD-SEC-007`,
`PD-SEC-008`, `PD-REL-006`, and `PD-REL-008`.

## Bounds, privacy, and failures

Every persisted document and D-Bus payload has a fixed schema and size bound.
Strings, collections, IDs, paths, enum values, and state transitions are
validated before mutation. Diagnostics contain stable field and error IDs but
not notification bodies, titles, action labels, window titles, file paths, or
rejected sender values.

Atomic-write failure retains the previous valid state. Missing optional
notification service behavior degrades only notifications. A shell restart
rebuilds notification and settings mirrors from their authorities and reloads
its own validated pin file. No restart transfers WM or external-service
authority into Prismdrake.

Relevant requirements: `PD-COMP-001` through `PD-COMP-010`, `PD-SEC-001`,
`PD-SEC-004`, `PD-SEC-007`, `PD-SEC-008`, `PD-REL-002`, `PD-REL-006`,
`PD-REL-008`, and `PD-API-001` through `PD-API-005`.
