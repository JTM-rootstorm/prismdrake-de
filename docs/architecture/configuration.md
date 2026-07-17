# Configuration and settings contract

The version-1 format is Accepted by
[ADR 0004](../adr/0004-configuration-format.md). PD0 validated examples and the
draft interface shape; settings loading and D-Bus service behavior remain PD1
implementation work.

## Locations

| Data | Accepted XDG location |
|---|---|
| User configuration | `$XDG_CONFIG_HOME/prismdrake/config.toml` |
| User state | `$XDG_STATE_HOME/prismdrake/` |
| Cache | `$XDG_CACHE_HOME/prismdrake/` |
| Runtime data | `$XDG_RUNTIME_DIR/prismdrake/` when required |
| Packaged defaults | Distribution-appropriate read-only XDG data directory |

Implementations use the XDG-specified fallback when an environment variable is
unset; they never hard-code a home directory. Configuration contains no
credentials or secrets.

## Version and domains

`schema_version` is a positive integer; PD0 supports exactly version `1`.
Unsupported versions fail before any state is published. `profile` is exactly
`lustre` or `forge`. The strict [JSON schema](../../schemas/prismdrake-config.schema.json)
defines appearance, panel, launcher, notifications, desktop, integration,
accessibility, keyboard, and developer domains.

Accessibility preferences are user state independent of profile defaults.
Switching profiles must preserve explicit text scale, high contrast, reduced
motion, transparency, focus emphasis, animation scale, and minimum target size
unless the user changes them.

Developer capability overrides are disabled by default and ignored in a
production build unless an explicit, separately designed developer mode enables
them. They are never capability evidence for normal operation.

## Loading and writes

Treat TOML, paths, themes, and referenced assets as untrusted. Bound input size
and collection length; reject duplicate meanings, invalid UTF-8, unsupported
versions, path traversal, wrong types, and out-of-range values with file and
field context. Do not coerce strings to numbers or booleans.

Writes use a same-filesystem temporary file, preserve intended permissions,
flush data as appropriate, and atomically rename only after full validation.
Migrations retain the original input until success and are explicit,
versioned, testable, and recoverable. Packaged defaults remain available.

## Atomic publication

1. Load a candidate configuration and referenced token documents.
2. Validate schemas, types, paths, assets, and capability fallbacks.
3. Resolve packaged defaults and user overrides.
4. Construct immutable settings and theme snapshots.
5. Assign one monotonically increasing generation.
6. Publish a notification naming the complete generation and changed domains.
7. Consumers atomically replace their previous complete generation.
8. Retain the prior generation until critical consumers acknowledge or time
   out; on failure, roll back and report diagnostics.

A restarted component fetches the latest complete generation. It never rebuilds
authority from a sequence of per-key events.

## Draft D-Bus boundary

[`org.prismdrake.Settings1`](../../interfaces/dbus/org.prismdrake.Settings1.xml)
is a draft versioned interface for reading the active profile and generation,
requesting a validated profile switch or reload, validating bounded candidate
TOML, and observing complete generation changes. It has no generic “set key”
method. Callers must handle typed errors, timeouts, service disappearance, and
name reacquisition. Acceptance of ADR 0004 does not freeze its ABI.

Relevant requirements: `PD-CONFIG-001` through `PD-CONFIG-010` and
`PD-API-001` through `PD-API-005`.
