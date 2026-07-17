# PD1 launcher-model evidence

**Date:** 2026-07-17

**Reference environment:** host plus `prismdrake-vm`

**Implementation revisions:** `e5c6f05` through `1fc2c54`

**Evidence checkpoint:** `1fc2c54`

## Scope and status

This report records the display-free desktop-entry discovery, catalog, search,
execution-planning, and controlled launch model completed for PD1-WP11. The
implementation follows the XDG Base Directory and Desktop Entry specifications,
publishes bounded immutable model state, and reaches `execve()` without composing
or invoking a shell command.

This is a PD1 engineering model, not the production launcher promised for PD2.
It does not add launcher presentation, keyboard navigation, icon lookup, desktop
actions, a production worker or search service, recent items, online or file
providers, package-manager integration, D-Bus application activation, or a
configured terminal selection. The caller still owns worker scheduling, current
generation selection, UI state presentation, and launch policy.

The work preserves [ADR 0002](../adr/0002-component-and-process-model.md): the
launcher remains a logical module of `prismdrake-shell`, and this model does not
create a launcher daemon. It follows the C++20, CMake, CTest, and system GoogleTest
baseline accepted by [ADR 0008](../adr/0008-build-language-and-testing-baseline.md).
[ADR 0005](../adr/0005-standards-and-glasswyrm-integration.md) remains Proposed;
using its standards-first direction here does not accept any proposed native
Glasswyrm interface.

## Requirement mapping

| Requirement | PD1 evidence | Remaining production boundary |
|---|---|---|
| `PD-LAUNCH-001` | Ordered application roots follow XDG data-directory precedence. Discovery derives Desktop Entry file identifiers, applies higher-root and tombstone shadowing, parses `Type=Application`, locale, visibility, `TryExec`, and execution metadata, and publishes only visible and eligible indices. | Icon-theme lookup, installed-session integration, filesystem monitoring, and a production refresh owner remain later work. |
| `PD-LAUNCH-002` | Local search covers the selected localized `Name`, `GenericName`, `Keywords`, and `Categories` fields. An ordinary settings application represented by a desktop entry is searchable through the same fields. | There is no distinct settings-destination index or provider, so this model alone does not complete the full production requirement. |
| `PD-LAUNCH-003` | `Exec` is parsed and expanded into bounded argument vectors. Actual and terminal executables are resolved from explicit inputs, launch plans contain exact argv, working directory, and environment values, and the detached boundary calls `execve()` directly. | D-Bus activation and desktop actions require separate paths. Production integration must continue to keep this synchronous boundary off the UI thread. |
| `PD-LAUNCH-007` | Root precedence, relative-path traversal, identifier collisions, eligibility, ranking, truncation, and tie-breaking have stable ordering under identical inputs. Tests prove final discovery, catalog, and search results do not depend on pull-slice size or incoming visible-entry order. | Locale-aware collation is not claimed; any later collation policy must preserve a documented deterministic fallback. |
| `PD-LAUNCH-008` | Search snapshots distinguish `loading`, `results`, `emptyCatalog`, `noResults`, and `error`, and carry catalog and request generations for stale-publication rejection. | No visual or assistive-technology presentation exists yet, so the user-facing state requirement remains a production UI responsibility. |
| `PD-PERF-006` | Discovery, catalog construction, and search are caller-driven operations with explicit per-call work budgets and cancellation. They create no threads, and search performs no filesystem, display, provider, or process work. | Discovery does perform bounded synchronous filesystem work inside each pull. The shell must schedule pulls on an explicitly owned worker and publish only current generations; this PD1 library does not supply that dispatcher. |

`PD-LAUNCH-004`, `PD-LAUNCH-005`, and `PD-LAUNCH-006` are not claimed by this
slice. Omitting online, file, history, and recent-item providers avoids those
privacy surfaces in PD1, but it does not implement their future disclosure,
keyboard, storage, or clearing requirements.

## Standards-based discovery and visibility

`ApplicationPaths` implements the application-root portion of the
[XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/latest/).
It places `$XDG_DATA_HOME/applications` first, uses
`$HOME/.local/share/applications` when the data-home value is absent or relative,
then processes `$XDG_DATA_DIRS` in order with the standard
`/usr/local/share:/usr/share` default. Empty and relative system-list components
are ignored, normalized duplicate application directories retain their first
precedence position, and no filesystem access occurs during this resolution.

`DesktopEntryDiscovery` then applies the
[Desktop Entry Specification](https://specifications.freedesktop.org/desktop-entry/latest/)
file-identifier and precedence model:

- candidate relative paths are processed in deterministic lexical order;
- directory separators become hyphens in the bounded desktop-file identifier;
- the first candidate to claim an identifier shadows lower-precedence roots,
  including a malformed file or a `Hidden=true` tombstone;
- a flattened identifier collision within one root retains the lexically first
  relative path;
- regular-file and root symlinks are permitted, while nested directory symlinks
  are not traversed; and
- incomplete or mutated roots fail conservatively rather than publishing an
  enumeration-dependent partial result and then falling through to a lower root.

The parser accepts bounded, NUL-free UTF-8 Desktop Entry documents for supported
versions 1.0 through 1.5. It validates document structure, known and extension
keys, booleans, escapes, list shapes, locale identifiers and fallback, activation
requirements, and unsafe control content. It selects localized `Name`,
`GenericName`, `Comment`, `Icon`, and `Keywords` values using the caller-supplied
messages locale. It retains the PD1 fields needed for discovery and launch but
does not preserve unknown values or round-trip the source document.

Visibility evaluation gives `Hidden` and `NoDisplay` priority, then applies
`OnlyShowIn` and `NotShowIn` to an ordered, bounded `XDG_CURRENT_DESKTOP` context.
The model retains a closed reason for every parsed entry while publishing only
visible indices. Catalog construction evaluates optional `TryExec` values into
closed eligible, missing, non-regular, and non-executable outcomes. Search can
consume only catalog-published eligible indices, so a visible but unavailable
`TryExec` entry cannot reappear through ranking.

`TryExec` is only an eligibility check. The actual `Exec` executable is resolved
again while constructing the process launch plan so catalog availability is not
treated as process authority.

## Bounded incremental model

Discovery publishes immutable cumulative snapshots after caller-selected work
batches. A cancelled discovery pull stops before additional work and may resume
later with a fresh token. Catalog and search cancellation is terminal for that
operation, preventing stale partial work from later being published as current.
Catalog snapshots bind a nonzero generation to one immutable discovery snapshot;
search snapshots bind both catalog and request generations. Callers must discard
publications whose generation is no longer current.

Search requires every normalized query token to match at least one selected
field. Ranking prefers exact, prefix, word-prefix, then substring shapes, and
within each shape prefers name, generic name, keyword, then category. Phrase
rank, aggregate and worst token rank, folded localized name, desktop-file
identifier, and entry index provide a total deterministic order. Empty queries
use the deterministic name and identifier order.

ASCII letters are folded to lowercase. Non-ASCII UTF-8 is compared exactly; the
PD1 implementation makes no Unicode normalization, locale-sensitive case-folding,
stemming, or collation claim. This limitation is explicit rather than depending
on the host locale.

Representative hard limits at the evidence checkpoint are:

| Boundary | Limit |
|---|---|
| Complete XDG application-path environment | 64 KiB, with at most 128 data-directory entries |
| One desktop-entry file | 1 MiB, with bounded line, group, entry, list, locale, and code-point counts |
| Discovery | At most 128 roots, 16,384 published entries, 256 diagnostics, and caller-bounded nodes, directories, candidates, and depth |
| One catalog or search advance | 4,096 work units |
| Search query | 1 KiB, 256 code points, and 32 tokens |
| Published search results | 512 |
| Raw or expanded `Exec` data | 256 arguments and 1 MiB aggregate argument or target vectors |
| Process environment | 4,096 entries and 1 MiB aggregate data |
| Final argv and environment envelope | 1 MiB including pointer storage |
| Detached launch handshake | Two seconds on a monotonic deadline |

All compile-time discovery maxima also have positive caller-tunable limits no
larger than the compiled envelope. Limit, syntax, I/O, and eligibility failures
use typed, static diagnostics rather than embedding paths, desktop-file contents,
query text, arguments, environment values, or other rejected input.

## Safe execution pipeline

`DesktopExec` applies the Desktop Entry `Exec` quoting and field-code rules after
the parser has decoded general desktop-entry string escapes. It supports typed
local-file and URI expansion, validates `%f`, `%F`, `%u`, `%U`, `%i`, `%c`, `%k`,
and `%%`, removes the specification's deprecated codes, rejects unsupported or
malformed codes, and never rescans replacement data. The executable token cannot
contain a field code or environment assignment. Unquoted reserved syntax and
unsafe control content are rejected, while correctly quoted shell metacharacters
remain literal argument bytes.

Executable lookup accepts an absolute path or a bare executable name. It uses an
explicit PATH string and absolute lookup base, resolves empty and relative PATH
components against that base, and never reads ambient PATH or process working
directory state. Candidates must be regular files with execute permission.
Symlinks to regular executable files are followed for validation without
canonicalizing the lexical path published in the plan.

`ProcessLaunchPlan` is inert. It resolves the selected application executable,
normalizes an absolute working directory, validates the caller-supplied
environment, rejects duplicate non-`PWD` variable names, and publishes exactly
one `PWD` matching that directory. A `Terminal=true` entry fails closed when no
terminal policy is configured. When a policy exists, its executable and prefix
are validated separately and the application argv is appended as literal
arguments; no terminal command string is composed. D-Bus-activatable entries are
rejected from this process path for a future dedicated activation implementation.

`launchDetachedApplication()` is the only process boundary. It revalidates the
complete plan, prepares C-compatible argv and environment arrays before forking,
uses close-on-exec handshake pipes, creates a new session through `setsid()` and a
double fork, resets the child signal mask and dispositions, enters the working
directory, attaches standard streams to `/dev/null`, closes unintended file
descriptors, and calls `execve()` with the exact plan. The broker is reaped on
success and error paths. Timeout cleanup kills the detached session and reaps the
broker; no PID or process ownership is returned to the shell.

The call is synchronous and may occupy its worker for the full two-second
handshake deadline. Its API therefore requires an explicitly owned worker thread.
Cancellation is observed only before the first fork commit point; after that
point the bounded handshake reaches a definite exec or failure outcome. Success
means that the close-on-exec handshake observed `execve()` acceptance, not that
the launched application later created a window or remained healthy.

## Controlled launch fixture

The test-only `prismdrake-application-launch-fixture` serializes its exact argv,
working directory, and environment to a caller-selected temporary file. The
detached test target supplies its path through a test-only environment variable
and compiles a pre-exec delay hook only into the dedicated timeout test binary.
Production launcher code does not select or execute this fixture automatically.

The seven detached-boundary tests prove:

- literal metacharacters, command substitutions, quotes, backslashes, globs, and
  whitespace arrive unchanged and create no shell side effect;
- the fixture observes the exact argv, working directory, and environment;
- missing and unlinked executables and a missing working directory fail without
  executing the fixture;
- pre-fork cancellation performs no launch;
- malformed and oversized plans fail with redacted diagnostics;
- sixteen repeated launches leave no waitable broker children; and
- the controlled pre-exec delay reaches the two-second timeout, kills the
  detached session before exec, and leaves neither output nor a waitable child.

Two pipeline tests additionally exercise the complete synthetic desktop-entry,
Exec expansion, launch-plan, and detached-execution path, and prove that a
terminal-required entry fails before launch when no terminal policy is
configured.

The broader 168-test launcher set also covers XDG defaults and hostile literal
path content; malformed, oversized, duplicate, localized, and invalid-UTF-8
desktop entries; visibility and precedence; symlink policy and directory
mutation; cancellation and immutable snapshots; `TryExec` filtering; all
selected search fields and states; deterministic truncation; strict field-code
expansion; terminal policy; working-directory and environment normalization;
and redaction at every launch-planning boundary.

## Validation evidence

At `1fc2c54`, the clean host warnings-as-errors build completed all 415 CTest
registrations with zero failures. Environment-gated display lanes reported only
their expected skips. The detached-application and complete-pipeline targets
passed all nine focused tests with GCC and Clang. Their AddressSanitizer plus
UndefinedBehaviorSanitizer run also passed all nine tests; LeakSanitizer was
disabled because the host sandbox cannot support that check reliably.

The exact-source matrix recorded:

- **Exact source archive:** `/mnt/shared/prismdrake-wp11-final-1fc2c54.tar.gz`.
- **SHA-256:** `c32becdcb588e61b9228e7c7f0b9cabbbf9e751256692436f4f82d025010df71`.
- **Gentoo GCC 15.3.0:** all 415 registrations passed; the one test requiring a
  non-root caller was skipped because the VM test user is root.
- **Gentoo Clang 22.1.8:** all nine detached-boundary and pipeline tests passed.
- **Gentoo GCC ASan and UBSan:** all nine focused tests passed with LeakSanitizer
  enabled.
- **GitHub Actions:** run `29610542916` passed repository contracts,
  clang-format 18, GCC, Clang 18, and CTest.

The exact archive was checksum-verified from the host share inside the guest,
extracted to a fresh guest work directory, configured with warnings as errors,
and removed after the matrix. All display and isolated-D-Bus integration tests
passed in the VM; no host build tree or untracked plan content entered the
archive.

## Impact and remaining boundary

- **Accessibility:** the model exposes deterministic ordering and explicit
  loading, empty, no-result, result, and error states that a UI can announce,
  but it creates no controls, focus order, accessible names, keyboard behavior,
  target sizes, contrast, or motion. `PD-LAUNCH-005` and the launcher UI portions
  of the accessibility requirements remain open.
- **Security and privacy:** desktop files, environment values, queries, paths,
  argv, and process plans are treated as untrusted and bounded. Diagnostics are
  redacted. There is no implicit shell, online provider, recent-item store,
  history provider, package-manager provider, or query logging in this model.
  The explicit launch environment remains caller-controlled and must not contain
  secrets that policy does not intend to pass to the application.
- **Dependencies and packaging:** the model uses the existing foundation library,
  C++ standard library, and Linux/POSIX process interfaces. It adds no Qt, GTK,
  GNOME desktop-stack, Glasswyrm, network, package-manager, or launcher-daemon
  runtime dependency.
- **Standards baseline:** discovery and execution follow XDG and Desktop Entry
  contracts. Icon metadata is parsed but icon-theme lookup is not implemented,
  so this slice does not claim all of `PD-INT-007`.
- **Glasswyrm integration:** no window-manager or compositor state is read or
  changed, no process-name capability detection is used, and no `GW_*` interface
  is implemented or advertised.
- **Compatibility and migration:** all C++ interfaces remain internal PD1 model
  boundaries with no public ABI promise. The model has no persistent launcher
  state or migration format.

Production work must connect these operations to a bounded shell-owned worker,
discard stale generations, present fully keyboard-accessible states, resolve
icons, define filesystem monitoring and refresh policy, add the separately
reviewed D-Bus activation path, and decide terminal configuration. Desktop
actions and richer providers remain outside this PD1 work package.
