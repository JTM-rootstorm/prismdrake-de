# Gentoo local repository

Prismdrake tracks an EAPI 8 local Portage repository named
`prismdrake-local` under `packaging/gentoo/repository`. It supports PD1
dependency evidence and VM reproducibility; it is not an official Gentoo
overlay and has no synchronization or signing claim.

The current repository contains only
`dev-util/prismdrake-dev-env`, a development metapackage. It intentionally does
not contain `x11-misc/prismdrake-9999`: the product ebuild remains deferred
until the build system and install targets are Accepted and implemented. This
keeps development-only tools out of future runtime metadata (`PD-DEP-002`,
`PD-PKG-006`, `PD-PKG-008`, and `PD-PKG-009`).

## Repository contract

```text
packaging/gentoo/repository/
├── metadata/
│   └── layout.conf
├── profiles/
│   ├── eapi
│   └── repo_name
└── dev-util/
    └── prismdrake-dev-env/
        ├── metadata.xml
        └── prismdrake-dev-env-0.1.ebuild
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
| `debug-tools` | on | GDB, strace, Valgrind, and lsof |
| `x11` | on | Xvfb, Xephyr, Openbox, X11 tools, D-Bus, AT-SPI, Mesa, and DejaVu fonts |
| `qt6` | on | Qt 6 modules for the isolated visible-shell evidence spike |
| `clang` | off | Optional compiler-matrix coverage |
| `implementation-deps` | off | Candidate parser, D-Bus, and test libraries after decision approval |
| `visual-tests` | off | Optional ImageMagick visual comparison tooling |

Enabling `qt6` gathers evidence for Proposed ADR 0003. It does not Accept Qt 6
Quick, CMake, or another architecture decision. Likewise,
`implementation-deps` describes candidate packages and stays disabled until
the corresponding implementation work is approved.

## Project-specific USE policy

The reference policy belongs in `/etc/portage/package.use/prismdrake-dev`, not
in global `make.conf`:

```text
# Prismdrake PD1 reference-VM policy; keep changes package-local.
dev-util/prismdrake-dev-env portage-qa debug-tools x11 qt6 -clang -implementation-deps -visual-tests
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
Permit only that atom through `/etc/portage/package.accept_keywords/prismdrake-dev`:

```text
# The project-owned development metapackage is intentionally testing-only.
dev-util/prismdrake-dev-env ~amd64
```

This is a narrow local-package exception, not permission to enable unstable
keywords globally or to unmask transitive dependencies.

## Resolution and QA

Generate manifests and scan from the guest after installing the default QA
layer:

```sh
export PRISMDRAKE_WORKSPACE=/mnt/shared/prismdrake-de
export PRISMDRAKE_PORTAGE_REPO="$PRISMDRAKE_WORKSPACE/packaging/gentoo/repository"

(cd "$PRISMDRAKE_PORTAGE_REPO" && pkgdev manifest)
PRISMDRAKE_PKGCHECK_CACHE=$(mktemp -d /tmp/prismdrake-pkgcheck.XXXXXX)
pkgcheck scan --cache-dir "$PRISMDRAKE_PKGCHECK_CACHE" \
  "$PRISMDRAKE_PORTAGE_REPO"
rm -rf "$PRISMDRAKE_PKGCHECK_CACHE"
```

Metapackages without distfiles normally produce no Manifest, but `pkgdev`
remains the authoritative generator if one becomes necessary. Never hand-edit
a generated Manifest. Keeping pkgcheck's disposable caches outside the overlay
also prevents a read-only scan from dirtying the shared checkout.

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

## Product ebuild gate

Add `x11-misc/prismdrake-9999` only after an Accepted build decision and real
install targets exist. That future patch must separate build, test, and runtime
dependencies; use the canonical Git source; install only production artifacts
to standard locations; preserve user configuration and state on uninstall; and
prove install, test, uninstall, and reinstall in a disposable VM checkpoint.

Until that gate is met, verification reports the product ebuild as deferred
rather than claiming Prismdrake itself is emergeable.
