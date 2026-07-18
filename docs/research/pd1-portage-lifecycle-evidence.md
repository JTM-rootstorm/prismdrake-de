# PD1 Gentoo package lifecycle evidence

- **Status:** Completed PD1 reference-environment evidence
- **Date:** 2026-07-18
- **Source revision:** `e747480ad6a8b2d6ea59beb931d0d80797881ca9`
- **Reference environment:** `prismdrake-vm`, snapshot `prismdrake-pd1-stage0-20260718`
- **Qt version:** 6.11.1

This report closes the PD1 installed-product gate for `PD-DEP-001` through
`PD-DEP-008`, `PD-TEST-001`, `PD-TEST-002`, `PD-TEST-005`, `PD-TEST-007`,
`PD-TEST-008`, `PD-A11Y-001` through `PD-A11Y-009`, and the Experimental
session, settings, shell, launcher, task, and fallback requirements exercised
by the version-three development demonstration. It records one exact Gentoo
reference result; it does not turn the prototype into a supported daily-use
desktop or establish compatibility with untested package versions.

## Immutable source and package identity

The live ebuild was forced to the exact source revision through a read-only
local Git remote. The source bundle SHA-256 was
`ebbf976a93ef565a33f78eb580b7fc904e6fba57178e741d00e9c714eafa8284`,
and the selected ebuild SHA-256 was
`69774d12cd8ab8806fb09619c3891fb3243df503ed184cf5b6273741d5058db5`.
The repository copy passed `pkgdev manifest` and `pkgcheck scan` without a
finding. Both default and `USE=test` pretend resolutions completed for the
same revision and ebuild.

Qt resource timestamps are derived from the checked-out commit through
`SOURCE_DATE_EPOCH`. Two independent clean ordinary builds produced identical
session, settings-daemon, and shell bytes; the shell build ID was identical in
both builds. This proves same-environment reproducibility for the measured
revision, not cross-toolchain reproducibility.

## Portage lifecycle result

`FEATURES="test keepwork" USE=test` produced a 179-unit product build with
install paths enabled and a separate 321-unit test build. The Portage test
phase completed 561 registrations with zero failures. The existing
privilege-dependent foreign-owner permission fixture reported its documented
skip; no required X11, D-Bus, Qt Quick, visual, performance, or AT-SPI lane was
silently skipped.

Four tests are incompatible with Portage's mandatory libsandbox environment.
Both exact-child-environment tests passed once outside the sandbox, and both
Openbox tests passed five consecutive attempts outside the sandbox. These were
the only package-test exclusions.

The tested install owned 14 expected paths. Its normalized artifact SHA-256 was
`09822fec03c6e3949c1796480407b5fdc2f8585bddd9dc09be37e0a260cc98c0`:

| Installed executable | SHA-256 |
|---|---|
| `/usr/bin/prismdrake-session` | `b19c10d585751563c11b6cc481882204bd796f82903aa640b598aeba2cdb9802` |
| `/usr/bin/prismdrake-settingsd` | `ecf671bb69579e640b67fa93a6921ef8c91332cd6fd24c22386723ac0214dd6f` |
| `/usr/bin/prismdrake-shell` | `54ade19a965e94dbafb10fb9e67ac17435c85b8e02ef9a3ef0addde552dffeae` |

The installed executables then passed the strict AT-SPI driver and the complete
version-three Xvfb/Openbox/D-Bus demonstration using packaged `/usr` data.
Unmerge removed every Portage-owned path while controlled XDG configuration
and state tree hashes remained byte-identical. An ordinary `USE=-test`
reinstall succeeded and reproduced all three tested executable hashes.

## Measured runtime boundary

`scanelf` measured these direct installed `NEEDED` boundaries:

- session: basu, core XCB, the C++ runtime, and libc;
- settings daemon: toml++, basu, the C++ runtime, libm, and libc; and
- shell: basu, core XCB, XCB RandR, Qt 6 Core/GUI/Quick, the C++ runtime,
  libm, and libc.

The package metadata also retains the session D-Bus broker and the direct Qt
and XCB package requirements. No mandatory GNOME desktop-stack dependency,
vendored library, shell-side blur implementation, or Glasswyrm-native protocol
claim was introduced.

## Compiler and quality matrix

Fresh GCC 15.3.0 and Clang 22.1.8 Debug builds used separate Ninja directories
with warnings as errors. Each passed all 565 CTest registrations with zero
failures and the same documented permission-fixture skip. The repository
validator passed all 39 negative fixtures, `format-check` passed, and the panel,
launcher, and notification Qt 6.11 QML lint targets completed without findings.

## Evidence privacy and integrity

The final lifecycle record remains private in the guest with mode 0600. Its
SHA-256 is
`16a397eb288341a87b90b65da9901c5ea432fb26e8e6f84b33580bbbb6983f9b`.
The non-final installed preflight hash is
`b95a5ce29e3f26308ffab3dfeba8aaed53d9df83f8ae6056d99440652c762440`.
The installed AT-SPI and complete-demo evidence hashes are respectively
`1b7be74f264c24712d962c259f2072c425e506e98449130e2e462fb86e9ba6f3`
and `1954a51c232161ad67c7c430444c4142109e0a7c2919aa7b4a7ddabc2f8f626f`.
Tracked files contain no session addresses, display numbers, PIDs, window IDs,
private paths, full accessibility trees, or unrestricted user content.

## Remaining boundary

PD1 remains an Experimental shell skeleton. Supported lower bounds remain
unmeasured except for the maintainer-selected Qt 6.11 floor exercised with Qt
6.11.1. PD1 does not claim a production notification service, global launcher
shortcut, full screen-reader workflow, mixed-scale or multi-output coverage,
alternate-window-manager coverage, secure locking, or approved visual goldens.
Those are later-milestone concerns and do not invalidate the completed PD1
exit demonstration.
