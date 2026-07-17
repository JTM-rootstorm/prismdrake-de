# Configuration and settings contract

The version-1 format is Accepted by
[ADR 0004](../adr/0004-configuration-format.md). PD0 validated examples and the
draft interface shape. The display-free version-1 parser, recovery loader, and
narrow whole-document writer are implemented in PD1; settings publication and
D-Bus service behavior remain later PD1 work.

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

## TOML normalization

The implementation uses the system-packaged toml++ library. Configuration is
bounded to 1 MiB before parsing and produces only complete immutable C++ domain
values. Syntax, unsupported-version, semantic-validation, and size failures
remain distinct. Diagnostics use canonical schema paths such as
`$.panel.size_px` but never echo rejected values, unknown key names, parser
source excerpts, or private filesystem paths.

The JSON Schema describes the normalized shape rather than requiring a JSON
conversion at runtime. TOML booleans, strings, and integer-only fields retain
their exact kinds. A JSON Schema `number` accepts either a TOML integer through
an explicit checked conversion or a finite TOML floating-point value. Strings
are measured in Unicode code points, arrays preserve input order while
rejecting duplicates, and ordinary tables, inline tables, or dotted keys are
accepted only when toml++ produces the same closed version-1 tree. TOML date,
time, non-finite number, unknown-key, and implicit-coercion cases are rejected.

Version dispatch currently contains only an identity version-1 path. Every
other version fails safely; no version-2 migration is implied.

## Loading and writes

Treat TOML, paths, themes, and referenced assets as untrusted. Bound input size
and collection length; reject duplicate meanings, invalid UTF-8, unsupported
versions, path traversal, wrong types, and out-of-range values with file and
field context. Do not coerce strings to numbers or booleans.

Writes use a same-filesystem temporary file, preserve intended permissions,
flush data as appropriate, and atomically rename only after full validation.
Migrations retain the original input until success and are explicit,
versioned, testable, and recoverable. Packaged defaults remain available.

The packaged default is `defaults/config.toml` beneath Prismdrake's read-only
data directory. The canonical user file is `config.toml` beneath the resolved
XDG configuration directory. Exact validated user TOML may be retained as
`last-known-valid-config.toml` beneath the resolved XDG state directory; it is
revalidated on every load.

Startup selects a valid user document first. A missing user document is an
intentional reset and selects the packaged default rather than resurrecting
stale state. An existing invalid or unreadable user document remains untouched
while the loader tries last-known-valid state and then packaged defaults. A
reload failure returns no candidate so the settings daemon can retain its
current generation; a missing user document on reload explicitly selects the
packaged default.

Last-known-valid promotion is a separate operation available only for a
validated user-derived candidate whose retained TOML still normalizes to the
published values under the same developer policy. The later settings-and-theme
publication transaction must succeed before calling it. Parsing, recovery, and
writes do not assign or consume a generation. A `durability_uncertain` write
result means rename committed the new document but final directory
synchronization failed; callers must not misreport that state as an untouched
old file.

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
