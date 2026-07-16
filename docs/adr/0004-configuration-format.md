# ADR 0004: Configuration format and snapshot model

- **Status:** Proposed
- **Date:** 2026-07-15
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

Propose `$XDG_CONFIG_HOME/prismdrake/config.toml` with explicit
`schema_version = 1`, strict documented domains, and profile identifiers
`lustre` and `forge`. Use XDG state, cache, runtime, and data locations for their
defined purposes. Validate all input and write atomically while preserving the
last valid input and packaged defaults.

Resolve settings and theme data into immutable snapshots with one monotonically
increasing generation. Publish only complete generations. Propose the narrow,
explicitly draft `org.prismdrake.Settings1` interface; do not expose generic
arbitrary-key mutation.

## Consequences

Implementations need a TOML parser, strict semantic validation, migration,
atomic filesystem utilities, and bounded D-Bus behavior. JSON Schema documents
the normalized shape, but PD1 must decide how TOML-to-schema validation is
implemented without hidden coercion. Unknown versions intentionally fail.

## Validation or evidence

PD0 parses all TOML examples, checks their normalized domains and constraints,
parses D-Bus XML, rejects invalid profiles and unsupported versions in negative
self-tests, and verifies the canonical namespace. Runtime transaction behavior
requires PD1 tests.

## Revisit conditions

Revisit if implementation shows ambiguous TOML mapping, inadequate migration,
unacceptable parser dependencies, or a safer equally recoverable format. A
format change requires a versioned migration and preservation of previous valid
configuration.

## References

- [Configuration contract](../architecture/configuration.md)
- [Configuration schema](../../schemas/prismdrake-config.schema.json)
- [Draft Settings1 interface](../../interfaces/dbus/org.prismdrake.Settings1.xml)
- [Project specification section 13](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#13-configuration-state-and-schemas)

