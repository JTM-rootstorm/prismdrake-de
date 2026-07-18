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
6. Start `prismdrake-notifyd` and other required non-visual services.
7. Start `prismdrake-shell` with the complete validated generation.
8. Start enabled portal and toolkit adapters.
9. Report degraded optional features and mark the session ready.

Readiness uses explicit process or D-Bus signals rather than sleeps. Dependencies
form an acyclic graph; consumers wait with bounded timeouts and useful errors.

## Experimental D-Bus ownership

| Process | Owned name | Exported interfaces | PD1 status |
|---|---|---|---|
| `prismdrake-settingsd` | `org.prismdrake.Settings1` | `org.prismdrake.Settings1`, `org.prismdrake.SettingsSnapshot1` | Experimental PD1 contract and implementation work |
| `prismdrake-notifyd` | `org.freedesktop.Notifications` | Standard notification interface | Planned with replacement/reacquisition semantics |
| Other components | Not assigned | A named interface requires its own reviewed contract | Deferred |

Both settings interfaces are exported at `/org/prismdrake/Settings1` and are
explicitly Experimental. `SettingsSnapshot1` is the internal complete-snapshot
transport for Prismdrake consumers. Implementing these interfaces does not
freeze their wire contract or provide an ABI compatibility guarantee; stability
requires a separate Accepted decision.

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

## Supervision and safe mode

Restartable components use bounded exponential backoff and a retry budget.
Repeated failure enters safe mode with packaged defaults, opaque fallback
materials, reduced motion, and optional integrations disabled. A crash loop
must preserve a basic logout path and must not restart the WM/compositor or
destroy its authoritative window state.

The Experimental PD1 supervisor gives settingsd one normal restart after 500
ms and the shell three normal restarts after 250 ms, 500 ms, and 1 second in a
rolling 30-second window. Thirty seconds of healthy runtime clears that
component's history. Exhaustion permits one final safe-mode launch of both
components; any further failure is terminal. Settings readiness has a 5-second
bound. SIGINT or SIGTERM interrupts backoff, then shutdown sends SIGTERM to the
exact shell and settingsd PIDs in that order, waits up to 2 seconds per child,
and reports a SIGKILL escalation with a further 1-second reap bound.

PD1 currently consumes a validated inherited session bus rather than spawning
one. The settings service has an explicit D-Bus readiness probe; shell startup
is confirmed by the bounded fork-to-exec handshake, while a distinct post-exec
shell-ready IPC contract remains open development-harness work.

## Shutdown order

1. Stop accepting new shell work and profile changes.
2. Close optional adapters and portal requests with bounded cancellation.
3. Stop shell and decoration presentation.
4. Flush safe notification/settings state atomically.
5. Release D-Bus names and stop non-visual services.
6. Terminate supervision after bounded child shutdown.

Logout, reboot, and power actions are requested through authoritative external
services. Forced termination is reported and never silently discards known
unsafe persistent work.
