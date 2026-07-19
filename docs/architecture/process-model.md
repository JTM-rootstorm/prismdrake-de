# Process and lifecycle model

The process model is Accepted by
[ADR 0002](../adr/0002-component-and-process-model.md). It is init-neutral: a
portable `prismdrake-session` supervisor is the baseline, while systemd user
units may be optional integration.

## Process boundaries

`prismdrake-session`, `prismdrake-settingsd`, `prismdrake-notifyd`, and
`prismdrake-shell` are separate processes. Shell surfaces begin as logical
modules in one shell process. `prismdrake-decor`, `prismdrake-control`,
`prismdrake-portal`, and toolkit adapters are optional separate processes or
packages where their feature is enabled. `prismdrake-polkit-agent` must remain
isolated. `prismdrake-lock` is deferred pending an Accepted security design.

No new daemon should be introduced without concrete fault-isolation, security,
dependency, or measured performance justification.

## Startup sequence

1. Establish the Prismdrake and XDG session environment.
2. Connect to or start suitable session D-Bus infrastructure.
3. Start `prismdrake-settingsd`; validate defaults, configuration, and themes.
4. Publish one complete settings and theme generation.
5. Detect standard and optional native capabilities without process-name
   inference.
6. Start `prismdrake-notifyd`; a name conflict or startup failure records a
   degraded notification state rather than failing the complete session.
7. Start `prismdrake-shell` with the complete validated generation.
8. Start enabled portal and toolkit adapters.
9. Report degraded optional features and mark the session ready.

Readiness uses explicit process or D-Bus signals rather than sleeps. Dependencies
form an acyclic graph; consumers wait with bounded timeouts and useful errors.

## Experimental D-Bus ownership

| Process | Owned name | Object path and interfaces | Status |
|---|---|---|---|
| `prismdrake-settingsd` | `org.prismdrake.Settings1` | `/org/prismdrake/Settings1`: `org.prismdrake.Settings1`, `org.prismdrake.SettingsSnapshot1` | Implemented Experimental settings boundary; snapshot v2 is the Accepted PD2 target |
| `prismdrake-notifyd` | `org.freedesktop.Notifications` | standard path and `org.freedesktop.Notifications` 1.3 | Implemented PD2 standards boundary |
| `prismdrake-notifyd` | `org.prismdrake.NotificationCenter1` | `/org/prismdrake/NotificationCenter1`: `org.prismdrake.NotificationCenter1` | Implemented Experimental PD2 contract |
| `prismdrake-shell` | `org.prismdrake.Shell1` | `/org/prismdrake/Shell1`: `org.prismdrake.Shell1` | Implemented Experimental PD2 contract |
| `prismdrake-session` | `org.prismdrake.Session1` | `/org/prismdrake/Session1`: `org.prismdrake.Session1` | Implemented Experimental PD2 contract |

Prismdrake-native interfaces allow same-user callers only. They use typed
redacted errors, bounded timeouts, fixed versioned methods, and no arbitrary-key,
command, or D-Bus-forwarding operations. Implementing them does not freeze a
wire contract or provide an ABI compatibility guarantee; stability requires a
separate Accepted decision. The required standard `a{sv}` notification hints
parameter remains a bounded standards exception rather than a native-interface
pattern.

## Generation propagation

Settings and theme inputs are validated together into immutable snapshots with
a monotonically increasing generation inside one D-Bus owner epoch. A change
notification names the new generation and affected domains, but it does not
carry replacement state. Each consumer fetches the complete immutable snapshot
and swaps to it atomically; it never combines old and new domains. If a
component restarts or observes a D-Bus ownership transition, it discards cached
generation assumptions and fetches the current complete snapshot.

Generation identity is the pair `(well-known-name ownership epoch,
generation)`. A new owner epoch restarts numbering at generation one only after
a complete snapshot is available. Validation, resolution, serialization, and
no-op operations consume no generation. Publication is one-way: PD1 has no
consumer acknowledgement or two-phase rollback protocol. A failed candidate
leaves the service's previous valid generation authoritative; a consumer that
cannot apply an already published generation retains its local prior snapshot
and recovers by fetching complete authoritative state rather than asking the
service to roll back.

Notification-center and pin publications use the same owner-epoch principle but
have independent authorities and generation spaces. A generation from
settingsd, notifyd, or shell pin state is never compared with another owner's
generation. Notification mutations guard record content with
`(record_id, content_generation)` in addition to the notifyd state generation.

## Supervision and safe mode

Restartable components use bounded exponential backoff and a retry budget.
Repeated failure enters safe mode with packaged defaults, opaque fallback
materials, reduced motion, and optional integrations disabled. A crash loop
must preserve a basic logout path and must not restart the WM/compositor or
destroy its authoritative window state.

The Experimental PD2 supervisor gives settingsd one normal restart after 500
ms, notifyd two normal restarts after 250 ms and 500 ms, and the shell three
normal restarts after 250 ms, 500 ms, and 1 second in a rolling 30-second
window. Thirty seconds of healthy runtime clears that component's history.
Notifyd exhaustion degrades notification functionality; critical-component
exhaustion permits one final safe-mode launch. Any further critical failure is
terminal. Settings, notification, and shell readiness are bounded. SIGINT or
SIGTERM interrupts readiness waits and backoff, then shutdown sends SIGTERM to
the exact shell, notifyd, and settingsd PIDs in reverse dependency order, waits
up to 2 seconds per child, and reports a SIGKILL escalation with a further
1-second reap bound.

After readiness, steady-state supervision is event driven. The supervisor
blocks on the Session1 bus descriptor and its monotonic bus deadline together
with a signal-safe activity channel written by `SIGCHLD`, `SIGINT`, and
`SIGTERM`. Every wake dispatches bounded Session1 work and performs nonblocking
checks only for the exact owned children. A signal delivered just before the
blocking wait remains queued in the activity channel, so it cannot strand child
reaping or shutdown behind a fixed monitor cadence. Readiness retries, recovery
backoff, and exact-child termination retain their separate bounded deadline
waits.

The current supervisor consumes a validated inherited session bus rather than
spawning one. The settings service has an explicit D-Bus readiness probe. For each shell
launch, the supervisor creates a new private Linux event descriptor, passes it
only across that exact fork/exec, and waits for one fixed post-exec value. The
shell adopts the descriptor close-on-exec, removes its private environment
entry from application launch environments, and publishes once only after the
initial complete settings generation has created the QML views and
standards-only panel window host. Success therefore means that a complete panel
presentation epoch exists; it does not claim that a frame has been exposed,
composited, or reviewed visually. Invalid messages are rejected without
rendering payload bytes, and timeout, child closure or exit, restart, and
shutdown all discard that launch's channel. This is private session plumbing,
not a public Prismdrake IPC interface.

PD2 inserts notifyd between settingsd and shell in the supervised child order.
Notifyd readiness is bounded. Exhaustion disables notifications while allowing
the settings-backed shell to continue. Shell and settingsd keep their existing
critical-readiness relationship. Name conflicts do not trigger an unbounded
restart loop.

## Logout transaction

`prismdrake-session` owns one nonzero unsigned 64-bit logout transaction ID per
active request. `RequestLogout` moves idle to confirmation requested;
transaction-matching `ConfirmLogout` begins bounded reverse-order shutdown and
`CancelLogout` returns a still-cancellable request to idle. Concurrent, stale,
zero, or wrong-epoch IDs fail without changing state. The shell confirmation is
presentation, not authorization or shutdown authority.

The complete transaction and method contract is documented in
[shell and session control](shell-control.md).

## Shutdown order

1. Stop accepting new shell work and new logout transactions.
2. Close optional adapters and portal requests with bounded cancellation.
3. Stop the exact supervised shell child.
4. Flush safe notification history atomically and stop the exact notifyd child.
5. Complete any already committed settings write and stop the exact settingsd
   child.
6. Release owned D-Bus names and terminate supervision after bounded child
   shutdown.

PD2 logout coordinates Prismdrake-owned children and does not claim authority
over unrelated applications or the WM. Broader application-session logout and
all reboot, shutdown, suspend, and hibernate actions use authoritative external
services. Forced termination is reported and never silently discards known
unsafe persistent work.
