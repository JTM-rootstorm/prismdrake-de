# PD1 diagnostics

Prismdrake PD1 exposes a small structured diagnostic contract for the
Experimental session supervisor and a separate bounded validation contract on
the Experimental settings D-Bus interface. Neither is a general logging
framework. There is no persistent Prismdrake log store, telemetry upload,
secret field, arbitrary message body, or stable diagnostics ABI in PD1.

## Structured session events

`prismdrake-session` renders one event per line to standard error. Every line
has exactly these six space-separated machine fields in this order:

```text
component=VALUE severity=VALUE event=VALUE generation=VALUE profile=VALUE recovery=VALUE
```

`generation` is either a nonzero unsigned 64-bit value or `none`; `profile` is
`lustre`, `forge`, or `none`. Current supervisor recovery events use `none` for
both optional fields. The complete closed identifier sets are:

| Field | Allowed values |
|---|---|
| `component` | `prismdrake-foundation`, `prismdrake-session`, `prismdrake-settingsd`, `prismdrake-notifyd`, `prismdrake-shell`, `prismdrake-decor`, `prismdrake-control`, `prismdrake-portal`, `prismdrake-polkit-agent`, `prismdrake-lock`, `prismdrake-themes`, `prismdrake-style-qt`, `prismdrake-theme-gtk` |
| `severity` | `debug`, `info`, `warning`, `error`, `critical` |
| `event` | `invalid_configuration`, `unsupported_schema_version`, `component_start_failed`, `component_restart_exhausted`, `dependency_unavailable`, `capability_unavailable`, `snapshot_rejected`, `snapshot_published`, `fallback_selected`, `operation_cancelled`, `internal_error` |
| `profile` | `lustre`, `forge`, `none` |
| `recovery` | `none`, `retry`, `restart_component`, `use_fallback`, `review_configuration`, `contact_administrator` |

The rendered line is capped at 256 bytes. Adding an identifier requires a code
change; there is no free-form suffix, filesystem path, PID, window title,
notification text, candidate value, credential, or token field. A renderer
invariant failure produces the fixed `prismdrake-foundation` `internal_error`
line rather than including unreviewed data.

Only lines matching this closed shape are machine-structured diagnostics.
`prismdrake-settingsd` startup errors and Qt warnings from `prismdrake-shell`
also use standard error, but they are ordinary bounded human-facing process
messages and must not be ingested as though they had the six-field contract.

## Where to observe state

| State | Current PD1 location or interface | Lifetime |
|---|---|---|
| Supervisor events | `prismdrake-session` standard error | Process stream only |
| Session ready | `$XDG_RUNTIME_DIR/prismdrake/session-PID-N/ready` | Empty private marker; removed during exact-instance cleanup |
| Development safe mode | `$XDG_RUNTIME_DIR/prismdrake/session-PID-N/safe-mode` | Empty private marker; removed during exact-instance cleanup |
| Active profile and generation | `org.prismdrake.Settings1.GetCurrentProfile` at `/org/prismdrake/Settings1` | Current D-Bus owner epoch |
| Candidate validation | `org.prismdrake.Settings1.ValidateCandidate` | One bounded reply; never applied |
| Complete internal snapshot | `org.prismdrake.SettingsSnapshot1.GetCurrentSnapshot` | Caller-owned copy from one owner epoch |

The runtime base and its `prismdrake` child must be real, current-user-owned
mode-0700 directories, not symlinks. Session instance names contain the exact
supervisor PID plus a bounded allocation ordinal. They are coordination state,
not durable logs. Never watch for a particular future PID or retain the marker
paths as cross-session identity.

The settings service owns `org.prismdrake.Settings1` on the user session bus.
Both exported interfaces are Experimental and have no compatibility guarantee.
The internal snapshot is useful to Prismdrake consumers, but it is not a
diagnostic dump and should not be attached to bug reports by default.

## Settings validation diagnostics

`ValidateCandidate` accepts at most 1 MiB of UTF-8 TOML and returns
`valid=false` with at most 64 ordered tuples of:

```text
(logical_source_id, field_path, diagnostic_code, expected_id, recovery_id)
```

The source, code, expected, and recovery values are closed or bounded
identifiers; the canonical field path is bounded. It never echoes TOML values,
unknown keys, parser excerpts, or private filesystem paths. Oversize,
authorization, timeout, busy, shutdown, and internal failures use fixed
`org.prismdrake.Settings1.Error.*` D-Bus names instead of candidate tuples.

Do not confuse these five-field validation tuples with the six-field session
event line. They have different producers, transports, and purposes.

## Restart and safe-mode observation

The current review-visible policy gives settingsd one normal restart and the
shell three normal restarts in an inclusive 30-second rolling window. A child
that remains healthy for 30 seconds clears its component history. Settingsd
uses a fixed 500 ms backoff; shell backoff grows from 250 ms to 500 ms and then
1 second. Each component must become ready within five seconds.

Observe recovery through the combination of structured events and private
markers:

1. A failed child with budget remaining emits
   `event=component_start_failed` and `recovery=restart_component` for that
   component before the bounded backoff and exact-child restart.
2. Exhausting the normal budget emits `component_start_failed` with
   `recovery=use_fallback`. The supervisor shuts down both exact children,
   creates the empty `safe-mode` marker, emits a session
   `event=fallback_selected recovery=use_fallback`, then performs one final
   launch of both children.
3. Safe mode passes `--safe-mode` to settingsd. That service reads packaged
   defaults and disables optional integrations. The shell has no independent
   settings authority; it consumes the resulting complete generation.
4. Any unexpected child failure in the final safe-mode launch emits
   `event=component_restart_exhausted severity=critical recovery=none` and ends
   the session. Safe mode is not a second retry budget.

An `operation_cancelled` event can mean a clean child stop, requested session
shutdown, or forced-termination observation depending on component and
severity. Do not count restart activity by grepping that identifier alone.
Use the exact component, severity, event, and recovery fields together.

## Safe operator queries

Query only the public profile/generation method when checking service health:

```bash
gdbus call --session \
  --dest org.prismdrake.Settings1 \
  --object-path /org/prismdrake/Settings1 \
  --method org.prismdrake.Settings1.GetCurrentProfile
```

Inspect the panel's standard X11 declaration without enumerating unrelated
window titles:

```bash
mapfile -t panel_windows < <(xdotool search --name '^Prismdrake Panel$')
test "${#panel_windows[@]}" -eq 1
panel="${panel_windows[0]}"
xprop -id "$panel" _NET_WM_WINDOW_TYPE _NET_WM_STRUT _NET_WM_STRUT_PARTIAL
```

The output describes Prismdrake's dock request. The window manager remains
authoritative for whether and how the work area is applied. No process-name
check proves a capability, and no `GW_*` native protocol is implemented by
this PD1 path.

Capture only structured supervisor lines from a private file after review:

```bash
awk '
  /^component=[^ ]+ severity=[^ ]+ event=[^ ]+ generation=[^ ]+ profile=[^ ]+ recovery=[^ ]+$/ {
    print
  }
' /absolute/private/session.stderr.log
```

This shape filter is not a semantic validator; compare values with the closed
table above before treating the result as trusted evidence.

## Troubleshooting

### Session exits before children start

Run `prismdrake-session --help` and verify that overridden `--settingsd` and
`--shell` values are absolute regular executable paths. The session also
requires non-empty `DISPLAY`, `XDG_RUNTIME_DIR`, and
`DBUS_SESSION_BUS_ADDRESS`. `XDG_RUNTIME_DIR` must be absolute, mode 0700,
current-user owned, and free of symlink traversal.

### Settings owner is absent

Confirm the check runs inside the same session bus as the supervisor. Do not
copy or publish `DBUS_SESSION_BUS_ADDRESS`. A settingsd owner loss invalidates
the entire generation epoch; the shell clears its presentation snapshot and
refetches one complete generation only after reacquisition. It never merges
partial domain state or retries an ambiguous mutation blindly.

### Panel is absent or has no dock properties

Confirm `QT_QPA_PLATFORM=xcb` is selected by the X11 environment, `DISPLAY` is
usable, and an EWMH window manager is present. Missing or malformed output
topology leaves no validated placement. The shell does not take ownership of
output policy, focus, stacking, or workspaces to compensate.

### Lustre has no blur

That is the expected standards fallback when the capability snapshot does not
offer compositor blur. Inspect the active profile/generation and the panel's
accessible theme-diagnostics description. Do not use screenshots as a blur
source and do not infer native support from a compositor process name.

### X11 transport disappears

Task and panel adapters share one owner-thread fatal-display gate. The first
reported transport loss clears shell presentation state and requests normal
process shutdown; later reports do not cause duplicate shutdown. Qt's platform
connection is not reconstructed in process. The supervisor can restart only
within its bounded policy and only if the display is usable again. Preserve
the first redacted process error and the subsequent structured recovery events.

### A user configuration is rejected

Leave the user file untouched. The loader may select validated
last-known-valid state and then packaged defaults at startup; a failed reload
retains the prior complete generation. Use `ValidateCandidate` only with a
reviewed local file and do not paste the candidate or complete snapshot into a
public report. The diagnostic tuple supplies a canonical field path and fixed
recovery identifier without echoing the value.

## Privacy and collection rules

- Prefer the closed six-field session events and the five-field validation
  tuples. Record the exact source revision and command separately.
- Store evidence in a private mode-0700 directory. Remove it after review under
  the applicable retention policy.
- Do not collect process environments, session-bus or AT-SPI addresses, full
  accessibility trees, arbitrary `xprop` output, complete runtime snapshots,
  configuration files, desktop-entry contents, notification bodies, window
  titles, filesystem paths, credentials, tokens, or private keys.
- Ordinary shell/settingsd standard error is not guaranteed to contain only
  structured fields. Review and redact it before sharing.
- `developer.diagnostics_enabled` is part of the normalized Experimental
  settings snapshot, but PD1 does not implement it as an operator switch for a
  persistent logging service. Developer capability overrides also remain
  disabled in production builds by default.
