# PD2 performance and accessibility evidence contract

## Claim boundary

This document defines the version-one evidence envelope for `PD2-WP15`. It
does not contain measured results, establish release budgets, or convert a
skipped check into a pass. PD2 retains the project input-acknowledgement target
where it applies, but does not invent endpoint-specific numeric budgets.

The PD1 runners, schemas, collectors, and reference observations remain
unchanged. PD2 measurements are injected into a separate strict document after
collection on the maintained Gentoo reference VM. The repository provides only
a validate-only command and skip-only synthetic contract data for unit tests.

## Versioned artifact

[`pd2-evidence-contract.schema.json`](../../tests/performance/pd2-evidence-contract.schema.json)
requires the exact source revision, identified PD1 reference evidence when a
numeric comparison is made, reference-environment identity, operating
system, architecture, Qt version, compiler versions, display backend, window
manager, clock, sample isolation, and collection method. It records that the
document is not a release budget.

The performance section has closed fields for:

- session ready and the shell's first complete desktop-plus-panel presentation;
- launcher acknowledgement and search response at two or more ordered,
  explicitly recorded catalog sizes;
- catalog refresh, icon decode, icon cache hit, and wallpaper publication;
- notification method latency, notification-center open, and quick-settings
  open;
- profile and do-not-disturb switches;
- output addition and removal reconciliation;
- scheduler wakeups during an idle minute; and
- common action acknowledgement.

An observed metric requires at least three raw samples, a unit, a bounded
method description, and an explicit PD1 comparison. The semantic validator
computes the median. A median above the supplied PD1 value must be marked as a
regression and carry a review explanation. A metric with no equivalent PD1
endpoint must say `not_available` or `not_comparable` and explain why. Omitting
the comparison or labeling a larger median as no regression fails validation.

## Accessibility and internationalization matrix

The artifact requires an ordered entry for every WP15 surface: panel, pinned
task, running task, grouped task, group popup, launcher, launcher results,
desktop-surface exclusion, transient notification, notification center, quick
settings, logout confirmation, output migration, and service-restart focus
recovery.

Every entry records Tab, Backtab, arrows, Enter, Space, Escape, Menu,
Shift+F10, accessible name, role, description, each required state, minimum
target size, high contrast, reduced motion, disabled transparency, text scale,
RTL smoke behavior, and long-string layout. Inapplicable combinations remain
visible as reviewed skips. The top-level accessibility section separately
requires live AT-SPI and screen-reader workflow results, so display-free QML
tests cannot silently stand in for the platform bridge.

The internationalization section separately records Qt translation routing,
RTL and long-string fixtures, locale-aware clock formatting, localized desktop
search, and the fragile-fragment audit. These results describe test outcomes;
they do not store translated application metadata or user content.

## Skips and privacy

Only a small closed set of skip reasons is accepted. Every skip has
`reviewed=true`, a reason code, and a bounded generic explanation. A hermetic
contract fixture is required to contain only skips; any observed measurement
or passing accessibility result in that fixture fails closed. Reference
evidence must use a nonzero source revision and a `reference_vm` environment.

The schema has no fields for filesystem paths, host or user names, process or
thread identifiers, X11 window identifiers, network or D-Bus addresses,
notification bodies, window titles, or application content. The semantic
validator also rejects common private path and address markers, IPv4 addresses,
X11-style hexadecimal identifiers, and private-content-shaped fields. The
redaction block explicitly attests to each excluded category.

## Validation-only workflow

The live adapters must write their independently observed values into the
contract without passing private runtime identifiers through to the artifact.
Validate the transferred artifact with:

```bash
python3 tests/performance/pd2_evidence_contract.py \
  --validate pd2-performance-accessibility.json
```

The artifact path above is illustrative and remains outside source control.
The validator bounds input to 512 KiB, validates the tracked schema, then
applies matrix completeness, privacy, unit, search-series, fixture-purpose,
and PD1-regression semantics.

Run the hermetic contract suite with:

```bash
python3 tests/performance/test_pd2_evidence_contract.py
```

## Live adapter seams

This scaffold intentionally does not instrument `ShellRuntime` or QML. Live
collectors still need reviewed adapters for the session and shell readiness
endpoints, launcher and popup acknowledgement, catalog/icon/wallpaper timing,
notification D-Bus timing, settings switches, output reconciliation, and
common actions. The existing PD1 scheduler-wakeup method may supply an injected
PD2 observation when its scope and revision match; it is not copied or
reinterpreted by this contract.

The existing AT-SPI driver may supply part of the accessibility result after it
is extended to the PD2 surface matrix. A reliable screen-reader workflow and
live output-migration/focus-recovery checks remain distinct adapters. Missing
dependencies or adapters must be emitted as reviewed skips until real evidence
is collected; a skip does not satisfy the corresponding WP15 exit claim.
