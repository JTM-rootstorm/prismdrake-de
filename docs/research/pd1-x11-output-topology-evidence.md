# PD1 X11 output topology evidence

**Date:** 2026-07-17

**Reference environment:** host plus `prismdrake-vm`

**Source revision:** implementation commit `fa0946b`

## Scope

This report records the display-free output-selection and bottom-panel strut
policy plus the bounded RandR discovery boundary for `PD1-009`. The slice is a
standards-only X11 implementation. It does not infer a window manager from a
process name, advertise a native Glasswyrm capability, take window-manager
authority, or move output policy into QML.

The pure policy assigns separate non-owning `OutputId` and `CrtcId` types and
publishes only a completely validated topology. One observation is limited to
64 total outputs, 64 CRTCs, and 4096 modes. The wire adapter limits each RandR
reply to 1 MiB and rejects duplicate or zero resource identifiers before
policy evaluation. Output, CRTC, mode, clone, and mode-name list relationships
are cross-checked before an active candidate is published. Active output rectangles must have nonnegative root
coordinates, nonzero dimensions, and checked 64-bit ends fully contained in
the current nonzero core-root rectangle. A malformed or oversized observation
does not publish a partial snapshot; callers retain the previous valid
snapshot or use the explicit core-root startup fallback when none exists.
Malformed RandR state is reduced only after refreshing the current core-root
geometry.

## Deterministic PD1 policy

The selected output is, in order:

1. The advertised primary output when it remains active and valid.
2. The active output containing the root origin with the lowest output ID.
3. The active output with the lowest `(y, x, output-id)` tuple.
4. The validated core-root rectangle when no active RandR output is usable.

A removed or inactive primary does not invalidate otherwise complete topology
state. Clone outputs may share one CRTC and identical geometry; divergent
geometry for one CRTC is malformed. PD1 output scale is exactly `1.0` and is
not inferred from physical dimensions.

`OutputTopology.hpp` is the canonical root/output geometry contract shared by
selection and strut calculation. The bottom panel uses root coordinates and a
full selected-output width. `_NET_WM_STRUT` and `_NET_WM_STRUT_PARTIAL` values
reserve the distance from the root bottom edge, including the part below a
shorter output. The calculation supports coordinates above `INT32_MAX`, exact
`UINT32_MAX` containment, and inclusive one-pixel start/end ranges. This slice
calculates the values but does not yet publish dock or strut properties.

The sole `RootEventStream` queue consumer owns both core-event and negotiated
RandR selection. It emits a distinct redacted topology-refresh hint for screen,
CRTC, output, and version-appropriate resource changes, so extension events
cannot be silently consumed by a competing core-only poller.

## Dependency boundary

Core transport remains a direct `libxcb.so.1` dependency. RandR discovery is a
separate direct `xcb-randr` dependency from Gentoo `x11-libs/libxcb`; the
observed host component is version 1.17.0 and links as
`libxcb-randr.so.0`. The library is mandatory at process load time, while a
server without a usable RandR extension degrades to the validated core-root
topology.

No Qt, GTK, GNOME desktop-stack, Xlib, compositor, xcb-ewmh helper, or native
Glasswyrm dependency is introduced by this slice. Version 1.17.0 is an observed
component version, not a declared or verified supported minimum.

## Actual host evidence

The pure topology and strut sources were compiled directly with GCC 15.3.0 and
Clang 22.1.8 under `-Wall -Wextra -Wpedantic -Werror`. Each standalone binary
passed all 21 `OutputTopologyTest` and `PanelStrutGeometryTest` cases. The cases
cover primary removal, clone selection and consistency, input-order
independence, negative/zero/overflow/out-of-root geometry, duplicate IDs,
resource ceilings, unavailable RandR, initial fallback, scale, unequal-height
strut math, coordinates above `INT32_MAX`, and inclusive one-pixel ranges.

The integrated current-tree host GCC validation then ran:

```sh
cmake --preset gcc-debug
cmake --build --preset gcc-debug --parallel 4
ctest --preset gcc-debug \
  -R '(OutputTopology|PanelStrutGeometry|RandrTopology)' \
  --output-on-failure
ctest --preset gcc-debug --output-on-failure
```

Configuration and build passed, and the final focused topology, strut, RandR,
and event-decoder filter passed 38 of 38 tests under both warning-as-error GCC
and Clang builds. The final full host GCC run completed 202 registrations with
zero failures. The ownership fixture skipped in its expected host context, and
both X11 integration registrations explicitly skipped because Xvfb is
unavailable on the host.

Host dependency inspection reported:

```text
pkg-config --modversion xcb xcb-randr
1.17.0
1.17.0

pkg-config --libs xcb xcb-randr
-lxcb-randr -lxcb
```

The host dynamic linker cache contains both `libxcb.so.1` and
`libxcb-randr.so.0`. `make validate` passed: required files were present,
structured documents parsed and validated, profiles/themes/fallbacks remained
consistent, local links and assets passed, and all 39 negative rejection paths
passed.

## Gentoo VM evidence

The source artifact `/mnt/shared/prismdrake-wp8-randr-current.tar.gz` was
validated from `/var/tmp/prismdrake-wp8-randr.full1`. The preserved
baseline `prismdrake-pd1-stage0-20260716` remained present after the run.

The guest reported GCC 15.3.0, Clang 22.1.8, core XCB 1.17.0, xcb-randr
1.17.0, and `x11-base/xorg-server-21.1.24`. The exact validation included:

```sh
pkg-config --modversion xcb xcb-randr
pkg-config --libs xcb xcb-randr

cmake --preset gcc-debug
cmake --build --preset gcc-debug --parallel 4
ctest --preset gcc-debug --output-on-failure

cmake --preset clang-debug
cmake --build --preset clang-debug --parallel 4
ctest --preset clang-debug --output-on-failure

cmake --build --preset gcc-debug --target format-check
make validate
ldd build/gcc-debug/tests/integration/prismdrake-x11-integration-tests
```

GCC and Clang each built 79 targets and completed 202 registered tests with
zero failures. The root-inapplicable permission fixture was the sole skip in
each matrix. The normal isolated-Xvfb lane passed live RandR discovery and
proved that the RandR-aware sole queue still delivers core lifecycle hints. The separate
`-extension RANDR` lane passed the core-root fallback. A GCC AddressSanitizer
plus UndefinedBehaviorSanitizer build completed the same 202 registrations
with zero failures and the same single skip. `make validate` passed all 39
negative paths, and the C++ format target passed. Dynamic linkage inspection showed direct
`libxcb-randr.so.0` and `libxcb.so.1` resolution.

The live Xvfb run found and fixed one wire-boundary defect: XCB reported 50
logical bytes for a valid output-info reply whose X11 envelope declared 52
bytes. Reply validation now permits only zero to three bytes of final protocol
padding while preserving the 1 MiB ceiling, minimum fixed structure size, and
exact list accessor counts.

Negative decoded-list tests cover duplicate, zero, and unknown identifiers;
active CRTC and mode membership; clone membership; current-versus-possible
CRTC output relationships; and inconsistent aggregate mode-name lengths.

RandR negotiation is version-correct: 1.2 uses `GetScreenResources` and the
screen, CRTC, and output notification masks; 1.3 uses
`GetScreenResourcesCurrent` and may query the primary output; 1.4 additionally
selects and recognizes resource-change notifications. A genuine RandR 1.2
server was not available, so that compiled branch is audited but not runtime
exercised. Synthetic tests cover deterministic multi-output policy and event
classification, but no physical hotplug event was generated. This evidence
does not establish real-GPU hotplug, physical multi-monitor,
mixed-DPI, or supported-minimum-version behavior.

## Subsequent work

Checked standard window-manager requests, dock and strut publication, and
WM-applied work-area verification are recorded in the
[dock and request evidence](pd1-x11-dock-request-evidence.md). The bounded task
mirror and stale task-record removal remain open. Proposed ADR 0005 remains
Proposed; neither evidence report accepts it or claims a candidate `GW_*`
protocol.
