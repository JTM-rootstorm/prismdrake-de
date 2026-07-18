# PD1 prototype performance-evidence method

## Status and claim boundary

This document defines the repeatable measurement method required by PD1-WP14.
It does not define a release budget, establish a performance baseline, or claim
that Prismdrake is optimized. Numeric release budgets remain required before
beta by `PD-PERF-007` and are outside PD1.

The tracked repository contains the method and a bounded in-process evidence
runner. Collected result JSON belongs with the reviewed Gentoo VM artifacts, not
in source control. A correctness failure invalidates the affected performance
trial; a timing result must never replace the corresponding functional test.

## Reference build and result identity

Collect evidence from a clean build of one exact commit in the maintained
Gentoo amd64 QEMU guest. Use the baseline-restorable VM, its Xvfb and Openbox
packages, the system Qt and XCB libraries selected by Portage, and no unrelated
interactive workload. Record the VM image or snapshot label in the surrounding
artifact manifest. The JSON deliberately records only a reviewed environment
identifier, not a host name, user name, path, window title, application name, or
machine identifier.

Configure a dedicated `RelWithDebInfo` tree and run the bounded collector:

```bash
cmake -S . -B build/performance-gcc -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPRISMDRAKE_WARNINGS_AS_ERRORS=ON
cmake --build build/performance-gcc \
  --target prismdrake-pd1-performance-evidence
build/performance-gcc/tests/performance/prismdrake-pd1-performance-evidence \
  --revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system \
  --iterations 25 > pd1-performance-gcc.json
python3 tests/performance/validate_evidence.py \
  build/performance-gcc/tests/performance/prismdrake-pd1-performance-evidence \
  tests/performance/performance-evidence.schema.json
```

Repeat from a separately configured Clang tree when comparing toolchains. Do
not combine samples from different commits, compilers, build types, VM
snapshots, Qt versions, or measurement methods. The runner embeds the product
version, compiler identity and version, architecture, CMake generator, build
type, C++ standard, and developer-override state. The exact source revision and
reviewed environment identifier are required arguments.

The schema command generates a bounded three-sample contract document, validates
it against the strict version-1 schema, and cross-checks the fixed series order
and summary values. The runner accepts 3 through 200 measured iterations,
performs one untimed warm-up, uses `std::chrono::steady_clock`, and emits integer
nanosecond samples plus minimum, median, p95, and maximum summaries. Its fixed
fixture sizes are 16, 64, and 256. Closed error identifiers are printed on
failure. Successful JSON contains neither fixture paths nor fixture content.

## In-process measurements

The runner measures five narrow operations. Each operation validates its
correctness result before retaining a sample.

| Measurement | Timed interval | Deliberate exclusions |
|---|---|---|
| Settings initial load | `SettingsEngine::start` reading the packaged configuration and complete theme bundle through publication of generation 1 | Process launch, D-Bus activation, and shell consumption |
| Profile-switch publication | Validated alternating `lustre`/`forge` request through complete immutable in-process publication | D-Bus transport, subscriber dispatch, rendering, and disk persistence |
| Desktop-entry discovery | Scanner creation and bounded pulls through a complete publication over pre-created, equal-size desktop files | Fixture creation and launcher presentation |
| Search response | Search-operation creation through complete in-memory result publication over an immutable controlled catalog | Discovery, `TryExec`, process launch, QML, and provider I/O |
| EWMH task-model update | One complete decoded observation applied to an already populated `TaskModel` through generation 2 publication | X server round trips, property decoding, rendering, and WM actions |

Desktop discovery is a warm-filesystem-cache measurement after the untimed
trial. Cold-cache claims require a separately reviewed VM reset method; do not
use privileged cache dropping inside this collector. Discovery pulls are capped
at 64 work units and 4096 pulls. Search advances at the production bound. EWMH
fixtures do not exceed the production 256-window limit. No operation creates an
unbounded queue or timing-dependent correctness threshold.

## Session startup to the first mapped panel

This metric must be collected in a fresh isolated D-Bus session and a fresh
Xvfb/Openbox display for every sample. Use a private, empty `XDG_RUNTIME_DIR`
with mode `0700`, install the X11 observer before launching
`prismdrake-session`, and take the start timestamp immediately before spawning
that exact process.

The endpoint is the later of these two observations from the same launch:

1. the exact session instance creates its private `ready` marker after the
   exact-child readiness channel reports the initial panel presentation epoch;
2. the X11 observer receives `MapNotify` for the only window on the fresh
   display that has valid `_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STRUT`, and
   `_NET_WM_STRUT_PARTIAL` properties.

Using the later event prevents a fork-to-exec handshake, stale marker, foreign
dock, or merely constructed QML tree from being reported as a visible panel.
The result means mapped and standards-valid on Xvfb; it does not prove that a
real compositor exposed a reviewed pixel. Use inotify and X11 events rather
than a filesystem or `xprop` polling loop. Bound each trial to the production
five-second shell-readiness deadline, terminate the exact session PID with
SIGTERM, require bounded clean shutdown, and discard any trial with multiple
session directories, a child restart, safe mode, a diagnostic error, or a
missing endpoint.

The current repository does not yet contain this external event collector.
Therefore startup-to-mapped-panel numbers remain blocked rather than being
inferred from earlier shell process durations or the private readiness event
alone.

## Idle wakeups

Measure wakeups only after the session startup trial has reached its valid
endpoint and then remained untouched for a fixed five-second settling period.
Keep the display, Openbox configuration, profile, output size, notification
model, launcher state, and absence of user input fixed. Enumerate the exact
`prismdrake-session`, `prismdrake-settingsd`, and `prismdrake-shell` thread IDs
from `/proc/<pid>/task` at the start and end of a 60-second window.

In the guest, use Linux `sched:sched_wakeup` and `sched:sched_wakeup_new`
tracepoints filtered on those target thread IDs. Report received wakeups for
each component, the interval in monotonic nanoseconds, the starting and ending
thread counts, and whether the thread set changed. A changed thread set
invalidates the trial. Tracepoint access may require root inside the disposable
guest. If the kernel, tracefs, or `perf` cannot provide filtered scheduler
events, mark the metric blocked. Do not substitute timer polling, CPU
percentage, context switches, or wakeup estimates and label them as wakeups.

No repository collector currently performs the privileged tracepoint capture,
so idle-wakeup results remain blocked pending a reviewed VM-only wrapper.

## Deterministic visual cadence

Cadence evidence must use the same locked C locale, UTC timezone, resolved font,
Basic controls style, fixed scale, software scene graph, basic render loop, and
fixed Xvfb output described by the visual-candidate harness. Add a dedicated
workload that invalidates exactly 240 consecutive frames of the shared
production component tree. Begin sampling only after two untimed warm-up frames
and record monotonic `frameSwapped` intervals from the owning Qt thread. Emit
raw integer nanoseconds, median, p95, maximum, and the count above 25,000,000 ns.
That count is descriptive prototype evidence, not a release threshold.

Run Lustre, Forge, reduced motion, disabled transparency, and missing blur as
separate series. Any QML warning, dropped test state, render-backend change,
font mismatch, incomplete frame series, or non-monotonic timestamp invalidates
the series. This software/Xvfb method characterizes the deterministic harness;
it does not claim production GPU, compositor, or physical-output cadence.

The current candidate recorder synchronizes isolated still images and provides
no controlled 240-frame workload or interval output. Visual cadence therefore
remains blocked. Screenshot completion time and byte-identical PNG output must
not be relabeled as frame cadence.

## Review and retention

For every collected series, retain the JSON, configure log, build log, focused
correctness-test log, exact commit, VM snapshot label, and SHA-256 hashes in the
untracked VM artifact exchange. Review raw samples before comparing summaries.
A later comparison must use identical methods and environments and must show
both before and after series; otherwise it is a measurement record, not an
optimization claim.

This method addresses the evidence portions of `PD-PERF-001` through
`PD-PERF-006` and `PD-PERF-009` without claiming `PD-PERF-007` completion.
Event-driven idle and startup collectors preserve the no-continuous-polling
boundary, while the controlled discovery and search series make the bounded
PD1 launcher work observable without moving I/O onto the UI thread.
