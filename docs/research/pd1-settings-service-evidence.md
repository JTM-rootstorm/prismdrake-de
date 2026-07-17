# PD1 settings-service evidence

**Date:** 2026-07-17

**Reference environment:** `prismdrake-vm` Gentoo guest

**Source revision:** `baa4541`

## Scope

This report records validation of the display-free PD1 `prismdrake-settingsd`
prototype, its complete immutable runtime snapshot, and its explicitly
Experimental D-Bus interfaces. The implementation supplies measured evidence
for `PD1-008` and supports `PD-CONFIG-001` through `PD-CONFIG-010`,
`PD-API-001` through `PD-API-005`, `PD-SEC-001`, `PD-SEC-004`, `PD-SEC-005`,
`PD-SEC-007`, `PD-SEC-012`, `PD-REL-006`, and `PD-TEST-002` at the settings
service boundary.

This evidence does not stabilize a D-Bus ABI, implement XSettings publication,
or claim a production-ready session. The Experimental contracts may change
without compatibility guarantees until a separate stability decision is
Accepted.

## Service and transport behavior

The daemon builds and serializes one complete settings/theme generation before
requesting `org.prismdrake.Settings1`. It exports the narrow
`org.prismdrake.Settings1` methods and the internal
`org.prismdrake.SettingsSnapshot1.GetCurrentSnapshot` complete-snapshot method
at `/org/prismdrake/Settings1`.

All potentially blocking file, parse, resolve, serialize, and publication work
runs through one worker slot. A queued, executing, or unconsumed result keeps
the slot occupied; excess work receives the fixed `Busy` error rather than
entering an unbounded queue. The dispatch thread owns every sd-bus object, and
the worker carries copied values and immutable snapshot pointers only.

Incoming calls require same-effective-UID credentials. Candidate TOML and
serialized replies are each limited to 1 MiB. Errors, diagnostics, signals, and
stderr use fixed or closed identifiers and do not echo candidate contents,
filesystem paths, parser excerpts, or secret sentinel values. Same-profile
changes and equivalent reloads return the current generation without emitting
a signal. Failed candidates retain the complete authoritative generation.

Generation identity is scoped to the D-Bus ownership epoch. Broker loss ends
the current epoch; the daemon rebuilds a complete snapshot before reacquiring
the name, and the new epoch starts at generation one. Clients must clear cached
generation assumptions on every ownership gap or disconnect.

## Provider and dynamic closure

CMake discovers exactly one sd-bus provider through pkg-config. It prefers
Gentoo basu and falls back to libsystemd for Ubuntu CI; it never links both and
does not add a libdbus backend. The Gentoo guest selected `sys-libs/basu-0.2.1`
with pkg-config link flag `-lbasu`. The compatibility header explicitly includes
basu's separate vtable header and restricts the implementation to the API
shared with the CI provider.

`ldd` on the guest-built daemon reported this direct/transitive shared-library
closure:

```text
libtomlplusplus.so.3
libbasu.so.0
libstdc++.so.6
libm.so.6
libgcc_s.so.1
libc.so.6
ld-linux-x86-64.so.2
```

nlohmann JSON remains a header-only build dependency. The daemon has no Qt,
GTK, GNOME desktop-stack, X11, or GUI-toolkit linkage.

## Isolated-bus and restart evidence

The committed integration test runs under `dbus-run-session` with `DISPLAY`
removed. It exercises initial Lustre generation one, complete snapshot retrieval,
no-op reload, valid and invalid profile changes, unknown snapshot versions,
exact and oversized candidate bounds, structured redacted validation,
authoritative-generation retention, and exactly one structured signal for a
real publication with no signal for no-ops.

A separate fixed-address broker restart exercise published Forge generation two,
terminated the broker, restarted `dbus-daemon` at the same address, and observed
the still-running daemon reacquire as Lustre generation one. This demonstrates
that an observed disconnect or ownership gap, not comparison of textual unique
owner names alone, resets the client epoch.

## Gentoo guest validation

Guest-local source and build directories under `/var/tmp` used the content now
recorded by revision `baa4541`; compilation did not run on the shared mount.
The preserved baseline snapshot remained `prismdrake-pd1-stage0-20260716`.

- GCC 15.3.0 Debug with warnings as errors built successfully against basu.
  All 111 CTest registrations ran; 110 passed and the root-inapplicable
  permission-denied read case reported an explicit skip. The root-only
  foreign-owner atomic-write case passed.
- Repository contract validation passed with 39 negative rejection paths, and
  `format-check` passed.
- Runtime linkage inspection confirmed `libbasu.so.0` and no libsystemd or
  libdbus linkage.
- Clang 22.1.8 with warnings as errors built successfully and passed the same
  111-test matrix. A Clang-Tidy-enabled clean build then completed all 42
  targets without findings after optional-lifetime and C-string boundary
  hardening.
- GCC ASan and UBSan built all 42 targets. The first leak-detection pass exposed
  three client bus connections that were unreferenced without an explicit
  close. Changing the RAII bus deleter to `sd_bus_flush_close_unref` removed the
  leak; the full rerun passed with leak detection enabled and no sanitizer
  finding.
- GCC Release with link-time optimization and warnings as errors built all 42
  targets with `-flto=auto`; the full test matrix passed.
- A Release `BUILD_TESTING=OFF` configuration built all 25 production targets,
  registered zero tests, and did not discover or require GoogleTest.
- `pkgcheck scan` completed without findings for the in-tree Gentoo repository.

## Remaining Experimental limitations

The PD1 service does not expose a wire cancellation method. It documents bounded
server budgets but does not return `TimedOut` after beginning an engine call;
therefore it cannot publish late after a service-returned timeout. Rare transport
failures such as credential mismatch, `Busy`, service stopping, and internal
reply failure have fixed production mappings, while deterministic injected
transport tests for every rare branch remain additional hardening rather than a
claim of coverage in this evidence slice.

The production installation layout, service activation metadata, XSettings
publication, and init-neutral supervisor belong to later PD1 work packages.
