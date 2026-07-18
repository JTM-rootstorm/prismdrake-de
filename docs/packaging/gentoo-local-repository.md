# Gentoo local repository

Prismdrake tracks an EAPI 8 local Portage repository named
`prismdrake-local` under `packaging/gentoo/repository`. It supports PD1
dependency evidence and VM reproducibility; it is not an official Gentoo
overlay and has no synchronization or signing claim.

The repository contains `dev-util/prismdrake-dev-env`, a development
metapackage, and the live Experimental `x11-misc/prismdrake-9999` product
ebuild. The product ebuild consumes the project CMake install contract and keeps
development-only tools out of runtime metadata (`PD-DEP-002`, `PD-PKG-006`,
`PD-PKG-008`, and `PD-PKG-009`). It installs a bounded PD1 X11 development
session, not a stable or production desktop package.

## Repository contract

```text
packaging/gentoo/repository/
├── metadata/
│   └── layout.conf
├── profiles/
│   ├── eapi
│   └── repo_name
├── dev-util/
│   └── prismdrake-dev-env/
│       ├── metadata.xml
│       └── prismdrake-dev-env-0.1.ebuild
└── x11-misc/
    └── prismdrake/
        ├── metadata.xml
        └── prismdrake-9999.ebuild
```

The repository inherits Gentoo categories and profiles, disables auto-sync,
uses thin unsigned manifests, and must not override unrelated Gentoo packages.
No ebuild may contain a local machine path, credential, or maintainer secret.

Register the checkout from inside the guest with a project-specific file:

```ini
[prismdrake-local]
location = /mnt/shared/prismdrake-de/packaging/gentoo/repository
priority = 1000
auto-sync = no
```

The location above matches the reference VM but is not embedded in an ebuild.
Use the actual checkout path when `PRISMDRAKE_WORKSPACE` differs. The guarded
[bootstrap helper](../development/gentoo-vm.md) writes this guest-local file.

Verify registration with:

```sh
portageq get_repos /
portageq get_repo_path / prismdrake-local
emerge --search prismdrake-dev-env
```

If the shared checkout is unmounted or moved, Portage must fail visibly. Do not
silently point registration at a stale copied overlay.

## Development metapackage

`dev-util/prismdrake-dev-env` always selects the build foundation: CMake,
Ninja, pkgconf, Git, and GCC. These are development dependencies, not a claim
about the future Prismdrake runtime package.

| USE flag | Default | Purpose |
|---|---:|---|
| `portage-qa` | on | Local repository QA and Portage inspection tools |
| `debug-tools` | on | GDB, strace, Valgrind, perf, and lsof |
| `x11` | on | Xvfb, Xephyr, Openbox, X11 tools, D-Bus, AT-SPI and its schema data, Mesa, DejaVu fonts, and fontconfig inspection |
| `qt6` | on | Qt 6.11-or-newer modules for the production shell and visible-shell evidence |
| `clang` | off | Optional compiler-matrix coverage |
| `implementation-deps` | off | Candidate parser, D-Bus, and test libraries after decision approval |
| `visual-tests` | off | Optional ImageMagick comparison, `xdotool` input, and XWD own-window capture tooling |

The generic ebuild defaults keep `clang` and `implementation-deps` disabled.
The implementation-authorized reference VM policy enables both for the
accepted compiler matrix and PD1 parser, D-Bus, and test dependencies.

## Project-specific USE policy

The reference policy belongs in `/etc/portage/package.use/prismdrake-dev`, not
in global `make.conf`:

```text
# Prismdrake PD1 reference-VM policy; keep changes package-local.
dev-util/prismdrake-dev-env portage-qa debug-tools x11 qt6 clang implementation-deps -visual-tests
x11-base/xorg-server xephyr xvfb
x11-wm/openbox session xdg
x11-libs/libxkbcommon X tools
x11-libs/pango X
x11-libs/cairo X
app-accessibility/at-spi2-core X
media-libs/freetype harfbuzz
media-libs/libglvnd X
media-libs/mesa X llvm
dev-qt/qtbase:6 X accessibility dbus gui opengl -gtk
dev-qt/qtdeclarative:6 accessibility opengl qmlls
dev-qt/qttools:6 opengl qdbus qtdiag qtplugininfo
```

These flags were checked against the Gentoo repository visible to the reference
VM on 2026-07-16. Re-run Portage inspection after repository updates; remove a
flag that the selected ebuild no longer exposes. The Qt `-gtk` selection avoids
adding a GTK platform-theme integration plugin to the controlled evidence
spike. It is not a ban on GTK applications or a project-wide toolkit policy.
The Pango, Cairo, Freetype, and libglvnd settings are the package-local closure
required by the X11 harness on a guest whose global USE policy disables X.
`qttools[widgets]` couples its OpenGL state to `qtbase`, so all three selected
Qt modules use the same OpenGL setting.

Do not globally enable `~amd64`, `X`, `qt6`, `systemd`, or `elogind`. Follow the
guest profile for init integration. Add `xorg` to `x11-base/xorg-server` only
when a real interactive Xorg seat is required in addition to Xvfb and Xephyr.
Do not add Wayland, WebEngine, multimedia, printing, SQL backends, or unrelated
Qt modules without a demonstrated PD1 use.

The project-owned metapackage is testing-only and therefore uses `~amd64`.
The live product ebuild deliberately has no architecture keyword while it
remains Experimental, so Portage requires `**` for that exact atom. Permit only
these two project-owned atoms through
`/etc/portage/package.accept_keywords/prismdrake-dev`:

```text
# Project-owned packages use package-local keyword exceptions only.
# The development metapackage is testing-only; the live product ebuild is intentionally unkeyworded.
dev-util/prismdrake-dev-env ~amd64
x11-misc/prismdrake **
```

These are narrow local-package exceptions, not permission to enable unstable
keywords globally, accept unkeyworded transitive dependencies, or unmask
anything outside the project-owned repository.

## Resolution and QA

Generate manifests and scan from the guest after installing the default QA
layer:

```sh
export PRISMDRAKE_WORKSPACE=/mnt/shared/prismdrake-de
export PRISMDRAKE_PORTAGE_REPO="$PRISMDRAKE_WORKSPACE/packaging/gentoo/repository"

PRISMDRAKE_QA_REPO=$(mktemp -d /tmp/prismdrake-repository-qa.XXXXXX)
PRISMDRAKE_PKGCHECK_CACHE=$(mktemp -d /tmp/prismdrake-pkgcheck.XXXXXX)
cp -a "$PRISMDRAKE_PORTAGE_REPO/." "$PRISMDRAKE_QA_REPO/"
(cd "$PRISMDRAKE_QA_REPO" && pkgdev manifest)
pkgcheck scan --cache-dir "$PRISMDRAKE_PKGCHECK_CACHE" \
  "$PRISMDRAKE_QA_REPO"
rm -rf "$PRISMDRAKE_QA_REPO" "$PRISMDRAKE_PKGCHECK_CACHE"
```

Metapackages without distfiles normally produce no Manifest. The current QA
flow uses a disposable exact copy because both pkgdev and pkgcheck may create
cache data while inspecting a repository. If a future package needs a
Manifest, generate it with pkgdev, review it, and commit it with the package;
never hand-edit it. Keeping all disposable caches outside the overlay prevents
a scan from dirtying the shared checkout.

Run a reviewed pretend before every install or feature combination:

```sh
emerge --pretend --verbose --changed-use --deep --noreplace --tree \
  dev-util/prismdrake-dev-env
USE="-qt6" emerge --pretend --verbose --changed-use --deep --noreplace --tree \
  dev-util/prismdrake-dev-env
USE="clang" emerge --pretend --verbose --changed-use --deep --noreplace --tree \
  dev-util/prismdrake-dev-env
USE="implementation-deps" emerge --pretend --verbose --changed-use --deep --noreplace --tree \
  dev-util/prismdrake-dev-env
USE="visual-tests" emerge --pretend --verbose --changed-use --deep --noreplace --tree \
  dev-util/prismdrake-dev-env
```

The command-scoped `USE` values above are resolution probes; the persistent
reference selection remains in the project-specific package.use file. Capture
the exact pretend output, `emerge --info`, resolved versions, licenses, binary
package choices, and final USE state as test evidence rather than copying a
stale package graph into normative documentation.

Install only after reviewing that evidence:

```sh
emerge --ask --verbose --changed-use --deep --noreplace \
  dev-util/prismdrake-dev-env
```

Stop on masks, keyword requirements, license prompts, or USE conflicts. Do not
use `--nodeps`, change global keyword policy, edit Portage databases directly,
or execute an actual automated depclean to force resolution.

## Experimental product ebuild

`x11-misc/prismdrake-9999` uses `git-r3` with the canonical repository URI and
enables the upstream install-path contract. Its runtime dependency set contains
only the libraries and broker required by the three installed Experimental
processes. GoogleTest, Python, Xvfb, Openbox, X11 inspection/input tools,
AT-SPI/PyGObject inspection support, fontconfig, and the fixed test font remain
conditional `USE=test` build dependencies. This complete gate prevents a clean
reference-VM package test from silently registering the live startup or AT-SPI
lanes as skipped merely because their evidence tooling was absent.

Run a reviewed resolution and targeted package test with:

```sh
emerge --pretend --verbose --tree --newuse x11-misc/prismdrake
USE=test emerge --ask --verbose --newuse x11-misc/prismdrake
```

Portage's mandatory libsandbox injects `LD_PRELOAD` and `SANDBOX_*` entries into
executed children. The ebuild therefore excludes exactly three upstream tests:

- `DetachedApplicationTest.ExecutesExactArgvWorkingDirectoryAndEnvironmentWithoutShell`
  and
  `LauncherPipelineTest.ExpandsPlansAndLaunchesLiteralArgumentsWithoutAShell`
  require byte-exact child environments and reject the injected variables.
- `X11DockOpenboxIntegrationTest` remains an exact Prismdrake X11 contract, but
  libsandbox's preload changes the third-party Openbox process's strut handling.

Run all three separately outside the Portage sandbox on the same source
revision. Run the exact Openbox test at least five consecutive times against
the same built artifact. No other test exclusion is permitted by this policy.

Before updating the ebuild, verify its installed file list and dynamic linkage,
run the settings and X11 session behavior from installed paths, unmerge it, and
confirm that user configuration and state remain byte-identical. Reinstall once
without `USE=test` to exercise the ordinary package path. Use a disposable VM
checkpoint and preserve the documented Stage 0 baseline. Current exact results
belong in the PD1 Gentoo package evidence report rather than this normative
workflow document.
