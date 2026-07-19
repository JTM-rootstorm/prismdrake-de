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

## Collection and validation workflow

[`collect_pd2_live_evidence.py`](../../tests/performance/collect_pd2_live_evidence.py)
assembles independently observed values without passing private runtime
identifiers through to the artifact. Every observer response is bound to the
exact source revision, reference-environment identity, and `pd2-live-v1`
method identity. The collector rejects missing or mismatched bindings, stale
reviewed-skip policy, observer diagnostics, incomplete cleanup, and Qt below
6.11. It validates the complete contract before atomically creating a private
mode-0600 artifact.

The command requires exact environment, performance, accessibility,
internationalization, and cleanup observer executables. Search catalog sizes,
PD1 reference identity, limitations, and each reviewed skip are explicit
arguments. Observer runtime values such as the isolated display and bus remain
in an allow-listed child environment and are never copied into evidence. The
performance observer also receives only explicit paths to the bound runtime
timing artifact, production asset runner, PD1 model artifact, and assembled
idle series.

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
python3 tests/performance/test_pd2_live_evidence.py
python3 tests/performance/test_pd2_runtime_performance.py
python3 tests/performance/test_pd2_asset_performance.py
python3 tests/performance/test_pd2_idle_series.py
python3 tests/performance/test_pd2_reference_observer.py
```

## Live adapter seams

The collector provides closed command seams for all 15 performance groups,
including the parameterized search, icon-cache, and output-reconciliation
endpoints. It requests all 364 ordered surface checks independently, including
RTL and long-string observations, then separately requests live AT-SPI, a
screen-reader workflow, and the six internationalization checks. A command
must emit one bounded JSON envelope and no standard-error diagnostics. No shell
is involved.

The reference performance observer has a closed route for every requested
performance endpoint. With two controlled search sizes it emits 18 observed
measurements from four sources:

- the installed-session collector supplies session readiness, complete shell
  presentation, launcher, notification method, notification center, quick
  settings, profile, DND, output add, output remove, and common-action timing;
- the PD1 model runner supplies catalog refresh and one search series sample
  set for each requested supported fixture size;
- the production asset runner supplies icon decode, exact-key cache hit, and
  wallpaper decode-to-context-thread publication timing; and
- the assembled scheduler series supplies repeated whole-minute received
  wakeup totals across the exact session, settingsd, and shell roles.

The installed demonstration artifact is authoritative for exact source
revision, installed executable hashes, workflow postconditions, and cleanup.
It contains no timing data. `pd2_runtime_performance.py` therefore repeats that
installed workflow with a fresh isolated session per trial and measures the
bounded postcondition acknowledgements with a monotonic clock. Asset timing
uses the production `IconResolver` and `WallpaperLoader`, not a QML or model
surrogate. The runtime collector deliberately does not also report wallpaper
timing; the production asset runner is authoritative for that endpoint.

Collect at least three installed-runtime trials after producing an exact
installed-demonstration artifact for the same revision:

```bash
python3 tests/performance/pd2_runtime_performance.py \
  --source-revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system \
  --installed-demo-evidence "$PWD/pd2-installed-demo.json" \
  --controlled-window-fixture "$PWD/pd2-controlled-window.desktop" \
  --iterations 3 \
  --output "$PWD/pd2-runtime-performance.json"
```

Collect the strict 60-second idle artifact through the same-tree workflow in
the [PD1 performance-evidence method](pd1-performance-evidence-method.md#idle-wakeups)
three or more times. Then assemble the repeated role totals without retaining
paths, PIDs, thread IDs, XIDs, or diagnostic payloads:

```bash
python3 tests/performance/pd2_idle_series.py \
  --source-revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system \
  --input "$PWD/pd2-idle-1.json" \
  --input "$PWD/pd2-idle-2.json" \
  --input "$PWD/pd2-idle-3.json" \
  --output "$PWD/pd2-idle-series.json"
```

Build the production and model runners, bind all four performance sources, and
choose at least two ordered model-supported search sizes. The model runner's
closed controlled sizes are 16, 64, and 256:

```bash
cmake --build build/gcc-debug --target \
  prismdrake-pd1-performance-evidence prismdrake-pd2-asset-performance
env \
  PRISMDRAKE_PD2_PERFORMANCE_RUNNER="$PWD/build/gcc-debug/tests/performance/prismdrake-pd1-performance-evidence" \
  PRISMDRAKE_PD2_ASSET_PERFORMANCE_RUNNER="$PWD/build/gcc-debug/tests/performance/prismdrake-pd2-asset-performance" \
  PRISMDRAKE_PD2_RUNTIME_PERFORMANCE_EVIDENCE="$PWD/pd2-runtime-performance.json" \
  PRISMDRAKE_PD2_IDLE_SERIES="$PWD/pd2-idle-series.json" \
  PRISMDRAKE_PD2_PD1_EVIDENCE="$PWD/pd1-performance.json" \
  python3 tests/performance/collect_pd2_live_evidence.py \
    --source-revision "$(git rev-parse HEAD)" \
    --environment-id gentoo-amd64-qemu-system \
    --pd1-reference-revision PD1_REVISION \
    --pd1-evidence-id pd1-reviewed-reference \
    --search-catalog-size 16 --search-catalog-size 64 \
    --limitation "Reference VM measurements do not define release budgets" \
    --observer "$PWD/tests/performance/pd2_reference_observer.py" \
    --accessibility-observer "$PWD/tests/accessibility/pd2_accessibility_observer.py" \
    --output "$PWD/pd2-performance-accessibility.json"
```

`PD1_REVISION` above is a placeholder for the exact lowercase PD1 evidence
revision. The remaining documented observer dependencies and any reviewed
accessibility skips must also be supplied for a complete live collection.
The dedicated accessibility observer uses the VM's GI `Atspi` binding. The RTL
and expanded-string routes pass only when CTest reports the complete exact
five-test and six-test selections respectively; a partial or broadened regex
selection fails closed.

Model endpoints compare only with the matching validated PD1 model series.
Installed-runtime, production-asset, and idle-wakeup endpoints report
`not_available` unless an actual matching validated multi-sample PD1 baseline
is supplied; they never manufacture a historical value. That status is an
explicit claim boundary, not a regression pass.

Reliable screen-reader automation and every runtime observer must still be
exercised on the reference VM. A genuinely unavailable dependency or
unautomatable state may be represented only by an exact reviewed skip declared
before collection. A skip does not satisfy the corresponding WP15 exit claim.
