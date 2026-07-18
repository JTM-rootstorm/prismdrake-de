# PD1 X11 dock and window-manager request evidence

**Date:** 2026-07-17

**Reference environment:** host plus `prismdrake-vm`

**Source revision:** implementation commit `446f0df`

## Scope

This report records the checked standards-only boundaries for bottom-panel
dock publication and task actions in PD1-WP8. It addresses the WP8 requirements
to publish inspectable dock properties, verify WM-applied work-area reservation,
and route activation, minimization, and close intent through the authoritative
window manager. It does not implement the task mirror, claim that a request was
applied before observing WM state, or advertise a Glasswyrm-native capability.

## Dock publication

`DockProperties` accepts a connection-proven fixed atom cache, a non-root panel
window identifier, validated root/output geometry, and a nonzero bounded panel
height. It derives the reservation through the existing canonical
`PanelStrutGeometry` policy and publishes:

- `_NET_WM_WINDOW_TYPE` as `ATOM/32` with exactly
  `_NET_WM_WINDOW_TYPE_DOCK`;
- `_NET_WM_STRUT` as `CARDINAL/32` with exactly four items; and
- `_NET_WM_STRUT_PARTIAL` as `CARDINAL/32` with exactly twelve items.

All writes are checked. Invalid geometry removes a prior publication when the
target remains available, and a partial protocol failure triggers best-effort
cleanup. X11 does not provide one atomic transaction across the three
properties, so callers must publish before mapping the panel; the integration
fixture enforces that order. Removal treats an already destroyed target as
clean while preserving other protocol and connection failures.

The adapter rejects the root window, a foreign atom cache, an unhealthy
connection, and a moved-from connection without dereferencing it. Errors use
bounded fixed diagnostics and never include window identifiers, titles, or
other private metadata.

## Standard window-manager requests

`EwmhWindowRequests` verifies the EWMH owner handshake before exposing task
actions. Activation requires advertised `_NET_ACTIVE_WINDOW`; close requires
advertised `_NET_CLOSE_WINDOW`; minimize uses ICCCM `WM_CHANGE_STATE` with
`IconicState` only when a verified WM owner exists because ICCCM has no
advertisement bit for that request.

Each action sends a format-32 `ClientMessage` to the root with
`SubstructureNotify | SubstructureRedirect`. Activation and close use pager
source indication `2` and preserve the caller's X server timestamp. Minimize
requests state `3` and never writes `_NET_WM_STATE_HIDDEN` directly. The target
is probed immediately before delivery, root and cross-connection identifiers
are rejected, and a destroyed target returns a stale-record result without
sending a message. A successful result means only that the checked request was
accepted by the X server; callers must continue observing authoritative WM
state and account for the bounded destruction race after the probe.

Missing or malformed EWMH ownership produces the existing reduced capability
state and disables all three request methods. No focus, stacking, visibility,
or client state is mutated directly.

## Work-area verification

The plain isolated-Xvfb lane creates a real panel window, publishes before map,
and independently reads each property with strict type, format, item-count, and
`bytes_after == 0` checks. The separate Xvfb/Openbox lane waits boundedly for
`_NET_SUPPORTING_WM_CHECK`, verifies the root/owner self-reference and required
`_NET_SUPPORTED` atoms through XCB, then maps the dock and observes the current
desktop's `_NET_WORKAREA`. For the 1024 by 768 fixture and 48-pixel bottom
panel, Openbox published the expected `[0, 0, 1024, 720]` rectangle.
The work-area observation remains bounded by eight seconds inside the existing
30-second isolated-lane timeout. This accommodates a delayed Openbox event turn
under package-build contention without converting the test to an unbounded
retry or weakening the exact expected rectangle.

Openbox is selected only as a controlled standards-capable test WM. Runtime
capability discovery uses protocol state and never infers support from its
process or executable name. The harness uses xprop solely for a bounded
readiness probe before the test binary performs independent protocol checks.

## Validation evidence

The exact source artifact was
`/mnt/shared/prismdrake-wp8-dock-current.tar.gz` with SHA-256
`35ebcac2542078ff0586595a5b42511e9fbf7ac3d34156c89723f81ea3081f70`.
It was extracted to `/var/tmp/prismdrake-wp8-dock.35ebcac2`. The preserved VM
baseline `prismdrake-pd1-stage0-20260716` remained present after testing.

The guest reported GCC 15.3.0, Clang 22.1.8, core XCB and xcb-randr 1.17.0,
Xorg/Xvfb 21.1.24, Openbox 3.6.1-r11, and xprop 1.2.8. Warning-as-error GCC and
Clang builds each completed all 210 CTest registrations with zero failures;
the root-inapplicable permission fixture was the sole skip. A GCC
AddressSanitizer plus UndefinedBehaviorSanitizer build with leak detection
completed the same 210 registrations with zero failures and the same skip.

The registrations include the normal Xvfb lane, RandR-disabled fallback, and
Openbox work-area lane. Focused live tests cover exact dock properties,
moved-from connection rejection, activate/minimize/close message fields,
stale-target suppression, root and cross-connection rejection, and complete
request disablement without a verified owner. The Openbox lane also passed ten
consecutive repetitions during harness stabilization.

The exact artifact passed `make validate` with all 39 negative rejection paths
and passed the C++ formatting target. Dynamic inspection showed only the
existing direct `libxcb-randr.so.0` and `libxcb.so.1` X11 links; Openbox and
xprop are optional test tools, not runtime dependencies.

The host warning-as-error GCC build, focused unit tests, formatting, and
contract validation passed. The managed host sandbox denied creation of the
isolated D-Bus socket under `/tmp`, and the host does not provide Xvfb, so the
full current-tree result is taken from the VM rather than overstated from the
host environment.

## Remaining work

WP8/WP10 still require the bounded EWMH task mirror, metadata decoding, model
generations, and stale-record removal across create, unmap, destroy, reorder,
and identifier-reuse cases. This request adapter is intentionally unable to
claim action success before that mirror observes the WM result. The production
panel shell remains WP9 scope. Proposed ADR 0005 remains Proposed.
