# PD1 X11 development harness

This operator path demonstrates the Experimental PD1 session, settings service,
and shell on an X11 display. It is a bounded development check, not a login
session recommendation or evidence that Prismdrake is a daily-use desktop. The
shell uses standard X11, ICCCM, EWMH, D-Bus, XDG, desktop-entry, and
accessibility contracts in this milestone. It does not require or advertise an
implemented Glasswyrm-native interface.

The automated Xvfb and AT-SPI lanes remain the reproducible pass/fail checks.
The interactive lane below is for reviewing the same production executables on
a visible development display and exercising the synthetic notification that
is deliberately outside a production notification service.

## Prerequisites and boundaries

Run the interactive lane from the repository root inside a disposable X11
session with an EWMH window manager. Openbox is the reference window manager.
Do not run it on a desktop where a second panel or test task controls would be
disruptive. The inherited `DISPLAY` must name X11, not a Wayland-only session.

The commands use `dbus-run-session` for a private session bus and a new
mode-0700 `XDG_RUNTIME_DIR`. The Prismdrake supervisor owns only the exact
`prismdrake-settingsd` and `prismdrake-shell` children it launches. SIGINT or
SIGTERM requests reverse-order shutdown; each child gets a two-second
termination grace followed by a bounded one-second kill-and-reap grace.

Required operator tools are CMake, Ninja, `dbus-run-session`, `gdbus`,
`xdotool`, and `xprop`. The isolated automated lanes additionally require
Xvfb, Openbox, and the dependencies documented in
[live accessibility testing](accessibility-testing.md).

## Reproducible build-tree checks

Configure against source-tree read-only data, build the three production
processes, and run the focused X11, window-host, session, accessibility, and
QML tests:

```bash
cmake -S . -B build/pd1-x11-harness -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPRISMDRAKE_WARNINGS_AS_ERRORS=ON \
  -DPRISMDRAKE_USE_INSTALL_PATHS=OFF \
  -DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON
cmake --build build/pd1-x11-harness --parallel 2
ctest --test-dir build/pd1-x11-harness \
  -R 'Session|X11|PanelWindow|PanelSurface|LauncherSurface|Notification|Accessibility' \
  --output-on-failure --timeout 60
```

`PRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON` makes a missing live accessibility
dependency a configure error instead of silently registering a skipped live
case. The CTest command supplies a per-test outer cap, while the live harnesses
apply their own narrower operation deadlines and cleanup bounds. A skipped
optional case is not a pass and must not be reported as completed evidence.

`Pd1DevelopmentDemonstrationTest` also performs two deliberate, exact-child
failure injections. It first restarts the shell, then kills only the supervised
settings daemon. The latter must remove the presentation epoch while preserving
the shell, window-manager owner, and fixture windows; a distinct settingsd child
must restore owner-epoch generation one, accessibility overrides, one valid
panel, and the task presentation. The version-three evidence represents
generation identity as `[[1, 2, 3], [1]]` because numeric generations are local
to each settings-owner epoch.

## Interactive build-tree demonstration

First export the exact build-tree paths. Keeping them absolute is required by
the session option parser:

```bash
export PRISMDRAKE_DEMO_SESSION="$PWD/build/pd1-x11-harness/src/session/prismdrake-session"
export PRISMDRAKE_DEMO_SETTINGSD="$PWD/build/pd1-x11-harness/src/settingsd/prismdrake-settingsd"
export PRISMDRAKE_DEMO_SHELL="$PWD/build/pd1-x11-harness/src/shell/runtime/prismdrake-shell"
export PRISMDRAKE_DEMO_LOG="$PWD/build/pd1-x11-harness/pd1-session.stderr.log"
```

Then start one private bus and runtime boundary. The harness gives Forge 10
seconds and Lustre 120 seconds of visual review after a 12-second startup
observation bound. It verifies the standard dock metadata, switches through
two complete profile generations, and requests bounded exact-child cleanup.
Press Ctrl-C to end earlier.

```bash
test -n "${DISPLAY:-}"
PRISMDRAKE_DEMO_RUNTIME="$(mktemp -d)"
export PRISMDRAKE_DEMO_RUNTIME

XDG_RUNTIME_DIR="$PRISMDRAKE_DEMO_RUNTIME" dbus-run-session -- bash -eu -c '
  umask 077
  session_pid=
  cleanup() {
    trap - EXIT INT TERM
    if [ -n "$session_pid" ] && kill -0 "$session_pid" 2>/dev/null; then
      kill -TERM "$session_pid" 2>/dev/null || true
      iteration=0
      while kill -0 "$session_pid" 2>/dev/null && [ "$iteration" -lt 160 ]; do
        sleep 0.05
        iteration=$((iteration + 1))
      done
      if kill -0 "$session_pid" 2>/dev/null; then
        kill -KILL "$session_pid" 2>/dev/null || true
      fi
      wait "$session_pid" 2>/dev/null || true
    fi
  }
  trap cleanup EXIT INT TERM

  "$PRISMDRAKE_DEMO_SESSION" \
    --settingsd "$PRISMDRAKE_DEMO_SETTINGSD" \
    --shell "$PRISMDRAKE_DEMO_SHELL" \
    2>"$PRISMDRAKE_DEMO_LOG" &
  session_pid=$!

  ready=
  iteration=0
  while [ "$iteration" -lt 240 ]; do
    ready="$(find "$XDG_RUNTIME_DIR/prismdrake" -mindepth 2 -maxdepth 2 \
      -type f -name ready -print -quit 2>/dev/null || true)"
    [ -n "$ready" ] && break
    kill -0 "$session_pid"
    sleep 0.05
    iteration=$((iteration + 1))
  done
  test -n "$ready"

  mapfile -t panel_windows < <(xdotool search --name "^Prismdrake Panel$")
  test "${#panel_windows[@]}" -eq 1
  panel="${panel_windows[0]}"
  xprop -id "$panel" _NET_WM_WINDOW_TYPE _NET_WM_STRUT _NET_WM_STRUT_PARTIAL

  gdbus call --session \
    --dest org.prismdrake.Settings1 \
    --object-path /org/prismdrake/Settings1 \
    --method org.prismdrake.Settings1.RequestProfileChange forge
  printf "%s\n" "Review Forge for 10 seconds before the harness restores Lustre."
  sleep 10
  gdbus call --session \
    --dest org.prismdrake.Settings1 \
    --object-path /org/prismdrake/Settings1 \
    --method org.prismdrake.Settings1.RequestProfileChange lustre

  printf "%s\n" "Review the checklist below; this session closes in 120 seconds."
  sleep 120
'

rmdir "$PRISMDRAKE_DEMO_RUNTIME/prismdrake" 2>/dev/null || true
rmdir "$PRISMDRAKE_DEMO_RUNTIME"
unset PRISMDRAKE_DEMO_RUNTIME
```

The panel search must return exactly one window. The `xprop` result must
identify it as `_NET_WM_WINDOW_TYPE_DOCK` and show both `_NET_WM_STRUT` and
`_NET_WM_STRUT_PARTIAL`. The active window manager remains authoritative for
the applied work area, focus, stacking, task lifecycle, and window requests.
Prismdrake only publishes standard dock intent and sends checked EWMH requests.

Review these actions while the bounded session is running:

1. Confirm the panel remains on the validated primary output's bottom edge and
   ordinary applications retain their window-manager-owned task behavior.
2. Use **Applications** to enter the launcher. Entry currently requires the
   panel pointer affordance or its AT-SPI `Press` action; PD1 does not claim a
   global launcher shortcut. Once open, type a query, use Tab or Down to reach
   a result, Shift-Tab or Up to return, and Escape to return focus to the panel.
3. Confirm Forge is opaque and Lustre remains readable. On the standard X11
   capability snapshot, compositor blur is unavailable, so Lustre uses its
   ordinary alpha or opaque material fallback; Prismdrake never captures and
   blurs the desktop itself.
4. Activate **Test notification**. Confirm one fixed card titled
   **Prismdrake test notification** appears, Tab reaches **Acknowledge** and
   **Dismiss**, Shift-Tab reverses traversal, and either action or Escape
   removes the card and returns focus to the panel.
5. Confirm the notification is explicitly synthetic. The shell does not own
   `org.freedesktop.Notifications`, store notification history, or claim that
   `prismdrake-notifyd` exists in PD1.

This manual observation does not prove that accessibility preferences persist
across profile changes, cover every profile/fallback/accessibility visual
matrix, or replace the full 18-step installed exit demonstration. Use the
[live accessibility lane](accessibility-testing.md) and
[deterministic visual baselines](../testing/visual-baselines.md) for their
automated contracts. The complete installed replay remains pending in the next
section.

The structured supervisor events are in `PRISMDRAKE_DEMO_LOG`; interpret them
using [PD1 diagnostics](diagnostics.md). Do not publish the whole log until it
has been reviewed for ordinary Qt or process stderr outside the closed event
format.

## Installed-artifact demonstration

**Evidence status:** the command below is the exact operator path, but the
Portage install, repeated lifecycle, and installed-artifact results remain
pending until the maintainer records them from the reference Gentoo VM. Do not
describe this section as passed merely because the build-tree lane succeeds.

After `x11-misc/prismdrake` is installed from the matching source revision,
reuse the interactive command above with only these path exports changed:

```bash
export PRISMDRAKE_DEMO_SESSION=/usr/bin/prismdrake-session
export PRISMDRAKE_DEMO_SETTINGSD=/usr/bin/prismdrake-settingsd
export PRISMDRAKE_DEMO_SHELL=/usr/bin/prismdrake-shell
export PRISMDRAKE_DEMO_LOG=/absolute/private/evidence/installed-session.stderr.log
```

Before starting, capture the strict installed-artifact preflight attestation
described in [the Gentoo repository guide](../packaging/gentoo-local-repository.md)
and pass it to `tests/integration/pd1_demo.py` as:

```bash
--artifact-provenance portage_installed \
--installed-artifact-attestation \
  /absolute/private/evidence/installed-preflight.json
```

The parent and isolated child both validate the attestation against the exact
executables and source driver before continuing. The installed build is
configured with `PRISMDRAKE_USE_INSTALL_PATHS=ON`; it must read packaged
defaults, themes, schemas, and interfaces under the configured `/usr` data
prefix rather than the source tree. Run the independent installed AT-SPI driver
from the exact matching source revision as documented in
[live accessibility testing](accessibility-testing.md).

An installed lifecycle result is complete only after the reference-VM record
includes install, all required package tests, this standards/fallback/keyboard
check, the synthetic notification action, bounded shutdown, uninstall state
verification, and ordinary reinstall. Those results intentionally are not
pre-filled here.

Only after the demonstration, unmerge, and ordinary reinstall should the
operator finalize and validate the strict tracked lifecycle record. The
preflight attestation is deliberately insufficient as a final result:

```bash
python3 tests/gentoo/portage_lifecycle_evidence.py \
  /absolute/private/evidence/pd1-portage-lifecycle.json
```

The contract binds the exact source revision, ebuild and tested artifact,
default and `USE=test` pretend graphs, package tests, installed ownership and
executable hashes, AT-SPI and version-three demonstration results, runtime
linkage, the three reviewed sandbox exclusions, unmerge preservation, and the
ordinary reinstall. It contains no pre-filled result and cannot establish a
pass until every phase was actually observed.

## Failure interpretation and cleanup

- No `ready` marker within the harness's 12-second observation window fails
  this demonstration. The supervisor may still be inside its separate bounded
  recovery policy, so inspect its events before retrying; do not extend the
  harness deadline until a root cause is known.
- A missing settings owner or snapshot removes the presentation epoch but does
  not transfer settings authority to the shell. A later complete owner epoch
  may rebuild it.
- The first fatal X11 transport loss requests shell shutdown. The supervisor
  may apply its bounded restart policy, but Prismdrake does not attempt to
  reconstruct a lost Qt platform connection in process.
- Missing blur or optional native capability selects a reduced visual fallback
  only. It does not authorize Prismdrake to perform composition or
  window-management policy.
- Successful supervisor cleanup removes its private `ready` and `safe-mode`
  markers and exact session instance. The final two `rmdir` commands remove
  only the empty harness-owned runtime directories; a failure there is a signal
  to inspect unexpected state, not permission for recursive deletion.
