# Proposed process and lifecycle model

The process model is Proposed by [ADR 0002](../adr/0002-component-and-process-model.md).
It is init-neutral: a portable `prismdrake-session` supervisor is the baseline,
while systemd user units may be optional integration.

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

## Proposed D-Bus ownership

| Process | Proposed name | Notes |
|---|---|---|
| `prismdrake-settingsd` | `org.prismdrake.Settings1` | Draft narrow settings interface |
| `prismdrake-notifyd` | `org.freedesktop.Notifications` | Standard service with replacement/reacquisition semantics |
| Other components | Not assigned in PD0 | A named interface requires its own reviewed contract |

PD0 does not freeze these interfaces or implement a service.

## Generation propagation

Settings and theme inputs are validated together into immutable snapshots with
a monotonically increasing generation. A change notification names the new
generation and affected domains. Each consumer swaps to that complete
generation atomically; it never combines old and new domains. If a component
restarts mid-switch, it requests the current complete generation. On validation
or application failure, the previous valid generation remains authoritative.

## Supervision and safe mode

Restartable components use bounded exponential backoff and a retry budget.
Repeated failure enters safe mode with packaged defaults, opaque fallback
materials, reduced motion, and optional integrations disabled. A crash loop
must preserve a basic logout path and must not restart the WM/compositor or
destroy its authoritative window state.

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
