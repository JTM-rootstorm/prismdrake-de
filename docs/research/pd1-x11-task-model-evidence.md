# PD1 X11 task-model evidence

**Date:** 2026-07-18

**Reference environment:** host plus `prismdrake-vm`

**Source revisions:** task contracts commit `ca48a8b`; task-source and
confirmation commit `ce34ada`; EWMH stabilization commit `3ae1d0c`; controller
stabilization commit `d3ded0b`; complete demonstration commit `8ce2e13`;
Openbox harness readiness commit `d9149f3`; blocker validation revision
`0e223cf`; bounded task-action revisions `e17a4d2`, `91652fb`, and `1e1dcc6`

## Scope

This report records the bounded, standards-only EWMH task mirror completed for
PD1-WP10. It advances `PD-WIN-001` through `PD-WIN-007` by reading
window-manager-owned task state, decoding untrusted ICCCM/EWMH metadata,
preserving explicit window-incarnation and task-lifetime identities, and
confirming activation, minimization, and close requests only from later
authoritative observations. It also establishes the metadata-mirroring portion
of `PD-WS-001` and `PD-WS-002` through `_NET_WM_DESKTOP`.

The model slice itself does not implement workspace navigation, window movement,
thumbnails, capture, or a Glasswyrm-native interface. A later bounded panel
slice now consumes this model for activation, minimization, and close actions;
it does not broaden the model's authority or complete the wider production task
presentation contract.

## Authoritative snapshot boundary

`EwmhTaskSource` verifies the EWMH owner handshake on every refresh and requires
the owner to advertise the client-list contract. It reads
`_NET_CLIENT_LIST`, optional `_NET_CLIENT_LIST_STACKING`, and optional
`_NET_ACTIVE_WINDOW`, then re-reads the owner and root properties before
publishing. The confirming read requires the same verified owner and the same
ordered mandatory client list. Optional stacking or active-window churn does
not invalidate that structural owner/client gate.

Each mandatory client list is limited to 256 nonzero, unique XIDs. An absent
stacking property falls back deterministically to client-list order. A valid,
bounded stacking list whose set disagrees with the mandatory client list also
falls back to client-list order and records that optional contradiction. A
valid active-window XID outside the mandatory list is cleared and records that
it was stale. `_NET_ACTIVE_WINDOW` set to X11 `None` is canonicalized to no
active window. Malformed, wrongly typed, zero-containing, duplicate, or
oversized mandatory data is rejected immediately. Malformed or oversized
optional data is also rejected immediately rather than retried; optional data
degrades only when its syntax is valid and its value contradicts the mandatory
set.

The Qt task controller retains the prior immutable publication while one owned
single-shot timer schedules transient owner/client stabilization retries after
10, 20, 40, 80, and 160 milliseconds, for a total deferred window of 310
milliseconds. Relevant real X11 events are coalesced while that epoch is
pending. Checked requests remain unavailable until a complete authoritative
refresh succeeds. Exhaustion emits one recoverable diagnostic exactly once and
does not rearm or poll; a later real X11 event may begin a fresh bounded epoch.

The source observes properties only. It selects `PropertyChange` on advertised
clients through the same `X11Connection` whose `RootEventStream` is the sole
event-queue consumer. XID-bearing topology and property hints request a later
refresh; they do not grant Prismdrake focus, stacking, visibility, workspace,
or lifecycle authority. Replacement of the verified WM owner clears the
source's incarnation mapping.

## Metadata and application identity

Every advertised client is decoded through strict type, format, size, and item
count contracts for titles, `WM_CLASS`, window types and states,
`_NET_WM_DESKTOP`, `WM_HINTS`, `WM_TRANSIENT_FOR`, and `_NET_WM_ICON`. Known
standard auxiliary types and `_NET_WM_STATE_SKIP_TASKBAR` are excluded from
task presentation. Invalid metadata excludes that record from the visible task
list while preserving its authoritative `(XID, incarnation)` membership for
lifecycle and request evaluation.

The PD1 identity heuristic is deterministic and deliberately limited:

1. Parse `WM_CLASS` as exactly two NUL-terminated Latin-1 fields.
2. Use the class field as the grouping key when it is nonempty.
3. Otherwise use the nonempty instance field.
4. When `WM_CLASS` is absent, use `unknown-application`.

Malformed or empty-present `WM_CLASS` is rejected rather than guessed. This
implementation does not infer identity from a PID, process name, executable,
desktop file, or WM/compositor name and does not claim perfect application
grouping. A later desktop-entry association policy needs a separately accepted
and tested mapping boundary.

Display titles prefer a nonempty UTF-8 `_NET_WM_NAME`, then convert a nonempty
legacy `WM_NAME` from Latin-1, and otherwise use `Untitled Window`. Published
task records always carry the fixed generic fallback icon name
`application-x-executable`. `_NET_WM_ICON` is structurally and dimensionally
validated at the untrusted property boundary, but pixel payloads are cleared
before task-model publication; this slice neither renders nor retains them.

## Immutable model and lifecycle

`TaskModel` is a display-free, single-owner, non-thread-safe publisher. Each
accepted complete observation creates a new immutable, generation-tagged
snapshot. Existing snapshots remain unchanged for readers. Reordering and
metadata updates retain the task lifetime for the same observed incarnation;
temporary metadata failure or skip-taskbar filtering also preserves lifetime
tracking. Removal drops stale records, and reuse of the same numeric XID after
destruction always receives a new source incarnation and model lifetime.

Task records mirror the authoritative active, hidden, urgent, workspace,
all-workspaces, type, modal, and transient relationships. The model validates
that decoded records exactly cover the authoritative client set, rejects
ambiguous incarnations and invalid transient relationships, and never replaces
the current snapshot with a partially valid observation.

## Request delivery and confirmation

The checked XCB adapter described in the
[dock and request evidence](pd1-x11-dock-request-evidence.md) establishes only
that the X server accepted an activation, minimization, or close
`ClientMessage`. `TaskRequestState` now keeps that delivery result separate
from the WM-owned outcome. A request is bound to the exact window,
incarnation, task lifetime, action, and issue generation, and its dispatch guard
requires that exact current snapshot immediately before delivery.

Activation is confirmed only when a newer matching task snapshot is active;
minimization only when it is hidden; and close only when the exact
`(XID, incarnation)` is absent from a newer authoritative client set. Same-XID
reuse never confirms an old activation or minimization and cannot redirect an
old request. Callers select a nonzero expiry of at most 1024 model generations;
an unobserved action then becomes explicitly refused rather than remaining
pending forever. Delivery rejection, disappearance, and target replacement
remain distinct terminal outcomes.

## Validation evidence

The exact source artifact was
`/mnt/shared/prismdrake-wp10-final-current.tar.gz` with SHA-256
`3d4009633650b426fa307889d2f7b5238465c93634c4ce75d6afa89a6f4db8dd`.
It was extracted to `/var/tmp/prismdrake-wp10-final.3d400963` in the Gentoo
reference VM.

Warning-as-error GCC 15.3.0 and Clang 22.1.8 builds each completed all 247 CTest
registrations with zero failures. The root-inapplicable bounded-file permission
fixture was the sole documented skip. The matrix included isolated Xvfb task
source tests for verified-owner publication, same-connection property hints,
stable lifetimes, destroyed-but-still-advertised clients, exact XID reuse,
active-window `None`, WM-owner replacement, and malformed or oversized icon
metadata. It also retained the RandR-disabled and Openbox dock lanes.

A GCC AddressSanitizer plus UndefinedBehaviorSanitizer build, run with
`ASAN_OPTIONS=detect_leaks=0`, completed the same 247 registrations with zero
failures and the same single skip. LeakSanitizer was not enabled in that lane.
The C++ formatting target passed. `make validate` passed, including all 39
negative contract rejection paths.

Unit coverage includes malformed, duplicate, zero, mismatched, and oversized
root lists; bounded title, class, enum, workspace, hint, transient, and icon
decoding; task filtering, reorder, immutable prior generations, stale removal,
XID reuse, and invalid-observation retention; and delivery versus confirmation,
expiry refusal, filtered targets, disappearance, and replacement.
Controller integration coverage includes owner appearance during the startup
epoch, a later real event after exhaustion, checked-request rejection while a
stale publication is stabilizing, actual X-server connection loss with
exact-once shutdown, and back-to-back Openbox maps followed by removal.

### 2026-07-18 Openbox stabilization trace

A redacted diagnostic run in `prismdrake-vm` held one Openbox session under a
bounded 400-sample observation at 25-millisecond intervals. The verified owner
remained stable. The mandatory client count advanced
`0 -> 1 -> 2 -> 2 -> 3 -> 3`, then remained at three for approximately eight
seconds. Every live stacking
observation agreed with the mandatory client set, although valid order could
differ, and the live active window belonged to that set. A stale active window
appeared only during teardown while the mandatory list was shrinking.

From the VM build containing EWMH revision `3ae1d0c`, controller revision
`d3ded0b`, and demonstration revision `8ce2e13`, the complete PD1 demonstration
passed 25 of 25 consecutive bounded runs. After the stress run, process checks
found zero lingering `prismdrake-shell`, fixture, Openbox, or Xvfb processes.

The exact final source archive at revision `0e223cf`, including the bounded
Openbox map-fixture readiness fix at `d9149f3`, has SHA-256
`abe75c49bdd28fc79d02b32e5f4ab1d37f6c46cbcb3faf7b3d14e19ad0412e35`.
Clean VM configure and build completed with GCC 15.3.0 and Clang 22.1.8. Each
compiler then passed all 559 registered tests, including the real Openbox task
controller lane, with zero failures and the same single root-inapplicable
permission test skipped. GitHub Actions run `29653471728` passed the GCC,
Clang 18, repository-contract, and C++/QML formatting jobs. These results close
the task-stabilization blocker; they do not claim that the broader PD1 exit gate
or its installed-artifact requirements are complete.

### Bounded task-action integration

The later production-panel integration adds a generic code-native task glyph
and a single in-panel action surface. Keyboard Menu or Shift+F10 and pointer
secondary activation open Minimize and Close for the exact coherent task
presentation. The UI stores no XID and emits only the existing typed controller
intents. Presentation replacement closes the surface; reconciliation may reopen
it only for the exact surviving delegate serial. An already-minimized task
disables Minimize and focuses Close.

The exact source archive
`prismdrake-pd1-action-demo-final-v6.tar.gz` has SHA-256
`7d41d316dd21184a009087bd494134e7f65ecf3022b48a468c5d766a01fda437`.
On the Gentoo reference VM, the strict contract and live development
demonstration passed 2/2. The live path proved keyboard minimization followed by
authoritative `_NET_WM_STATE_HIDDEN`, keyboard reactivation followed by removal
of that state, pointer action-menu entry followed by AT-SPI Close, and a second
keyboard Close. Exact process and window disappearance completed both close
claims. The evidence document has SHA-256
`524764a6517a6fc282386b3d06496c270412cc3ed25915a38171f71b5fd93957`.

## Impact and remaining boundary

- **Accessibility:** the display-free model remains independent of controls,
  focus, motion, color, and the accessibility tree. Its bounded panel consumer
  now has keyboard and pointer entry, accessible popup-menu and menu-item roles,
  visible focus, semantic state descriptions, and token-enforced targets. Full
  screen-reader, mixed-scale, and production task-strip evidence remains open.
- **Security and privacy:** all X11 metadata is treated as untrusted and bounded.
  Validation errors are static and do not include XIDs, titles, class values,
  icon pixels, or other property contents. The task model retains only the
  bounded fields required for presentation and request correlation.
- **Dependencies and packaging:** the slice uses the existing direct core XCB
  boundary and introduces no Qt, GTK, GNOME desktop-stack, Xlib, compositor,
  helper-library, or additional runtime dependency.
- **Standards baseline:** task state and actions remain ICCCM/EWMH based. No
  process-name capability detection or nonstandard state mutation was added.
- **Glasswyrm integration:** Glasswyrm or another active WM remains
  authoritative. No `GW_*` capability is implemented or advertised.
- **Capture and thumbnails:** no pixels are captured and no thumbnail path is
  implemented. Title plus the fixed generic icon name remains the reduced
  fallback; any future native thumbnail feature still requires capability,
  privacy, authorization, and lock-state review.

The production task-strip presentation and its action policy remain later
work. Workspace switching and richer workspace metadata also remain outside
this slice. Proposed ADR 0005 remains Proposed; these implementation commits do
not accept it.
