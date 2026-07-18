# PD1 prototype performance-evidence method

## Status and claim boundary

This document defines the repeatable measurement method required by PD1-WP14.
It does not define a release budget, establish a performance baseline, or claim
that Prismdrake is optimized. Numeric release budgets remain required before
beta by `PD-PERF-007` and are outside PD1.

The tracked repository contains the method, a bounded in-process runner, and
dedicated external startup, scheduler-wakeup, and visual-cadence collectors.
Collected result JSON belongs with the reviewed Gentoo VM artifacts, not in
source control. A correctness failure invalidates the affected performance
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
that exact process. The collector fixes the supervised tree to the available
UTF-8 C locale while keeping its bounded `xprop` subprocesses on their exact
C-output grammar. Before taking that timestamp, the collector changes a
private probe property on the fresh root window and requires the already-running
`xev` observer to report the corresponding `PropertyNotify`. It removes the
probe property before launching the session. A missed or unbounded handshake
invalidates the trial rather than risking an observer-startup race.

The endpoint is the later of these two observations from the same launch:

1. the exact session instance creates its private `ready` marker after the
   exact-child readiness channel reports the initial panel presentation epoch;
2. the X11 observer receives a root `MapNotify` or EWMH client-list change,
   then resolves the complete `_NET_CLIENT_LIST_STACKING` inventory to the
   only client with normal ICCCM `WM_STATE`, valid `_NET_WM_WINDOW_TYPE_DOCK`,
   `_NET_WM_STRUT`, and `_NET_WM_STRUT_PARTIAL` properties, and an exact
   `_NET_WM_PID` belonging to the single direct `prismdrake-shell` child owned
   by the supervised session. This client-list resolution is required because
   a reparenting window manager reports its frame, not the dock client, in the
   root `MapNotify` event.

Using the later event prevents a fork-to-exec handshake, stale marker, foreign
dock, or merely constructed QML tree from being reported as a visible panel.
The result means mapped and standards-valid on Xvfb; it does not prove that a
real compositor exposed a reviewed pixel. Use inotify and X11 events rather
than a filesystem or `xprop` polling loop. Bound each full-session trial to
exactly ten seconds: the production supervisor sequentially permits five
seconds for settings readiness and then five seconds for shell readiness.
Terminate the exact session PID with SIGTERM and allow at most seven seconds
for the supervisor's reverse-order child shutdown (two seconds TERM plus one
second kill/reap per child, with one second of scheduling margin). Require
bounded clean shutdown, and discard any trial with multiple
session directories, a child restart, safe mode, a diagnostic error, or a
missing endpoint. Every supervised restart publishes the structured
`recovery=restart_component` diagnostic before relaunch. The collector inspects
the bounded session diagnostic stream through the endpoint, rejects that exact
field or any other pre-endpoint diagnostic, and records
`child_restart_observed=false` only after that check.
The authoritative EWMH task reader permits exactly four coherence attempts so
the short Openbox property burst around the dock map can settle without polling;
four torn or malformed snapshots still fail closed and surface a diagnostic.

The collector parses the exact atom, cardinal, and ICCCM state fields from one
bounded `xprop` read; substring matches are not evidence. It cross-checks the
claimed window PID against the kernel-owned direct-child set and expected shell
executable, then retains a pidfd through the endpoint. A foreign dock, a second
dock observed through the endpoint, an invalid strut shape, or a shell identity
change invalidates the trial. PIDs and XIDs remain internal and are never
emitted. After both endpoint signals, a bounded `_NET_CLIENT_LIST_STACKING`
inventory acts as an X-server barrier: every mapped client is inspected and the
trial proceeds only when the complete managed set contains exactly the one
owned dock. All `xprop` stdout is streamed into a fixed 64 KiB buffer; overflow,
invalid ASCII, timeout, or a malformed inventory fails closed.

Build the three production executables, then run one fresh trial with:

```bash
cmake --build build/performance-gcc --target \
  prismdrake-session prismdrake-settingsd prismdrake-shell
dbus-run-session -- python3 tests/integration/run_with_xvfb.py \
  --openbox "$(command -v openbox)" --xprop "$(command -v xprop)" \
  "$(command -v Xvfb)" "$(command -v python3)" -- \
  "$PWD/tests/performance/collect_startup_to_panel.py" \
  --session "$PWD/build/performance-gcc/src/session/prismdrake-session" \
  --settingsd "$PWD/build/performance-gcc/src/settingsd/prismdrake-settingsd" \
  --shell "$PWD/build/performance-gcc/src/shell/runtime/prismdrake-shell" \
  --xev "$(command -v xev)" --xprop "$(command -v xprop)" \
  --stdbuf "$(command -v stdbuf)" \
  --revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system > pd1-startup.json
```

Repeat the complete command, including the outer D-Bus and Xvfb/Openbox setup,
for each independent sample. The collector fails closed on timeout, multiple
session instances, safe mode, diagnostics, invalid dock properties, early exit,
or unbounded shutdown. It emits no PIDs, XIDs, paths, or diagnostic payloads.
The live validation wrapper forwards an exact fixed collector failure identifier
but replaces malformed, unknown, multiline, or otherwise open stderr with its
own `collector_failed` identifier.
Live reference numbers remain pending VM collection; earlier process durations
are not substituted.

## Idle wakeups

Measure wakeups only after the session startup trial has reached its valid
endpoint and then remained untouched for a fixed five-second settling period.
Keep the display, Openbox configuration, profile, output size, notification
model, launcher state, and absence of user input fixed. Enumerate the exact
`prismdrake-session`, `prismdrake-settingsd`, and `prismdrake-shell` thread IDs
from `/proc/<pid>/task` at the start and end of a 60-second window.

In the guest, use system-wide `perf stat -a` with Linux
`sched:sched_wakeup` and `sched:sched_wakeup_new` tracepoints filtered on those
target thread IDs. Per-process perf scope cannot observe arbitrary wakers and
is invalid for this received-wakeup metric. Report received wakeups for
each component, the interval in monotonic nanoseconds, the starting and ending
thread counts, and whether the thread set changed. A changed thread set
invalidates the trial. Tracepoint access may require root inside the disposable
guest. If the kernel, tracefs, or `perf` cannot provide filtered scheduler
events, mark the metric blocked. Do not substitute timer polling, CPU
percentage, context switches, or wakeup estimates and label them as wakeups.

`collect_idle_wakeups.py` performs the five-second settling interval itself,
verifies each supplied PID resolves to the exact expected executable, captures
all three roles concurrently, and rejects a changed thread set. It repeats the
exact executable checks after settling and after collection, and terminates and
reaps every already-started `perf` process group on every exit path. Each perf
instance starts in a new session; the collector is a child subreaper so the
bounded TERM/KILL cleanup also adopts and reaps the `sleep` workload child. It
holds pidfds for all three Prismdrake processes across settling and capture,
checking liveness plus the exact `/proc` executable at every boundary. Invoke it
only through the bounded same-tree workflow below. The workflow launches one
supervised session, validates its ready marker and sole owned mapped dock, then
runs the idle collector before shutting that exact tree down. The idle collector
independently requires settingsd and shell to be the session's complete direct
child set and revalidates the endpoint before settling, before capture, and
after capture:

```bash
dbus-run-session -- python3 tests/integration/run_with_xvfb.py \
  --openbox "$(command -v openbox)" --xprop "$(command -v xprop)" \
  "$(command -v Xvfb)" "$(command -v python3)" -- \
  "$PWD/tests/performance/collect_live_session_performance.py" \
  --session "$PWD/build/performance-gcc/src/session/prismdrake-session" \
  --settingsd "$PWD/build/performance-gcc/src/settingsd/prismdrake-settingsd" \
  --shell "$PWD/build/performance-gcc/src/shell/runtime/prismdrake-shell" \
  --xev "$(command -v xev)" --xprop "$(command -v xprop)" \
  --stdbuf "$(command -v stdbuf)" \
  --perf "$(command -v perf)" --sleep "$(command -v sleep)" \
  --revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system \
  --startup-output "$PWD/pd1-startup.json" \
  --idle-output "$PWD/pd1-idle-wakeups.json"
```

Tracepoint or permission unavailability remains a reported blocker, not a
reason to substitute another metric. Production evidence is exactly 60 seconds; short test-mode collector
output records `contract_eligible=false` and is rejected by the production
schema. Live reference results remain pending VM collection.

## Deterministic visual cadence

Cadence evidence uses the same locked C locale, UTC timezone, resolved font,
Basic controls style, fixed scale, offscreen QPA, software scene graph, and
basic render loop as the visual-candidate harness. The dedicated workload
invalidates the shared production panel tree until it records exactly 240
consecutive `frameSwapped` intervals after two untimed warm-up frames. It emits
raw integer nanoseconds, median, p95, maximum, and the count above 25,000,000 ns.
That count is descriptive prototype evidence, not a release threshold.

Run Lustre, Forge, reduced motion, disabled transparency, and missing blur as
separate series. Any QML warning, dropped test state, render-backend change,
font mismatch, incomplete frame series, or non-monotonic timestamp invalidates
the series. This offscreen software method characterizes the deterministic
harness; it does not claim production GPU, compositor, or physical-output
cadence.

The artifact records both requested environment values and runtime-observed Qt
state: `LANG`, `LC_ALL`, `TZ`, Qt locale and runtime UTC offset, requested and actual QPA,
requested and actual graphics backend, and configure-time claimed plus runtime
actual font family. The strict validator requires UTC, the C locale, offscreen
QPA, software graphics, and equality between claimed and observed values; the
font source is the configure-time `fc-match` resolution. Evidence includes only
the safe source basename and a lowercase SHA-256 digest, never the font path.

Build and run the collector with the environment locked explicitly:

```bash
cmake --build build/performance-gcc --target prismdrake-visual-cadence-evidence
env LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QSG_RENDER_LOOP=basic \
  QT_QUICK_CONTROLS_STYLE=Basic QT_SCALE_FACTOR=1 \
  build/performance-gcc/tests/performance/prismdrake-visual-cadence-evidence \
  --revision "$(git rev-parse HEAD)" \
  --environment-id gentoo-amd64-qemu-system > pd1-visual-cadence.json
```

The collector runs separate Lustre, Forge, reduced-motion,
disabled-transparency, and missing-blur series. It rejects Qt/QML warnings,
font or backend drift, non-monotonic timestamps, and incomplete frame series.
Still-image completion time remains a separate non-cadence quantity. Live
reference results remain pending VM collection.

Validate each external JSON document with:

```bash
python3 tests/performance/validate_external_artifact.py \
  tests/performance/external-performance-evidence.schema.json \
  pd1-visual-cadence.json
```

The validator applies both the strict schema and kind-specific semantic checks:
startup endpoint equality and recovery state; idle component order, stable
thread counts, and exact wakeup sums; and cadence scenario order plus summaries
recomputed from all raw samples.

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
