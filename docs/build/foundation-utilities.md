# Foundation utilities

The internal `prismdrake-foundation` target provides the display-free PD1
primitives shared by later configuration, settings, session, and diagnostic
work. It remains an in-tree static library, not an installed API or stable ABI.
This slice supports `PD-CONFIG-001`, `PD-CONFIG-003`, `PD-CONFIG-010`,
`PD-SEC-001`, `PD-SEC-008`, `PD-OBS-001`, `PD-OBS-006`, `PD-OBS-007`,
`PD-REL-007`, and `PD-REL-008` at the utility boundary; component code must
still apply its own schema and policy.

## Error contract

`Result<T>` carries either a value or a bounded `Error` containing a stable
category, an actionable message, and recovery guidance. Operational failures
do not require exceptions. Callers must not append private file content,
credentials, notification text, window titles, or other user content to these
fields.

## XDG paths and runtime state

The XDG resolver accepts an injected environment value for deterministic tests
and has a narrow current-process adapter. Absolute XDG home overrides are
honored. Missing or relative non-runtime overrides use the documented
HOME-based defaults; a fallback requires an absolute `HOME` value.
`XDG_RUNTIME_DIR` has no HOME fallback and must be absolute.

Resolution performs no filesystem mutation. A separate read-only validator
checks the existing XDG runtime base and its direct `prismdrake` subdirectory:
both must be real directories rather than symlinks, owned by the expected user,
and exactly mode `0700`. The later session component must create the project
subdirectory deliberately and then validate it; this library does not silently
repair unsafe runtime state.

## Bounded reads

`readBoundedFile` accepts an explicit positive byte limit and returns a
binary-safe string. It opens a file descriptor, requires a regular file,
rejects an already oversized file before reserving its buffer, and performs a
sentinel read when the limit is reached so concurrent growth cannot be accepted
silently. Missing, permission, size, unsupported-type, and I/O failures remain
distinguishable without including file contents in diagnostics.

## Atomic replacement

`writeFileAtomically` creates a unique temporary file in the destination
directory, writes the complete binary-safe payload, synchronizes it, rechecks
the destination, atomically renames it, and synchronizes the directory. It
rejects symbolic-link destinations and redirected parent paths, preserves the
ordinary permission bits of an existing current-user-owned regular file, and
uses explicit permissions for a new file. A failure before rename removes the
temporary and leaves the previous destination authoritative. A directory-sync
failure after rename is reported explicitly because replacement has already
occurred but durability is not confirmed.

## Diagnostics and process primitives

Structured diagnostic events use closed component, severity, profile, and
recovery enums plus a bounded lower-snake-case event identifier. There is no
free-form body, path, or secret field. Rendering therefore emits only reviewed
machine fields.

Published generations are unsigned 64-bit values: zero is unpublished, and
checked advancement refuses overflow. Timeouts use an injected monotonic clock;
the deterministic test clock cannot move backward. Cancellation uses one
explicit shared atomic state with no hidden worker thread. Stable exit-status
mapping converts foundation error categories at future process boundaries.

## Validation

The unit suite covers successful and rejected result values, XDG fallback and
runtime ownership/mode boundaries, missing and oversized inputs, binary data,
atomic replacement and failure preservation, diagnostic identifier bounds and
redaction, interrupted payload writes, generation zero/overflow, deterministic time, concurrent
cancellation, and every error-to-exit mapping. All tests are display-free and
run through the existing GCC, Clang, sanitizer, and CTest paths.
