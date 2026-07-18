# Shell and session control

This Accepted PD2 design freezes the same-user control boundary under
[ADR 0009](../adr/0009-pd2-state-and-control-contracts.md). The interfaces are
Experimental. Their implementation does not create a stable wire or ABI
guarantee.

## Trust and transport

`org.prismdrake.Shell1` is owned by `prismdrake-shell` at
`/org/prismdrake/Shell1`. `org.prismdrake.Session1` is owned by
`prismdrake-session` at `/org/prismdrake/Session1`. Both are exported on the
user's session bus.

Every method obtains peer credentials from the bus and requires the caller's
effective UID to match the service UID. Missing, incomplete, or mismatched
credentials fail closed. The session bus is not treated as sufficient proof by
itself. These interfaces grant no privilege and expose no root service.

Requests and replies are bounded. Service handlers complete validation and
state transition synchronously without waiting for UI or child exit. The
reference client applies a two-second monotonic deadline per method call and
returns a distinct timeout exit status. Work that continues after an accepted
logout confirmation is owned by the session state machine and has separate
bounded child-stop deadlines.

Errors are typed by invalid argument, unknown output, unavailable owner,
unauthorized caller, invalid transaction, stale transaction, busy transaction,
invalid state, and timeout class. Error text contains stable identifiers and
recovery guidance but not bus unique names, environment values, output
metadata, command text, or filesystem paths.

These rules address `PD-SEC-004`, `PD-SEC-008`, `PD-REL-006`,
`PD-REL-007`, and `PD-SESSION-004`.

## Experimental Shell1

The shell interface provides only fixed presentation commands:

```text
ToggleLauncher(output_name: s)
OpenQuickSettings(output_name: s)
OpenNotificationCenter(output_name: s)
FocusPanel(output_name: s)
```

An empty output name resolves the current primary output. A nonempty name must
match one active validated output. Unknown, ambiguous, or disappearing outputs
fail with a typed error unless the documented operation has already committed a
primary-output transfer. Requests are idempotent where practical; launcher
toggle deliberately changes between its two visible states.

Placement follows [the surface-placement contract](surface-placement.md). A
Shell1 request asks the shell focus router to use the accepted window-manager
focus path. It does not focus an application window directly, override WM
policy, establish a global shortcut, or create a global keyboard grab.

The active window manager is the global-binding authority. A binding invokes a
fixed `prismdrake-shellctl` command. No output name or action is interpolated
into a shell command.

## Experimental Session1

The session interface exposes a single logout transaction:

```text
GetSessionState()
  -> transaction_id: t, state: s
RequestLogout()
  -> transaction_id: t
ConfirmLogout(transaction_id: t)
CancelLogout(transaction_id: t)

SessionStateChanged(transaction_id: t, state: s)
```

Transaction ID zero represents no active transaction in `idle`. Each accepted
`RequestLogout` from idle allocates a nonzero unsigned 64-bit ID and enters
`confirmation_requested`. A second request while a transaction is active fails
with a deterministic busy error and does not replace, confirm, or cancel the
active transaction.

Only the exact current ID may be passed to `ConfirmLogout` or `CancelLogout`.
Zero, older, future, and already completed IDs fail without changing state.
`ConfirmLogout` and `CancelLogout` are valid only in
`confirmation_requested`. All transitions publish
`SessionStateChanged` after the new state becomes authoritative.

`GetSessionState` returns transaction ID zero only for the current `idle` state.
Transition signals for `cancelled`, `complete`, or `failed` retain the nonzero ID
of the transaction they describe. Cancellation publishes `cancelled` followed
by `idle`; clients must not reuse the completed ID.

The state machine is:

```text
idle
  -> confirmation_requested
  -> confirmed
  -> stopping_shell
  -> stopping_notifyd
  -> stopping_settingsd
  -> complete

confirmation_requested -> cancelled -> idle
confirmed or stopping_* -> failed
```

The shell presents confirmation and sends the typed confirm or cancel request;
it is not logout authority and the dialog is not an authorization prompt. A
shell crash leaves the transaction in the session process. After restart, the
shell calls `GetSessionState` and reconstructs the matching confirmation when
that state is still cancellable.

After confirmation, `prismdrake-session` stops only the exact Prismdrake child
processes it owns, in reverse dependency order: shell, notifyd, then settingsd.
Each child receives bounded graceful termination followed by the already
documented escalation policy. The transaction does not terminate the active
window manager, arbitrary applications, or unrelated session processes. PD2
adds no reboot, shutdown, suspend, hibernate, lock, or generic process method.

This boundary implements `PD-SESSION-002` through `PD-SESSION-006` and the PD2
logout-only portion of `PD-POWER-001`.

## prismdrake-shellctl

The command-line client accepts only this closed command grammar:

```text
prismdrake-shellctl launcher toggle
prismdrake-shellctl quick-settings open
prismdrake-shellctl notifications open
prismdrake-shellctl panel focus
prismdrake-shellctl session logout
```

Shell-surface commands may accept a separately validated output option when the
implemented command reference documents it; absence means primary output. The
client maps each command directly to one typed method. It has no arbitrary bus
name, object path, interface, method, property, argument, executable, or shell
mode.

Exit status distinguishes success, usage error, unavailable service,
authorization failure, rejected state, and timeout. Diagnostics are redacted.
The client owns no state, does not remain resident, and does not retry in an
unbounded loop.

## Lifecycle and recovery

- Shell owner loss causes shellctl presentation commands to fail promptly. The
  window manager remains usable.
- Session owner loss invalidates all cached transaction IDs. A replacement
  owner starts a new owner epoch and does not accept an ID from the prior epoch.
- Output removal uses the transfer/dismiss rules in the surface contract.
- No method waits for a visual frame, application exit, or D-Bus owner to return.
- Forced child termination is reported without including private child
  arguments or environment.

Relevant requirements: `PD-COMP-001`, `PD-COMP-003`, `PD-COMP-008` through
`PD-COMP-010`, `PD-INPUT-001` through `PD-INPUT-003`, `PD-REL-003`,
`PD-REL-006`, `PD-API-001` through `PD-API-005`, and `PD-DOC-006`.
