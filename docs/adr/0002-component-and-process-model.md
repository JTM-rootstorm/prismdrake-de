# ADR 0002: Component and process model

- **Status:** Proposed
- **Date:** 2026-07-15
- **Owners:** Prismdrake maintainers

## Context

Prismdrake needs clear authority and crash boundaries before production code
creates accidental coupling. Too few processes could combine untrusted parsing,
privileged prompts, persistence, and UI failure. Too many daemons could create
cyclic startup and fragile supervision.

## Decision drivers

- Keep Glasswyrm authoritative for WM and compositor state.
- Preserve window usability across shell or decorator failure.
- Isolate persistent policy, untrusted notification input, and authorization.
- Avoid cyclic startup, hidden threads, and init-system lock-in.
- Keep toolkit-specific dependencies out of non-visual core services.

## Considered options

1. One monolithic session, service, and shell process.
2. One process per shell surface or feature.
3. A small service set plus one modular shell process and isolated security or
   adapter boundaries.

The monolith has simple startup but poor fault, dependency, and security
isolation. Per-widget processes maximize isolation while multiplying IPC,
startup dependencies, and consistency hazards. The third option gives explicit
authority without unnecessary process count.

## Decision

Propose the component catalog in [the component model](../architecture/component-model.md).
Use separate `prismdrake-session`, `prismdrake-settingsd`,
`prismdrake-notifyd`, and `prismdrake-shell` processes. Keep panel, launcher,
desktop, task, quick-settings, notification, and OSD surfaces as logical modules
within one shell process initially.

Keep authorization prompts in an isolated `prismdrake-polkit-agent`. Make
decorations, portals, control UI, and toolkit adapters optional boundaries as
specified. Defer `prismdrake-lock` until its threat model and platform support
are Accepted. Use init-neutral supervision, bounded restart/backoff, safe mode,
and immutable generation-tagged settings snapshots.

## Consequences

The design introduces versioned IPC and supervision work but prevents UI
failure from becoming settings or WM failure. Core services can remain
toolkit-neutral. Every future process split needs evidence based on security,
fault isolation, dependencies, or measured performance.

## Validation or evidence

The architecture overview assigns one authority to each state domain. The
failure matrix exercises shell, settings, notification, decorator, adapter,
profile-switch, and accessibility failure paths. Implementation evidence must
be collected in PD1 and later milestones.

## Revisit conditions

Revisit before PD1 scaffolding if maintainer review changes component ownership.
Revisit a particular process boundary if prototypes demonstrate a startup
cycle, unacceptable latency, an unsafe trust boundary, or materially simpler
isolation.

## References

- [Project specification sections 9 through 11](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#9-system-context-and-ownership)
- [Architecture overview](../architecture/overview.md)
- [Process model](../architecture/process-model.md)
- [Failure behavior and fallbacks](../architecture/failure-and-fallbacks.md)
