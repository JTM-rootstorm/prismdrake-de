# PD2 performance and accessibility evidence contract

## Claim boundary

This document defines the version-one evidence envelope for `PD2-WP15`. It
does not contain measured results, establish release budgets, or convert a
skipped check into a pass. PD2 retains the project input-acknowledgement target
where it applies, but does not invent endpoint-specific numeric budgets.

The PD1 runners, schemas, collectors, and reference observations remain
unchanged. PD2 measurements are injected into a separate strict document after
collection on the maintained Gentoo reference VM. The repository provides
source-bound live collectors and runtime observers, validate-only commands, and
skip-only synthetic contract data for unit tests. The collectors become
evidence only when run against the reviewed clean revision in that environment.

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
Every common-action acknowledgement sample must also remain at or below the
project's 100 ms target. This target is enforced independently of PD1
comparison status so an unavailable historical baseline cannot hide a slow
acknowledgement.

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
mode-0600 artifact. Publication is create-only: a retained or concurrently
created artifact is never replaced.

Before invoking an observer, the collector uses the fixed system Git executable
to require that its own repository is the clean checkout at the requested
revision. Every observer executable must resolve to a nonsymlinked file inside
that checkout. Evidence inputs and outputs therefore belong outside the source
checkout; placing them in the checkout would intentionally fail the cleanliness
attestation.

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
endpoints. It requests one closed matrix observation containing all 364 ordered
surface checks, including RTL and long-string observations, then separately
requests live AT-SPI, a screen-reader workflow, and the six internationalization
checks. A command must emit one bounded JSON envelope and no standard-error
diagnostics. No shell is involved.

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
  wakeup totals across the exact session, settingsd, notifyd, and shell roles.

The steady-state stack uses event-driven waits rather than fixed-cadence
polling. The session supervisor waits for exact-child signals, session-bus
activity or deadlines, and explicit shutdown activity. Settingsd similarly
waits for its worker, a session-bus event or deadline, or an explicit signal
wake. Shell surface staging remains GUI-thread work, while each host's bounded
worker owns checked dock or desktop X11 property transactions and reports
queued token-bound completions. Settings, composition, desktop-controller, and
child-focus publication therefore do not wait synchronously for X11 replies on
the GUI thread. Their remaining timed waits are bounded readiness, recovery, and
termination policy rather than idle polling. Until a matching validated
multi-sample PD1 idle baseline exists, the final record must identify that
comparison as unavailable instead of labeling the observed total a regression
pass. PD2-specific shell work must not add an unbounded repeating timer.

The installed demonstration artifact is authoritative for exact source
revision, installed executable hashes, workflow postconditions, and cleanup.
It contains no timing data. `pd2_runtime_performance.py` therefore repeats that
installed workflow with a fresh isolated session per trial and measures the
bounded postconditions with a monotonic clock. Common-action timing is narrower:
the live port records action injection to the first visible shell
acknowledgement separately from group setup and the authoritative final WM
postcondition. Asset timing uses the production `IconResolver` and
`WallpaperLoader`, not a QML or model surrogate. The runtime collector
deliberately does not also report wallpaper timing; the production asset runner
is authoritative for that endpoint.

Collect at least three installed-runtime trials after producing an exact
installed-demonstration artifact for the same revision:

```bash
python3 tests/performance/pd2_runtime_performance.py \
  --source-revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system \
  --installed-demo-evidence "/absolute/private/evidence/pd2-installed-demo.json" \
  --controlled-window-fixture "$PWD/build/performance-gcc/tests/integration/prismdrake-controlled-window-fixture" \
  --iterations 3 \
  --output "/absolute/private/evidence/pd2-runtime-performance.json"
```

Collect the strict 60-second idle artifact through the same-tree workflow in
the [PD1 performance-evidence method](pd1-performance-evidence-method.md#idle-wakeups)
three or more times. Then assemble the repeated role totals without retaining
paths, PIDs, thread IDs, XIDs, or diagnostic payloads:

```bash
python3 tests/performance/pd2_idle_series.py \
  --source-revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system \
  --input "/absolute/private/evidence/pd2-idle-1.json" \
  --input "/absolute/private/evidence/pd2-idle-2.json" \
  --input "/absolute/private/evidence/pd2-idle-3.json" \
  --output "/absolute/private/evidence/pd2-idle-series.json"
```

Each input must be a distinct regular file from a separately collected trial,
with a collector-generated unique trial identifier. The series binds the
ordered trial identities by digest and rejects a repeated path, inode, or trial
identifier. Idle totals cover session, settingsd, notifyd, and shell.

Build the production and model runners, bind all four performance sources, and
choose at least two ordered model-supported search sizes. The model runner's
closed controlled sizes are 16, 64, and 256:

The runtime collector measures session readiness from the session launch
request, shell presentation from the kernel-recorded shell process start to the
observed complete panel endpoint, and notification method latency around only
the standard `Notify` call through receipt and validation of its reply. It does
not include `GetCurrentState` or surface polling in the `Notify` sample.
The artifact records this clock split explicitly: ordinary step and D-Bus reply
durations use Python's monotonic nanosecond clock, while the shell-start endpoint
uses Linux boot-time nanoseconds aligned with `/proc` process-start ticks.

```bash
cmake -S . -B build/performance-gcc -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPRISMDRAKE_WARNINGS_AS_ERRORS=ON
cmake --build build/performance-gcc --target \
  prismdrake-pd1-performance-evidence prismdrake-pd2-asset-performance \
  prismdrake-controlled-window-fixture
PRISMDRAKE_PD1_REVISION=$(python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["source_revision"])' \
  /absolute/private/evidence/pd1-performance.json)
env \
  PRISMDRAKE_PD2_BUILD_DIRECTORY="$PWD/build/performance-gcc" \
  PRISMDRAKE_PD2_PERFORMANCE_RUNNER="$PWD/build/performance-gcc/tests/performance/prismdrake-pd1-performance-evidence" \
  PRISMDRAKE_PD2_ASSET_PERFORMANCE_RUNNER="$PWD/build/performance-gcc/tests/performance/prismdrake-pd2-asset-performance" \
  PRISMDRAKE_PD2_RUNTIME_PERFORMANCE_EVIDENCE="/absolute/private/evidence/pd2-runtime-performance.json" \
  PRISMDRAKE_PD2_IDLE_SERIES="/absolute/private/evidence/pd2-idle-series.json" \
  PRISMDRAKE_PD2_PD1_EVIDENCE="/absolute/private/evidence/pd1-performance.json" \
  PRISMDRAKE_PD2_INSTALLED_DEMO_EVIDENCE="/absolute/private/evidence/pd2-installed-demonstration.json" \
  PRISMDRAKE_PD2_PACKAGE_TEST_EVIDENCE="/absolute/private/evidence/non-root/pd2-package-test.json" \
  python3 tests/performance/collect_pd2_live_evidence.py \
    --source-revision "$(git rev-parse HEAD)" \
    --environment-id gentoo-amd64-qemu-system \
    --pd1-reference-revision "$PRISMDRAKE_PD1_REVISION" \
    --pd1-evidence-id pd1-reviewed-reference \
    --search-catalog-size 16 --search-catalog-size 64 \
    --reviewed-accessibility-profile pd2-reference-v1 \
    --limitation "Reference VM measurements do not define release budgets" \
    --observer "$PWD/tests/performance/pd2_reference_observer.py" \
    --accessibility-observer "$PWD/tests/accessibility/pd2_accessibility_observer.py" \
    --output "/absolute/private/evidence/pd2-performance-accessibility.json"
```

The two PD2 inputs above must be the validated initial installed demonstration
and the collector-owned package-test stage for the same source revision. This
makes WP16 preflight, package test, and the initial WP14 demonstration
prerequisites of WP15 collection; unmerge and ordinary reinstall may follow
after WP15. The command derives the exact lowercase PD1 revision from the bound
PD1 artifact instead of relying on a copied placeholder. Any reviewed
accessibility skip is selected through the tracked `pd2-reference-v1` review
profile. That profile expands to one exact key and reason code for every
source-classified not-applicable or unimplemented matrix cell plus the reviewed
screen-reader dependency skip. Explicit `--reviewed-skip` arguments remain
available for unrelated environment-specific skips and cannot duplicate a
profile key.
The all-not-applicable desktop row is additionally gated by the source-bound
`Accessible.ignored` QML contract and the installed Tier C desktop step's exact
`tier_c_desktop_accessibility_excluded` AT-SPI proof; a static source marker or
generic desktop-step success alone is insufficient.
The dedicated accessibility observer uses the VM's GI `Atspi` binding. Qt
translation routing, RTL, and expanded-string routes pass only when CTest
reports their complete exact five-test, five-test, and six-test selections
respectively; a partial or broadened regex selection fails closed.
The CTest, translated-fragment, model, and asset routes read the build's
configured source revision and source directory from `CMakeCache.txt`, then
require that directory to be a clean Git checkout at the requested revision.
The model and asset executables must resolve to their exact expected paths in
that build tree; public environment overrides cannot replace Git or the other
system inspection tools used by the final collector. A stale build, external
runner, or tracked source modification is rejected. Cache metadata alone is not
cryptographic source proof; final evidence must be collected from the reviewed
clean exact checkout.

Model endpoints compare only with the matching validated PD1 model series.
Installed-runtime, production-asset, and idle-wakeup endpoints report
`not_available` unless an actual matching validated multi-sample PD1 baseline
is supplied; they never manufacture a historical value. That status is an
explicit claim boundary, not a regression pass.

Reliable screen-reader automation and every runtime observer must still be
exercised on the reference VM. A genuinely unavailable dependency or
unautomatable state may be represented only by an exact reviewed skip declared
before collection. A skip does not satisfy the corresponding WP15 exit claim.
Orca is the screen-reader dependency for this decision; the presence of a
speech backend such as Speech Dispatcher or eSpeak NG does not by itself make a
screen-reader workflow available. If Orca is present, collection fails closed
until a reliable bounded workflow adapter exists instead of misreporting a
dependency skip.

The dedicated accessibility observer classifies all 364 cells explicitly: 49
use exact installed or Tier C observations, 181 use named QML CTest targets,
and 134 are reviewed not-applicable semantics. No cell is silently omitted or
left as an unimplemented adapter. A passing CTest-backed cell still requires
the source-bound package-test artifact; it is not a substitute for the 49 live
observations.
Its description, Backtab, output-migration focus, and service-restart focus
claims require the exact corresponding records from the 48-step installed
workflow. Restart claims require the settingsd, notifyd, and shell
focus-recovery records; generic process-restart success is not accepted as
accessibility evidence.

The CTest-backed cells require the collector-owned package-test stage: its
source-owned exact inventory, complete ordered JUnit result, one documented
non-root ownership skip, and zero disabled or unrun tests must all match. The
stage source revision and tracked ebuild digest must match the clean checkout,
and its canonical five-executable artifact digest must match the validated
installed demonstration. A passing unrelated build cannot satisfy the matrix.
