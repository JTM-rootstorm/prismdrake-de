# PD1 synthetic notification-presentation evidence

**Date:** 2026-07-17

**Reference environments:** host plus `prismdrake-vm`

**Evidence checkpoint:** functional WP12 implementation through `70aaf34`, with
the exact Gentoo source artifact and historical Ubuntu lower-bound CI recorded
below

## Scope and status

This report describes the implementation boundary currently present for
PD1-WP12 and issue `PD1-014`: a bounded synthetic notification-card model, a
passive Qt presentation adapter, and token-driven Qt Quick card and list
components. The work supplies notification fixtures, deterministic replacement
and timeout behavior, typed action and dismissal intents, keyboard traversal,
accessible metadata, and presentation inputs for Prismdrake Lustre, Prismdrake
Forge, high contrast, reduced motion, and opaque fallback operation.

The Experimental shell runtime now wires one fixed, bounded development
fixture from the panel to this model and presentation boundary. The runtime
keeps the synthetic owner outside settings-owned QML view epochs, so an active
card can be presented again after a settings-owner loss and complete-generation
rebuild. The card can be traversed, acknowledged, or dismissed by keyboard;
completion hides the card surface and returns focus to the panel affordance.

This is still not a production notification service. It does not own
`org.freedesktop.Notifications`, accept D-Bus sender input, persist history,
implement do-not-disturb policy, fetch remote images, load sender-selected local
paths, play sounds, retain notifications across sessions, or define lock-surface
behavior. The production `prismdrake-notifyd` authority and its service contract
remain PD2-or-later work under the controlling PD1 plan.

The implementation preserves the Accepted component boundary in
[ADR 0002](../adr/0002-component-and-process-model.md): notification surfaces
remain a logical module of `prismdrake-shell`, while the future freedesktop
service, policy, history, and routing authority belongs to
`prismdrake-notifyd`. It follows the Accepted Qt 6 Quick and C++ division in
[ADR 0003](../adr/0003-shell-toolkit.md), the shared profile and accessibility
token direction in [ADR 0006](../adr/0006-theme-token-model.md), and the C++20,
CMake, CTest, system-dependency, and sanitizer baseline in
[ADR 0008](../adr/0008-build-language-and-testing-baseline.md).
[ADR 0005](../adr/0005-standards-and-glasswyrm-integration.md) remains
Proposed; this implementation neither accepts nor implements one of its
candidate native protocols.

## Requirement mapping

| Requirement | Implemented PD1 evidence | Remaining boundary |
|---|---|---|
| `PD-NOTIFY-002` | Synthetic cards have model-owned stable identifiers, typed urgency, ordered actions, deterministic identifier replacement, configured/default/never timeouts, an injected monotonic clock, and generation-checked action and dismissal intents. | These semantics have not been connected to a freedesktop notification-service owner or real sender lifecycle. |
| `PD-NOTIFY-005` | The card exposes a notification role, name, description, and focus state. Actions and dismissal expose button metadata. Keyboard order is card, enabled actions in source order, then dismissal; reverse traversal, Escape dismissal, list-to-list traversal, and focus recovery after replacement, dismissal, or timeout are represented in the QML tests. The Experimental shell enters the fixed fixture from the panel and returns focus there after completion. | Live AT-SPI and screen-reader behavior, large-text layout, RTL behavior, and the final accessibility-tree smoke lane remain WP13 work. |
| `PD-NOTIFY-006` | Text, identifiers, actions, icon names, image metadata and pixels, timeouts, generations, and aggregate publication size are validated before publication and revalidated at the Qt boundary. QML renders sender text with `Text.PlainText`. | A future D-Bus service must repeat validation at its process boundary and define sender, image-decoding, and transport policy. |
| `PD-A11Y-001` through `PD-A11Y-009` | The component has explicit keyboard traversal, visible focus borders, accessible roles and labels, token-supplied minimum target dimensions, a textual and shaped urgency indicator, and zero-duration reduced-motion behavior. The same card implementation consumes Lustre and Forge token values. | This slice does not establish real wallpaper contrast, mixed scaling, text expansion, platform-bridge behavior, or complete shell focus integration. |
| `PD-THEME-003`, `PD-THEME-008`, `PD-THEME-009` | The QML component consumes high-contrast, reduced-motion, opaque-fallback, focus, urgency, and minimum-target values through one typed token input instead of profile-specific component forks. The installed surface projects those values from one complete resolved settings/theme generation. Urgency remains available as text and shape, not hue alone. | Contrast and final rendering still require deterministic baselines. |
| `PD-SEC-001`, `PD-SEC-011`, `PD-SEC-012` | The model rejects malformed, oversized, unsafe-control, non-UTF-8, path-like icon, inconsistent image, invalid enum, stale-generation, and capacity-exceeding inputs. Negative unit tests exercise these boundaries. No rich-text, script, file, URI, or command execution path exists. | Scheduled fuzzing and the production service's process-boundary validation remain outside WP12. |
| `PD-PERF-001`, `PD-PERF-004`, `PD-PERF-009` | The synthetic authority and passive adapter perform no disk, network, D-Bus, X11, image-decoding, or polling work. Time is injected and publications are bounded. Reduced-motion duration is deterministic. | No frame-time, input-latency, allocation, startup, or wakeup budget is measured here. |

`PD-NOTIFY-001`, `PD-NOTIFY-003`, `PD-NOTIFY-004`, and `PD-NOTIFY-007`
are not claimed by this slice. Their freedesktop service, history, policy, and
service-continuity responsibilities remain with the future
`prismdrake-notifyd` implementation.

## Model and publication boundary

`SyntheticNotificationModel` is a single-owner, display-free C++ model. It
performs no I/O, logging, D-Bus work, rendering, image decoding, thread
creation, or sender callback. The only accepted inputs are explicit synthetic
fixtures or developer/test callers. Each accepted mutation publishes one
complete immutable `NotificationSnapshot` with a nonzero generation. Captured
snapshot pointers retain prior publications unchanged.

The model implements the following deterministic behavior:

- Upsert allocates a stable local identifier or replaces the exact requested
  existing card without changing list position.
- Card content generations change independently from snapshot generations, so
  stale actions and dismissals fail rather than targeting replacement content.
- Default, explicit, and non-expiring timeout forms are typed. Explicit and
  default expiry use an injected monotonic clock and an inclusive deadline.
- Timeout advancement removes all cards due at the observed clock value in one
  complete publication.
- Focus targets are modelled as card, enabled actions in input order, then
  dismissal when available.
- The model publishes typed intent data for an action; it never invokes sender
  code or another process.
- Failed validation, stale identity, stale generation, or capacity exhaustion
  retains the previous complete snapshot.

`validateNotificationSnapshot()` repeats the complete card, identifier,
generation, text, visual, action, timeout, accessibility, and aggregate-image
checks before copied or test-constructed state can cross into another in-process
layer.

## Untrusted-data limits

The implementation uses fixed compile-time envelopes rather than accepting
sender-controlled unbounded collections or allocation sizes:

| Boundary | Limit and policy |
|---|---|
| Complete publication | 32 cards and 8 MiB of aggregate decoded synthetic image pixels |
| Summary | 1,024 UTF-8 bytes and 256 Unicode code points; nonempty |
| Body | 16 KiB and 4,096 code points; tab and newline are the only accepted control whitespace |
| Application name | 512 bytes and 128 code points |
| Application identifier | 255 ASCII identifier bytes; no path or URI form |
| Actions | 8 unique action identifiers per card |
| Action identifier and label | 64 ASCII identifier bytes; label up to 512 UTF-8 bytes and 128 code points |
| Theme icon name | 255 ASCII identifier bytes; separators, URI syntax, `.` and `..` are rejected |
| Decoded synthetic image | Tightly packed RGB8 or RGBA8, at most 512 by 512 pixels and 1 MiB per card |
| Timeout | Positive explicit/default duration no longer than 24 hours, or typed non-expiring behavior |

All visible sender text remains literal plain text. The core does not parse a
rich-text subset, and `NotificationCard.qml` explicitly selects
`Text.PlainText` for summaries, bodies, urgency text, and action labels. Theme
icons remain validated lookup keys, not paths. Decoded image fixtures have no
file or network origin. The current Qt adapter deliberately does not expose the
icon or pixel payload to QML, so this slice makes no image-rendering, scaling,
or image-accessibility claim. Diagnostics are static and do not include rejected
notification bodies or other sender content.

## Qt presentation boundary

`NotificationPresentationModel` is a passive `QAbstractListModel` mirror of one
validated immutable publication. It exposes bounded Unicode presentation
properties and QObject action affordances; exact 64-bit notification identities
and content generations remain in C++ and are emitted only through typed
`actionRequested` and `dismissRequested` signals. They do not round-trip through
JavaScript numeric values.

The adapter rejects absent, stale, conflicting, malformed, reentrant, and
non-owner-thread publications without replacing its prior coherent snapshot.
Incremental row removal, insertion, movement, and replacement preserve QObject
identity for unchanged cards. Reconciliation-start and publication-complete
signals allow `NotificationList.qml` to capture focus before a model mutation
and restore it only after the rows and current snapshot are coherent.

`NotificationCard.qml` owns layout, visual state, toolkit accessibility
metadata, and brief interruptible motion. It provides:

- literal summary, body, application-name, action, and urgency presentation;
- explicit notification and button roles, names, descriptions, focusability,
  and focused state;
- visible focus borders and token-sized action and dismissal targets in both
  dimensions;
- deterministic Tab and Backtab traversal that skips disabled actions;
- Escape dismissal for dismissible cards;
- a textual urgency label plus a shaped urgency surface; and
- reduced-motion, high-contrast, opaque-fallback, Lustre, and Forge token inputs
  through one shared component implementation.

The compiled internal QML module carries version `0.1` as explicit internal
module metadata. This Experimental version does not declare a stable public QML
API or compatibility guarantee.

`NotificationList.qml` moves focus between cards and recovers focus when a
publication removes or replaces the focused delegate. It keeps the same
control focused when an unaffected card survives, moves to the next card after
a focused removal when one exists, otherwise moves to the previous card, and
emits a focus-exit intent when the focused list becomes empty. The QML layer
does not mutate the synthetic model directly; it emits passive action or
dismissal intent for a C++ owner to evaluate.

## Standards and Glasswyrm ownership

Notification presentation is a Prismdrake responsibility, so this slice does
not read or mutate window-manager state. It has no X11 transport, compositor,
capture, thumbnail, or blur implementation and advertises no `GW_*` capability.
Glasswyrm or another active window manager remains authoritative for focus,
stacking, outputs, and composition. Any later translucent material remains an
effect request whose blur execution belongs to the compositor; the current
opaque-fallback input uses an ordinary resolved surface color and never captures
or blurs the desktop.

The freedesktop notification standard remains the required production baseline,
but this evidence intentionally stops before bus ownership or protocol handling.
Implementing an internal synthetic model does not satisfy
`PD-NOTIFY-001` and does not imply compatibility with real notification senders.

## Dependency and fallback impact

The display-free `prismdrake-notification-model` target links only the existing
Prismdrake foundation boundary. Its public API contains no Qt types. The passive
presentation adapter links Qt Core, while the separate compiled QML module uses
system Qt QML, Qt Quick, and Qt Quick Controls. Qt Quick Test is test-only.
GoogleTest remains test-only, and no dependency is downloaded or vendored.

The root build declares Qt 6.11 as the minimum supported toolkit version. The
Gentoo reference environment verifies Qt 6.11.1 and qtdeclarative 6.11.1-r1.
Ubuntu 24.04's Qt 6.4.2 packages are below the supported floor and no longer
provide product compatibility evidence. The shell dependency manifest remains
Experimental with `runtime_dependency_state` set to `measured`. The final
[Portage lifecycle evidence](pd1-portage-lifecycle-evidence.md) records the
installed executable closure, installed AT-SPI replay, package ownership,
unmerge, and ordinary reinstall. The earlier
[panel-shell evidence](pd1-panel-shell-evidence.md) records the implemented
complete-generation theme adapter and shared panel component.
The notification slice adds no GTK, GNOME Shell, Mutter, GNOME settings/control-center,
libadwaita, Xlib, Glasswyrm-native, or image-loader dependency.

Fallback behavior is local and explicit:

- invalid or stale publications retain the prior coherent presentation;
- disabled actions cannot emit activation intent;
- nondismissible cards omit the dismissal affordance;
- missing native capabilities require no alternate code path because none are
  consumed;
- reduced motion makes the component motion duration zero;
- disabled transparency and missing blur are represented by an opaque resolved
  surface input; and
- Lustre and Forge use the same component with different resolved values.

The later panel-shell work proves that these QML token inputs arrive from one
validated settings/theme generation rather than mixed individual values.

## Implemented test inventory

This section records source-level coverage present in the repository. The
personally observed host, VM, and CI outcomes are recorded separately in the
validation table below.

### Display-free model tests

`tests/unit/NotificationModelTests.cpp` defines 14 GoogleTest cases covering:

- typed accessible plain-text publication and focus order;
- replacement identity, ordering, content generation, and immutable prior
  snapshots;
- default, explicit, never, and inclusive injected-clock timeouts;
- nondismissible focus behavior and owner removal;
- generation-checked dismissal and unknown or stale identity;
- valid packed RGB8 and RGBA8 images;
- malformed, oversized, control, invalid-UTF-8, and redacted text failures;
- copied-snapshot revalidation;
- malformed, duplicate, and oversized actions;
- path-, URI-, and traversal-like icon rejection;
- malformed, inconsistent, invalid-format, and oversized image rejection;
- aggregate image-memory enforcement;
- invalid timeout, urgency, clock, and configuration state; and
- card capacity, unknown replacement, and moved-from
  behavior.

### Passive Qt adapter tests

`tests/qt/NotificationPresentationModelTests.cpp` defines nine GoogleTest cases
covering literal ordered mirroring without numeric identity properties,
replacement and typed intent refresh, unchanged QObject preservation,
disabled and nondismissible affordances, typed dismissal intent, copied-input
and size-bound rejection, stale and conflicting generation retention,
cross-thread rejection, and reentrant publication rejection while rows remain
coherent.

### Qt Quick component tests

`tests/qt/qml/tst_NotificationCard.qml` defines four card tests covering literal
plain-text and accessible notification metadata; reduced-motion, opaque, and
high-contrast inputs; forward and reverse keyboard order with disabled-action
skipping, exact activation, Escape dismissal, and minimum target width and
height; and reuse of the same component with Forge token values.

`tests/qt/qml-real/tst_NotificationList.qml` uses a C++ Quick Test fixture backed
by the real `SyntheticNotificationModel` and `NotificationPresentationModel`.
Its six tests cover next-then-previous focus after dismissal, next-card focus
after injected-clock timeout, card focus after replacement, preservation of an
unaffected action focus when an earlier row is removed, presentation-object
identity when multiple cards have identical visible content, and focus exit
when the focused list becomes empty.

`tests/qt/qml-real/tst_NotificationSurface.qml` instantiates the shell-facing
surface with the real model fixture and a complete theme-generation projection.
It verifies that one critical card reaches the view, respects the minimum target
envelope, and accepts programmatic keyboard focus. Three display-free
`DevelopmentNotificationOwnerTest` cases cover fixed-card publication and
replacement, stale identity and unknown-action rejection, exact acknowledge and
dismiss behavior, and retention of the presentation publication across QML view
epochs.

Both QML targets are configured for the offscreen platform, software rendering,
and the Basic Quick Controls style. That configuration makes component behavior
deterministic enough for functional tests; it is not a visual-baseline result.

## Final validation record

The table records the functional WP12 implementation and compatibility fixes
through `70aaf34`. The exact staged artifact was extracted and tested in the
Gentoo VM at `/var/tmp/prismdrake-wp12-qt-final`.

The Ubuntu Qt 6.4 rows below are retained as historical evidence for that
revision. The maintainer ended Qt 6.4 support and selected Qt 6.11 as the
minimum on 2026-07-18; those rows are not current compatibility claims.

| Layer | Exact command or artifact | Result | Revision, environment, or run |
|---|---|---|---|
| Contract validation | `make validate` | Passed all 39 rejection fixtures. | Host, `70aaf34` |
| Host GCC warning-as-error build and complete CTest suite | `ctest --test-dir build/wp12-review --output-on-failure` | Passed 440 of 440 tests; the foreign-owner permission test and three X11 integration tests reported their expected host-environment skips because host Xvfb was unavailable. | GCC 15.3.0, `70aaf34` |
| Host Qt QML lint, card Quick Test, and real-model list Quick Test | `cmake --build build/wp12-review --target prismdrake-shell-notification-qml_qmllint`; `ctest --test-dir build/wp12-review -R 'Notification(Card\|List)QmlTest' -V` | QML lint passed; the card runner passed 6 of 6 and the real-model list runner passed 8 of 8. | Qt 6.11.1, offscreen/software/Basic, `70aaf34` |
| Host Clang build and focused notification tests | `ctest --test-dir build/wp12-clang-review -R 'Notification' --output-on-failure` | Passed all 25 focused registrations. | Clang 22.1.8, `70aaf34` |
| Host ASan plus UBSan notification tests | `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/wp12-sanitizers-review -R 'Notification' --output-on-failure` | Passed all 25 focused registrations; host leak detection was disabled because the sandboxed host runtime does not provide a reliable LeakSanitizer environment. | GCC 15.3.0 ASan plus UBSan, `70aaf34` |
| Exact staged/source archive and SHA-256 | `/mnt/shared/prismdrake-wp12-qt-staged.tar.gz`; SHA-256 `c8b10cdb1abad59428bc227ec7cf229e5e7d44eafb2fd1f99ebea90f54c328de` | Exact implementation tree `2c01cdcb2d6a69251d44c5ce258b98afc6f9165b` extracted successfully. | Implementation commit `89576e3`; the later commits through `70aaf34` are Qt 6.4 compatibility and CI-environment fixes |
| Gentoo VM GCC complete suite, including Xvfb and isolated D-Bus lanes | `ctest --test-dir /var/tmp/prismdrake-wp12-qt-final/build-gcc --output-on-failure` | Passed 440 of 440 tests; only the root-inapplicable `BoundedFileTest.DistinguishesPermissionDenied` fixture skipped. Card and real-model list verbose runners passed 6 of 6 and 8 of 8. | GCC 15.3.0, Qt 6.11.1, `prismdrake-vm` |
| Gentoo VM Clang focused notification tests | `ctest --test-dir /var/tmp/prismdrake-wp12-qt-final/build-clang -R 'Notification' --output-on-failure` | Passed all 25 focused registrations. | Clang 22.1.8, `prismdrake-vm` |
| Gentoo VM ASan plus UBSan focused notification tests | `ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /var/tmp/prismdrake-wp12-qt-final/build-sanitizers -R 'Notification' --output-on-failure` | Passed all 25 focused registrations with LeakSanitizer enabled. | GCC 15.3.0 ASan plus UBSan, `prismdrake-vm` |
| Historical Ubuntu Qt lane | `pkg-config --modversion Qt6Core Qt6Qml Qt6Quick Qt6QuickControls2`; complete GCC and Clang CI jobs | All four components reported 6.4.2; GCC and Clang each passed all 440 tests, and the QML lint target passed. Superseded as compatibility evidence on 2026-07-18. | Ubuntu 24.04, GitHub Actions run `29616932095` |
| Historical GitHub Actions | Run `29616932095` | All four jobs passed: GCC build/test, Clang build/test, contract validation, and C++ format plus QML lint. | `70aaf34`; retained as historical evidence |

## Explicitly unresolved evidence

WP12's model and functional presentation code did not close the visual and
assistive-technology acceptance gate by itself. The final installed lifecycle
later closed the PD1 AT-SPI requirement. The following remain assigned to PD3
visual-system work or later integration:

- intentionally reviewed deterministic visual baselines for Lustre, Forge,
  high contrast, reduced motion, disabled transparency, missing blur, large
  text, and RTL layout;
- contrast evaluation over controlled wallpaper and opaque fallback material;
- broader screen-reader and assistive-technology coverage beyond the installed
  AT-SPI smoke fixture;
- per-output placement beyond the panel host's selected output, mixed scaling,
  and a production font baseline;
- rendering and accessibility policy for validated icon or image content; and
- the separately owned `prismdrake-notifyd` freedesktop service, persistence,
  privacy, do-not-disturb, restart-continuity, and lock-surface policy.

This record closes the functional implementation and validation portion of
`PD1-014`. The later installed lifecycle closes the PD1 runtime and AT-SPI
evidence assigned to WP13. Intentionally reviewed visual baselines remain a PD3
visual-system concern, and the production notification service remains outside
this presentation slice.
