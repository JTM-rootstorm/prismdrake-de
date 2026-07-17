# PD1 X11 capability and event evidence

**Date:** 2026-07-17

**Reference environment:** `prismdrake-vm` Gentoo guest

**Source revision:** `b7f1f91`

## Scope

This report records the bounded property, standards-capability, and root-event
slices of `PD1-009`. The implementation remains a toolkit-neutral core-XCB
adapter. It does not take window-manager ownership, infer a window manager from
process identity, or advertise any native Glasswyrm capability.

The adapter now interns one closed source-reviewed atom vocabulary and assigns
atoms and windows separate non-owning identifier types. Property reads validate
the connection provenance, expected type, format, item count, byte count, and
complete reply before copying data out of the XCB reply. The global property
ceiling is 1 MiB; each caller supplies a narrower positive item and byte bound.

## Capability and event behavior

EWMH discovery verifies `_NET_SUPPORTING_WM_CHECK` on both the root and the
supporting window. It reads at most 256 `_NET_SUPPORTED` atoms, validates every
atom identifier, then re-reads the root owner before publishing flags. Owner
replacement, disappearance, malformed properties, and oversized data publish a
reduced all-false capability state. Only loss of the X11 transport is fatal.
The public snapshot exposes no supporting-window identifier.

The root event stream selects only structure, substructure-notify, and property
change events. It never selects `SubstructureRedirect`. Exactly one stream may
consume a connection queue, its shared transport remains valid if the original
connection object moves or is destroyed, and destruction removes only the mask
bits the stream added. A dispatch examines at most 256 raw events. Create,
destroy, configure, property, and protocol-error events become non-authoritative
refresh hints; destroyed resource identifiers are not retained.

## Validation

The preserved VM baseline remained `prismdrake-pd1-stage0-20260716`. Guest
source and build directories are under `/var/tmp/prismdrake-wp8.b7f1f91`.

- GCC 15.3.0 and Clang 22.1.8 warnings-as-errors builds each completed 72
  targets against system XCB 1.17.0.
- Each compiler passed all 172 CTest registrations. The only skip was the
  root-inapplicable permission-denied file fixture.
- The isolated Xvfb executable passed 21 tests: 11 EWMH ownership and malformed
  state cases, four property/provenance cases, four root-event lifecycle cases,
  and two connection cases.
- The harness uses Xorg Server Xvfb 21.1.24 with `-displayfd` and `-noreset`, so
  suite boundaries cannot reset the isolated server.
- A full GCC ASan and UBSan build passed the same 172 registrations with leak
  detection enabled and the same documented root-only skip.
- Repository validation passed all 39 negative contract paths, and the C++
  formatting target passed.
- Host GCC and Clang builds passed 172 registrations with the ownership fixture
  and explicitly registered missing-Xvfb integration test reported as skipped.

The only added runtime dependency remains direct system `libxcb.so.1`. This
slice adds no Qt, GTK, GNOME desktop-stack, Xlib, compositor, xcb-ewmh helper,
or native Glasswyrm dependency.

## Remaining work

WP8 still requires standard window-manager request encoding, dock and strut
properties, work-area verification, stale task-record removal, and a documented
RandR primary-output policy with a core-root fallback. Proposed ADR 0005 remains
Proposed; this evidence does not change its status.
