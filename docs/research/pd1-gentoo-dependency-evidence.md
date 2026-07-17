# PD1 Gentoo dependency evidence

- **Status:** Observed Stage 0 evidence supporting the approved dependency boundary
- **Observed:** 2026-07-16 in `prismdrake-vm`
- **Scope:** `dev-util/prismdrake-dev-env-0.1` with the default USE policy

This snapshot records the reviewed development and evidence environment. It is
not runtime metadata for Prismdrake. ADRs 0003 and 0008 and the development
dependency boundary were accepted separately by the maintainer on 2026-07-16.
The future live-ebuild scope is approved, but its implementation remains a
separate PD1 work package.

## Resolver state

The guest used Gentoo Base 2.18, profile
`default/linux/amd64/23.0/systemd`, Portage 3.0.81.2, and the official Gentoo
binary repository. The project overlay resolved directly from
`/mnt/shared/prismdrake-de/packaging/gentoo/repository`; both `portageq` and
`eselect repository list -i` confirmed that live path. Repository QA ran from
an isolated copy and `pkgcheck` 0.10.40-r1 reported no findings.

The installed metapackage USE state was:

```text
-clang +debug-tools -implementation-deps +portage-qa +qt6 -visual-tests +x11
```

The optional visual evidence tools were installed in a separately reviewed
transaction; they remain disabled in the persistent metapackage policy.
Project-specific X11, Qt accessibility, OpenGL, QML tooling, and software
rendering selections are listed in
[Gentoo local repository](../packaging/gentoo-local-repository.md). The guest's
global `make.conf` was not changed.

An `emerge --pretend --verbose --emptytree --tree` of the default selection
resolved 451 packages and 1,028,851 KiB of downloads. That number is a full
development closure from an empty tree, not incremental installation cost or
a Prismdrake runtime budget. The shared evidence bundle contains the exact
752-line empty-tree resolver output, the complete `emerge --info`, the final
verifier log, and an exploratory `equery depgraph` snapshot. Those local logs
are machine evidence and intentionally remain outside Git.

All five reviewed matrices resolved without masks, keyword failures, license
blocks, or USE conflicts: default, `-qt6`, `clang`, `implementation-deps`, and
`visual-tests`. The final verifier reported zero failures and zero warnings.

## Direct installed development atoms

| Layer | Installed package versions |
|---|---|
| Build | `cmake-4.3.3-r1`, `ninja-1.13.2-r1`, `pkgconf-2.5.1`, `git-2.54.0`, `gcc-15.3.0` |
| Portage QA | `eselect-repository-15`, `gentoolkit-0.7.2`, `portage-utils-0.97.1`, `pkgcheck-0.10.40-r1`, `pkgdev-0.2.15` |
| Debug | `gdb-17.2`, `strace-7.0-r1`, `valgrind-3.27.1`, `lsof-4.99.6` |
| X11 and accessibility | `at-spi2-core-2.58.6`, `gsettings-desktop-schemas-49.1`, `dejavu-2.37`, `mesa-26.0.8`, `dbus-1.16.2`, `xorg-server-21.1.24`, `openbox-3.6.1-r11` |
| Qt evidence | `qtbase-6.11.1`, `qtdeclarative-6.11.1-r1`, `qtsvg-6.11.1`, `qttools-6.11.1` |
| Optional visual evidence | `imagemagick-7.1.2.18`, `xdotool-4.20260303.1`, `xwd-1.0.9` |

The remaining direct X11 utilities in the metapackage are small standard
inspection tools selected by atom in the ebuild. Their precise transitive
versions are preserved in the empty-tree output rather than duplicated here.

## Direct-license review

The direct atoms use established free-software licenses. Notable groups are:

- Qt modules: Gentoo's `|| ( GPL-2 GPL-3 LGPL-3 ) FDL-1.3` metadata.
- Xorg server, Mesa, XWD, xdotool, Ninja, pkgconf, pkgcheck, and pkgdev:
  MIT, BSD, Apache-2.0, ISC, or the package's documented combination.
- GCC, Git, GDB, Valgrind, Openbox, Portage utilities, D-Bus, AT-SPI, and
  GSettings schemas: GPL/LGPL/AFL/Apache combinations recorded by Gentoo.
- DejaVu fonts: `BitstreamVera`; ImageMagick: `imagemagick`; lsof: `lsof`.
- The project-owned development package: `metapackage` and no installed files.

No proprietary asset or mandatory GNOME desktop-stack runtime was introduced.
`gsettings-desktop-schemas` is a data-only runtime requirement of the AT-SPI
test bus in this minimal guest; it does not add GNOME Shell, Mutter, or a GNOME
settings service.

## Incremental install observations

The environment was installed in reviewable layers with `--getbinpkg` only
when binary USE matched:

1. X11/debug foundation: 62 packages, 52 binary and 10 source builds.
2. Qt evidence layer: 11 packages, 6 binary packages; QtBase, QtTools,
   QtLanguageServer, and QtDeclarative built from source because official
   binaries omitted required accessibility, QML tooling, or OpenGL settings.
3. Visual/input evidence: four packages initially, followed by binary `xwd`
   and binary `gsettings-desktop-schemas` after live evidence exposed those
   missing direct requirements.

No `emerge --depclean`, profile switch, global keyword change, or global USE
change was performed. After installation and evidence capture, the 30 GiB root
filesystem had approximately 6.5 GiB free.

## Remaining evidence boundary

The maintainer approved the reviewed mandatory and optional development layers
and the future live-ebuild scope. This evidence does not establish a minimal
production closure, installed-size budget, or dependency set for components
that have not yet been implemented. Those values require measurement from the
production targets and ebuild rather than inference from this development
metapackage.
