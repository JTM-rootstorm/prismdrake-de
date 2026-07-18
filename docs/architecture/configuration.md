# Configuration and settings contract

The TOML, XDG, validation, atomic-write, and immutable-snapshot model is
Accepted by [ADR 0004](../adr/0004-configuration-format.md). Configuration and
runtime-snapshot version 2, version-1 in-memory normalization, and PD2 state
ownership are Accepted by
[ADR 0009](../adr/0009-pd2-state-and-control-contracts.md). The narrow settings
and complete-snapshot D-Bus transport remains Experimental without a stability
guarantee.

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

`schema_version` is a positive integer. PD2 source readers support exactly
versions `1` and `2`; normalized typed configuration is always version 2.
Unsupported versions fail before any state is published. `profile` is exactly
`lustre` or `forge`. The strict
[JSON schema](../../schemas/prismdrake-config.schema.json) defines appearance,
panel, launcher, notifications, desktop, integration, accessibility, keyboard,
and developer domains.

Version 2 adds these complete fields:

```toml
[panel]
output_policy = "primary" # "primary", "all", or "named"
outputs = []

[desktop]
wallpaper_source = ""
background_color = "#182033"
wallpaper_mode = "fill"
icons_enabled = false
```

`panel.outputs` is an ordered unique set of validated output names and is used
only by `named`. `wallpaper_source` is empty or a bounded string of at most 128
Unicode code points. Empty selects no image. The desktop and wallpaper
implementation performs separately specified local-file and image validation
before presentation.
`background_color` is exactly `#RRGGBB`. Desktop icons remain disabled in PD2.
The panel output and surface effects are defined in
[surface placement](surface-placement.md).

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
accepted only when toml++ produces the same closed tree for the declared source
version. TOML date, time, non-finite number, unknown-key, and implicit-coercion
cases are rejected.

Version dispatch has two closed paths:

- Version 2 validates directly.
- Version 1 validates against its original closed shape, then normalizes in
  memory: an empty `panel.outputs` becomes `output_policy = "primary"`; a
  nonempty list becomes `output_policy = "named"`; `wallpaper_source` becomes
  `""`; and `background_color` becomes `"#182033"`.

The normalized value reports schema version 2. Reading or loading version 1
never rewrites source bytes. An explicit whole-document write serializes one
complete version-2 document. Unknown source versions fail safely.

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

Promotion retains the exact validated source bytes, including version-1 bytes.
Only the explicit validate-and-write path performs a migration to complete
version 2. The original input remains available until that atomic write
succeeds, preserving `PD-CONFIG-003`, `PD-CONFIG-006`, and `PD-PKG-010`.

## Atomic publication

1. Load a candidate configuration and referenced token documents.
2. Validate schemas, types, paths, assets, and capability fallbacks.
3. Resolve packaged defaults and user overrides.
4. Construct immutable settings and theme snapshots.
5. Assign one monotonically increasing generation.
6. Serialize the complete Experimental runtime payload within its fixed bound.
7. Atomically swap the typed snapshot and its immutable serialized payload.
8. Publish a notification naming the complete generation and changed domains.
9. Consumers fetch and atomically replace their previous complete generation.

Validation, resolution, generation-allocation, or serialization failure before
the swap leaves the prior generation authoritative and consumes no generation
number. Once the swap succeeds, the new generation is authoritative. Signal
delivery or last-known-valid persistence failure is reported but does not
restore an older generation number. PD1 has no consumer-acknowledgement or
two-phase rollback method. The service retains only the current and immediately
previous snapshots internally; a captured immutable snapshot may outlive that
retention through shared ownership.

A restarted component fetches the latest complete generation. It never rebuilds
authority from a sequence of per-key events.

## Experimental runtime snapshot

The strict
[`prismdrake-runtime-snapshot` schema](../../schemas/prismdrake-runtime-snapshot.schema.json)
defines version 2 of the internal complete-snapshot payload. It is an
Experimental transport contract, not a stable ABI. The canonical UTF-8 JSON
payload is limited to 1 MiB and contains:

- one nonzero generation and the authoritative top-level profile identifier;
- a closed `settings` object containing every normalized configuration domain
  except the deliberately non-duplicated top-level profile;
- the complete resolved theme candidate, effective accessibility values,
  resolved materials, fallback presentation, and stable warnings;
- configuration and theme provenance as closed logical source identifiers,
  never filesystem paths; and
- closed validation-warning and restart-domain identifiers.

The top-level runtime `schema_version` and nested
`settings.schema_version` are both `2`; the independently versioned resolved
theme remains schema version `1`. The serialized payload is built before
publication and retained with the typed snapshot. Its generation is identical
to the typed generation and later D-Bus reply. Unknown schema versions, unknown
properties, inconsistent profile/theme identity, duplicate identifiers, and
out-of-bound collections or values are rejected. Original TOML, rejected
values, parser excerpts, and user or packaged filesystem paths are never
serialized.

## Experimental D-Bus boundary

[`org.prismdrake.Settings1`](../../interfaces/dbus/org.prismdrake.Settings1.xml)
is an Experimental versioned interface for reading the active profile and
generation, requesting a validated runtime profile switch or reload, validating
bounded candidate TOML, and observing complete generation changes. The same
object exports the internal `org.prismdrake.SettingsSnapshot1` interface so
Prismdrake consumers can fetch one complete immutable, schema-versioned runtime
snapshot rather than reconstructing state from per-key events. Neither
interface has a generic “set key” method or caller-selected path.

Generation identity includes the D-Bus well-known-name ownership epoch. Clients
invalidate cached state on owner changes or disconnects, then fetch the current
complete snapshot after reacquisition. A signal and subsequent fetch may race;
the later complete generation wins. There is no acknowledgement or rollback
method. Callers must handle fixed typed errors, bounded timeouts, service
disappearance, and name reacquisition. Acceptance of ADR 0004 and implementation
of the PD1 prototype do not stabilize either interface or guarantee wire
compatibility.

PD2 settingsd, shell clients, and session readiness request runtime snapshot
version 2 only. Runtime version 1 and unknown transport versions are rejected;
source configuration version 1 remains supported only through the in-memory
normalization path above.

The Experimental shell client implements this boundary without Qt D-Bus or a
polling timer. It installs owner and generation matches before its initial
owner query, drives the selected sd-bus provider from Qt socket notifiers and
the provider's monotonic deadline, and treats every generation signal as a hint
to fetch one newer complete snapshot. Received JSON remains private to a strict
bounded inverse parser; only an immutable typed snapshot crosses into shell
theme and presentation adapters.

The configured notifications do-not-disturb value is the settingsd-owned
baseline. `prismdrake-notifyd` may layer an ephemeral effective override without
writing this document, as defined in
[PD2 state ownership](pd2-state-ownership.md#do-not-disturb-authority).

Relevant requirements: `PD-CONFIG-001` through `PD-CONFIG-010`, `PD-SEC-001`,
`PD-SEC-008`, `PD-REL-002`, `PD-REL-008`, and `PD-API-001` through
`PD-API-005`.
