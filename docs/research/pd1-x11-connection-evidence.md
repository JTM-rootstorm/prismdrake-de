# PD1 X11 connection evidence

**Date:** 2026-07-17

**Reference environment:** `prismdrake-vm` Gentoo guest

**Source revision:** `7237a96`

## Scope

This report records the first bounded implementation slice for `PD1-009`. The
toolkit-neutral `Prismdrake::x11` library owns one core-XCB connection, validates
the selected screen and nonzero root window, and completes a checked geometry
round trip before reporting that the display is usable. The public boundary
exposes typed `WindowId` and `ScreenInfo` values rather than raw XCB objects.

This slice supports the real-display prerequisite for `PD1-010`. It does not
claim that the remaining WP8 adapter is complete. In particular, it does not
implement EWMH capability discovery, property decoding, root event handling,
standard window-manager requests, dock properties, work-area reservation,
RandR output policy, or any Glasswyrm-native interface.

## Failure and authority boundary

Display values are bounded, NUL-free, and never repeated in diagnostics. A
connection error, missing selected screen, zero root identifier, protocol
error, mismatched geometry root, or zero geometry fails with a fixed actionable
error. The connection destructor disconnects only its own XCB client. It does
not start or stop the X server, window manager, compositor, or applications.

An Xvfb server without a window manager is intentionally a usable X11 display.
EWMH support is a separate reduced-function capability and must not be inferred
from process names or environment branding.

## Gentoo guest validation

Guest-local source and build directories under
`/var/tmp/prismdrake-wp8.7237a96` used revision `7237a96`. The preserved VM
baseline remained `prismdrake-pd1-stage0-20260716`.

- GCC 15.3.0 and Clang 22.1.8 warnings-as-errors builds each completed all 63
  targets against system XCB 1.17.0.
- Each compiler ran 155 CTest registrations with no failures. The only skip was
  the root-inapplicable permission-denied file fixture.
- `X11ConnectionIntegrationTest` started Xorg Server Xvfb 21.1.24 through
  `-displayfd`, completed a real geometry round trip at 1024 by 768 pixels, and
  rejected a dead display without echoing its value.
- Repository validation passed all 39 negative contract paths and the C++
  formatting target passed.
- The integration executable directly needed `libxcb.so.1`; the observed XCB
  transitive closure also included `libXau.so.6` and `libXdmcp.so.6`. No Qt,
  GTK, GNOME desktop-stack, Xlib, compositor, or native Glasswyrm library was
  linked.

The host completed the strict GCC build, unit tests, formatting, and repository
validation. Its X11 integration registration was explicitly absent because
Xvfb was not installed; the Gentoo guest supplied the required real-server
evidence.

## Remaining work

The next WP8 slices must add bounded atom/property handling, verified EWMH
capabilities, synthetic event decoding, standard dock and strut properties,
stale-window recovery, and documented primary-output behavior. Proposed ADR
0005 remains Proposed; this evidence neither accepts it nor implements any
candidate `GW_*` protocol.
