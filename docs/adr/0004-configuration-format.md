# ADR 0004: Configuration format and snapshot model

- **Status:** Accepted
- **Date:** 2026-07-16
- **Owners:** Prismdrake maintainers

## Context

Settings need a versioned, human-editable source before implementations invent
incompatible keys. The service must reject untrusted input safely and publish
profile changes without mixed generations.

## Decision drivers

- Human readability and explicit types.
- Strict, versioned validation and recoverable migration.
- XDG directory semantics and atomic writes.
- Accessibility preferences independent from profile defaults.
- Narrow typed IPC without arbitrary remote key mutation.

## Considered options

1. TOML user configuration plus a normalized version-1 contract.
2. JSON user configuration using the same conceptual schema.
3. A D-Bus-only store with no primary editable file.

TOML offers readable comments and typed tables. JSON aligns directly with JSON
Schema but is less friendly for hand editing and comments. A D-Bus-only model
centralizes writes but makes recovery, review, and offline editing harder and
risks turning the interface into an unrestricted mutation API.

## Decision

Use `$XDG_CONFIG_HOME/prismdrake/config.toml` with explicit
`schema_version = 1`, strict documented domains, and profile identifiers
`lustre` and `forge`. Use XDG state, cache, runtime, and data locations for their
defined purposes. Validate all input and write atomically while preserving the
last valid input and packaged defaults.

Resolve settings and theme data into immutable snapshots with one monotonically
increasing generation per D-Bus owner epoch. Publish only complete generations.
Implement the narrow `org.prismdrake.Settings1` interface and internal
`org.prismdrake.SettingsSnapshot1` complete-snapshot transport as explicitly
Experimental PD1 contracts; this decision does not stabilize either interface.
Do not expose generic arbitrary-key mutation, consumer acknowledgement, or a
two-phase rollback protocol.

## Consequences

Implementations need a TOML parser, strict semantic validation, migration,
atomic filesystem utilities, bounded D-Bus behavior, and a bounded complete
runtime serialization. JSON Schema documents the normalized source and runtime
shapes, while the implementation validates TOML kinds explicitly without
hidden coercion. Unknown source or snapshot versions intentionally fail.

Once a complete generation is published, it remains authoritative regardless
of signal delivery or consumer state. Failed candidates do not consume a
generation. Consumers fetch and atomically replace complete snapshots; owner
changes invalidate cached generation assumptions and begin a new epoch.

## Validation or evidence

PD0 parsed the TOML examples, checked their normalized domains and constraints,
parsed the initial D-Bus XML, rejected invalid profiles and unsupported versions
in negative self-tests, and verified the canonical namespace. PD1 implements
the display-free loader, recovery and atomic-write boundaries, combined
settings/theme publication, pre-publication bounded runtime serialization, and
Experimental settings and complete-snapshot interface contracts with focused
transaction and negative validation tests.

## Revisit conditions

Revisit if implementation shows ambiguous TOML mapping, inadequate migration,
unacceptable parser dependencies, or a safer equally recoverable format. A
format change requires a versioned migration and preservation of previous valid
configuration.

## References

- [Configuration contract](../architecture/configuration.md)
- [Configuration schema](../../schemas/prismdrake-config.schema.json)
- [Experimental settings interfaces](../../interfaces/dbus/org.prismdrake.Settings1.xml)
- [Project specification section 13](../PRISMDRAKE_PROJECT_SPECIFICATION.md#13-configuration-state-and-schemas)
