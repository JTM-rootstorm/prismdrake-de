# Gentoo local repository

Prismdrake tracks an EAPI 8 local Portage repository named
`prismdrake-local` under `packaging/gentoo/repository`. It supports historical
PD1 dependency evidence, the active PD2 package lifecycle, and VM
reproducibility; it is not an official Gentoo overlay and has no
synchronization or signing claim.

The repository contains `dev-util/prismdrake-dev-env`, a development
metapackage, and the live Experimental `x11-misc/prismdrake-9999` product
ebuild. The product ebuild consumes the project CMake install contract and keeps
development-only tools out of runtime metadata (`PD-DEP-002`, `PD-PKG-006`,
`PD-PKG-008`, and `PD-PKG-009`). It installs the bounded Experimental PD2
session, settings service, notification service, shell, and control client, not
a stable or production desktop package.

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
| `x11` | on | Xvfb, Xephyr, Openbox, X11 inspection and protocol tracing tools, D-Bus, AT-SPI and its schema data, Mesa, DejaVu fonts, and fontconfig inspection |
| `qt6` | on | Qt 6.11-or-newer modules for the Experimental shell and visible-shell evidence |
| `clang` | off | Optional compiler-matrix coverage |
| `implementation-deps` | off | Candidate parser, D-Bus, and test libraries after decision approval |
| `visual-tests` | off | Optional ImageMagick comparison, `xdotool` input, and XWD own-window capture tooling |

The generic ebuild defaults keep `clang` and `implementation-deps` disabled.
The implementation-authorized reference VM policy enables both for the
accepted compiler matrix and current parser, D-Bus, and test dependencies.

## Project-specific USE policy

The reference policy belongs in `/etc/portage/package.use/prismdrake-dev`, not
in global `make.conf`:

```text
# Prismdrake reference-VM policy; keep changes package-local.
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
dev-qt/qttools:6 linguist opengl qdbus qtdiag qtplugininfo
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
Qt modules without a demonstrated current Prismdrake use.

The project-owned metapackage is testing-only and therefore uses `~amd64`.
The live product ebuild deliberately has no architecture keyword while it
remains Experimental, so Portage requires `**` for that exact atom. Permit only
these two project-owned atoms and the exact WP14 protocol-tracer atom through
`/etc/portage/package.accept_keywords/prismdrake-dev`:

```text
# Project packages and the WP14 tracer use package-local keyword exceptions only.
# The development metapackage and X11 tracer are testing-only; the live product ebuild is intentionally unkeyworded.
dev-util/prismdrake-dev-env ~amd64
x11-misc/prismdrake **
x11-misc/xtrace ~amd64
```

`x11-misc/xtrace` installs the X11 protocol tracer as `/usr/bin/x11trace` on
Gentoo. Do not substitute glibc's unrelated `/usr/bin/xtrace` dynamic-loader
diagnostic. These are narrow local-package exceptions, not permission to
enable unstable keywords globally, accept other unkeyworded transitive
dependencies, or unmask unrelated packages.

## Product lifecycle evidence contract

Run this lifecycle only after the final revision has been selected. A later
rebase, amended commit, or signing pass rewrites the revision and invalidates
every source-revision-bound attestation, demonstration, performance artifact,
and lifecycle stage. Rerun all such evidence after any history rewrite.

For a local unpublished revision, keep the tracked ebuild's canonical GitHub
URI and use `git-r3`'s repository-specific override. Create the bundle outside
the checkout, bind the exact commit explicitly, and retain both until the
lifecycle record has validated:

```bash
export PRISMDRAKE_INPUT_CHECKOUT=/mnt/shared/prismdrake-pd2-source
export PRISMDRAKE_EVIDENCE_ROOT=/var/tmp/prismdrake-pd2-evidence
install -d -m 0711 "$PRISMDRAKE_EVIDENCE_ROOT"

PRISMDRAKE_REVISION=$(/usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" \
  rev-parse --verify HEAD)
test -z "$(/usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" \
  status --porcelain=v1 --untracked-files=all)"
(umask 077 && /usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" bundle create \
  "$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle" HEAD)
test "$(/usr/bin/stat -c %a \
  "$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle")" = 600

export PRISMDRAKE_TRANSPORT_REPOSITORY=\
"$PRISMDRAKE_EVIDENCE_ROOT/prismdrake-transport.git"
test ! -e "$PRISMDRAKE_TRANSPORT_REPOSITORY"
/usr/bin/git clone --bare "$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle" \
  "$PRISMDRAKE_TRANSPORT_REPOSITORY"
/usr/bin/chmod -R u=rwX,go=rX "$PRISMDRAKE_TRANSPORT_REPOSITORY"
/usr/bin/git --git-dir="$PRISMDRAKE_TRANSPORT_REPOSITORY" fsck --full
test "$(/usr/bin/git --git-dir="$PRISMDRAKE_TRANSPORT_REPOSITORY" \
  rev-parse HEAD)" = "$PRISMDRAKE_REVISION"

export EGIT_OVERRIDE_REPO_JTM_ROOTSTORM_PRISMDRAKE_DE=\
"file://$PRISMDRAKE_TRANSPORT_REPOSITORY"
export EGIT_OVERRIDE_COMMIT_JTM_ROOTSTORM_PRISMDRAKE_DE=\
"$PRISMDRAKE_REVISION"

install -d -m 0755 /etc/portage/env /etc/portage/package.env
cat >/etc/portage/env/prismdrake-package-validation.env <<EOF
USE="test"
FEATURES="test keepwork"
EGIT_OVERRIDE_REPO_JTM_ROOTSTORM_PRISMDRAKE_DE=\
"$EGIT_OVERRIDE_REPO_JTM_ROOTSTORM_PRISMDRAKE_DE"
EGIT_OVERRIDE_COMMIT_JTM_ROOTSTORM_PRISMDRAKE_DE=\
"$EGIT_OVERRIDE_COMMIT_JTM_ROOTSTORM_PRISMDRAKE_DE"
EOF
printf '%s\n' \
  'x11-misc/prismdrake prismdrake-package-validation.env' \
  >/etc/portage/package.env/prismdrake-package-validation

emerge --ask --verbose --oneshot --newuse \
  =x11-misc/prismdrake-9999::prismdrake-local
```

git-r3 requires a repository transport and cannot fetch a bundle directly as
`EGIT_OVERRIDE_REPO`; doing so fails before unpack with Git's `too large to be a
.git file` diagnostic. The verified mode-0600 bundle remains the source
attestation artifact. The read-only bare clone is only its local git-r3
transport and must resolve the same exact commit.

The validation environment is bound only to the Prismdrake atom. A command-line
`USE=test` would apply to dependency packages too and can make unrelated Gentoo
test flags conflict with their own `REQUIRED_USE`; do not use it for this gate.
Keep the package environment and bare transport available through the evidence
stages, then restore the preserved Portage inputs after the lifecycle record has
validated. Record the override variable names, bundle digest, and resolved
commit; do not record a private host path in public evidence. A pushed canonical
revision may omit the repository override, but it must still bind the exact
commit used by the evidence.
The evidence root is traversal-only so the ordinary package-test user can reach
its private mode-0700 subdirectory; root-owned evidence files remain mode 0600.

With `FEATURES="test keepwork"`, derive the retained Portage paths rather than
copying or guessing them. The separate CTest build stores the absolute source
directory in `CMakeCache.txt`; moving either tree or substituting an arbitrary
clone makes the package-test collector fail closed:

```bash
PRISMDRAKE_PORTAGE_TMPDIR=$(/usr/bin/portageq envvar PORTAGE_TMPDIR)
PRISMDRAKE_PORTAGE_WORKDIR=\
"${PRISMDRAKE_PORTAGE_TMPDIR%/}/portage/x11-misc/prismdrake-9999/work"
export PRISMDRAKE_SOURCE_CHECKOUT=\
"$PRISMDRAKE_PORTAGE_WORKDIR/prismdrake-9999"
export PRISMDRAKE_TEST_BUILD=\
"$PRISMDRAKE_PORTAGE_WORKDIR/prismdrake-9999_test_build"
export PRISMDRAKE_CONTROLLED_FIXTURE=\
"$PRISMDRAKE_TEST_BUILD/tests/integration/prismdrake-controlled-window-fixture"
export PRISMDRAKE_DURABLE_FIXTURE=\
"$PRISMDRAKE_EVIDENCE_ROOT/harness/prismdrake-controlled-window-fixture"

test -d "$PRISMDRAKE_SOURCE_CHECKOUT/.git"
test -f "$PRISMDRAKE_TEST_BUILD/CMakeCache.txt"
test -x "$PRISMDRAKE_CONTROLLED_FIXTURE"
test "$(/usr/bin/git -C "$PRISMDRAKE_SOURCE_CHECKOUT" rev-parse --verify HEAD)" \
  = "$PRISMDRAKE_REVISION"
test -z "$(/usr/bin/git -C "$PRISMDRAKE_SOURCE_CHECKOUT" \
  status --porcelain=v1 --untracked-files=all)"
test "$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
  "$PRISMDRAKE_TEST_BUILD/CMakeCache.txt")" = "$PRISMDRAKE_SOURCE_CHECKOUT"
test "$(sed -n \
  's/^PRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST:BOOL=//p' \
  "$PRISMDRAKE_TEST_BUILD/CMakeCache.txt")" = ON

install -d -m 0700 "$PRISMDRAKE_EVIDENCE_ROOT/harness"
install -m 0700 "$PRISMDRAKE_CONTROLLED_FIXTURE" \
  "$PRISMDRAKE_DURABLE_FIXTURE"
test -x "$PRISMDRAKE_DURABLE_FIXTURE"
cd "$PRISMDRAKE_SOURCE_CHECKOUT"
```

Complete every source- and retained-build-dependent stage before the ordinary
reinstall, because a later Portage clean may remove the retained work tree. The
`runuser` package-test command intentionally inherits this directory after the
retained trees are assigned to the ordinary test user. The private durable
fixture copy is created before that handoff and before unmerge. Preflight and
both installed demonstrations use this copy, while the package-test collector
continues to require the original fixture inside its exact retained build.

The installed demonstration uses a staged evidence flow so it never depends on
a result that it is itself expected to produce. After the `USE=test` install,
create an exact `git bundle` from the clean source checkout. Capture the
non-final, mode-0600 preflight attestation from absolute paths:

```bash
git -C "$PRISMDRAKE_SOURCE_CHECKOUT" status --porcelain=v1 --untracked-files=all
install -m 0600 "$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle" \
  "$PRISMDRAKE_EVIDENCE_ROOT/source-attestation.bundle"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py preflight \
  --source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT" \
  --source-bundle "$PRISMDRAKE_EVIDENCE_ROOT/source-attestation.bundle" \
  --controlled-window-fixture "$PRISMDRAKE_DURABLE_FIXTURE" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json"
```

The status command must print nothing. The preflight command requires that
same clean checkout, verifies that the bundle's
sole `HEAD` is the checkout `HEAD`, hashes the ebuild from that checkout, and
checks `/usr/share/prismdrake/build-provenance.json` against the same revision.
It hashes the selected VDB ebuild under
`/var/db/pkg/x11-misc/prismdrake-9999`, requires the `prismdrake-local`
repository and actual `test` USE state, parses the VDB `CONTENTS` file, and
cross-checks that complete path-and-kind set against canonical
`/usr/bin/equery -C -q files` output and installed file kinds. VDB comparison
uses the normalized path-and-kind hash, not raw `CONTENTS` bytes whose mtime
fields may legitimately change across an ordinary reinstall. It also measures the fixed
`/usr/bin/prismdrake-*` set and rejects ownership that omits required packaged
data. Its canonical harness manifest covers every imported live helper,
validator, schema, and the controlled fixture binary. The demonstration
recomputes that complete manifest before and after each run.

Execute Portage QA against a disposable copy, both exact pretend resolutions,
the retained package-test selection, the exact four outside-sandbox tests,
installed linkage, and each demo through the collector:

```bash
python3 tests/gentoo/pd2_portage_lifecycle_collector.py qa \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-portage-qa.json"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py resolutions \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-resolutions.json"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py sandbox-tests \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --test-build "$PRISMDRAKE_TEST_BUILD" \
  --source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT" \
  --controlled-window-fixture "$PRISMDRAKE_CONTROLLED_FIXTURE" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-sandbox-tests.json"

install -d -o prismdrake -g prismdrake -m 0700 \
  "$PRISMDRAKE_EVIDENCE_ROOT/non-root"
install -o prismdrake -g prismdrake -m 0600 \
  "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  "$PRISMDRAKE_EVIDENCE_ROOT/non-root/pd2-installed-preflight.json"
chown -R prismdrake:prismdrake \
  "$PRISMDRAKE_SOURCE_CHECKOUT" "$PRISMDRAKE_TEST_BUILD"

runuser --user prismdrake -- \
  python3 tests/gentoo/pd2_portage_lifecycle_collector.py package-test \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/non-root/pd2-installed-preflight.json" \
  --test-build "$PRISMDRAKE_TEST_BUILD" \
  --source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT" \
  --controlled-window-fixture "$PRISMDRAKE_CONTROLLED_FIXTURE" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/non-root/pd2-package-test.json"

chown -R root:root "$PRISMDRAKE_SOURCE_CHECKOUT" "$PRISMDRAKE_TEST_BUILD"
test -z "$(/usr/bin/git -C "$PRISMDRAKE_SOURCE_CHECKOUT" \
  status --porcelain=v1 --untracked-files=all)"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py linkage \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-linkage.json"

python3 tests/integration/pd2_demo.py \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --controlled-window-fixture "$PRISMDRAKE_DURABLE_FIXTURE" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-demonstration.json"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py demonstration \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --phase initial_install \
  --evidence "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-demonstration.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-initial-demonstration.json"
```

The QA stage runs canonical `/usr/bin/pkgdev manifest` and `/usr/bin/pkgcheck
scan --exit error,warning,style,info` against a disposable repository copy;
any QA report fails the stage. The resolution stage verifies the
`prismdrake-local` path and ebuild bytes through `/usr/bin/portageq`, then runs
the fixed `/usr/bin/emerge` pretend command once with the command-line
`USE=-test` override and once with the package-local validation environment.

The two `chown` targets above are disposable guest copies dedicated to the
lifecycle run, never the host checkout or Portage's live repository. They make
the exact clean source and final retained build readable and owned by the
ordinary test user without changing Git bytes or the canonical root-owned
preflight artifact. Run the root-owned outside-sandbox stage before this
ownership handoff; otherwise Git can correctly reject the root observer's
access to the non-root-owned checkout as dubious ownership. Restore the two
disposable trees to root after the non-root package selection and verify the
checkout remains clean before the later root-owned WP15 source attestation.

The CTest collectors require the retained build's exact clean source directory
and configured revision, compare the complete ordered inventory to
`tests/gentoo/pd2-ctest-inventory-v1.json`, and bind the source-owned contract
to every ordered test name, command, and CTest behavior property. They also
require exactly one `PRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST:BOOL=ON` cache entry,
so a dependency-degraded developer registration cannot satisfy the package
contract. Exact source and build roots and the single source-bound configured
revision are normalized before hashing so disposable paths and a later exact
revision do not change the
reviewed contract; diagnostic source-line and backtrace metadata is excluded.
The canonical inventory is captured from the ebuild's retained `USE=test`
tree, where `PRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON` makes the real 45-second,
serial live AT-SPI command part of the contract. A dependency-degraded developer
tree that registers the five-second skip command is intentionally not an
equivalent package-test inventory.
Unknown or newly introduced behavior properties therefore require an explicit
contract review. The collectors additionally hash project-owned command files
and the controlled fixture, then parse CTest JUnit output. A disabled, failed,
missing, additional, or unreviewed skipped selected test fails the stage. The
only reviewed package-selection skip is
`AtomicFileTest.RejectsDestinationOwnedByAnotherUser`, recorded with reason
`requires_root_ownership_transition` and CTest's `status=notrun` plus
`<skipped>` encoding when it actually skips. Exactly the four reviewed tests
are excluded from the package selection. The package-test collector rejects
effective UID 0 and must run as the ordinary `prismdrake` test user against the
final retained build, matching Portage `userpriv`; root remains required for
VDB, preflight, unmerge, and reinstall observation. The
outside-sandbox collector executes the two exact-environment tests once and
both Openbox tests five times against the same test manifest. The collector
uses only canonical absolute tool paths; there are no public tool-path
overrides. Source commands have a 30-second deadline, ordinary observers a
120-second deadline, and Portage and complete CTest selections an 1800-second
deadline.
Output is drained with a fixed limit while each child runs. Linkage is measured
only after the five executable hashes match preflight.

Seed valid nonempty lifecycle fixtures at the ordinary test user's canonical
XDG paths. Stop the installed demonstration and all Prismdrake processes before
the `before` copy so no state owner can race the snapshot. Resolve the account's
home directory rather than assuming `/home/prismdrake`:

```bash
PRISMDRAKE_USER_HOME=$(/usr/bin/getent passwd prismdrake | /usr/bin/cut -d: -f6)
test -n "$PRISMDRAKE_USER_HOME"
export PRISMDRAKE_USER_CONFIG=\
"$PRISMDRAKE_USER_HOME/.config/prismdrake/config.toml"
export PRISMDRAKE_USER_PINS=\
"$PRISMDRAKE_USER_HOME/.local/state/prismdrake/shell/pinned-applications-v1.json"
export PRISMDRAKE_USER_HISTORY=\
"$PRISMDRAKE_USER_HOME/.local/state/prismdrake/notifications/history-v1.json"

install -d -o prismdrake -g prismdrake -m 0700 \
  "$(dirname "$PRISMDRAKE_USER_CONFIG")" \
  "$(dirname "$PRISMDRAKE_USER_PINS")" \
  "$(dirname "$PRISMDRAKE_USER_HISTORY")" \
  "$PRISMDRAKE_EVIDENCE_ROOT/before" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after"
install -o prismdrake -g prismdrake -m 0600 \
  /usr/share/prismdrake/defaults/config.toml "$PRISMDRAKE_USER_CONFIG"
printf '%s\n' '{"schema_version":1,"pins":["org.prismdrake.fixture.desktop"]}' \
  > "$PRISMDRAKE_USER_PINS"
printf '%s\n' '{"schema_version":1,"records":[]}' \
  > "$PRISMDRAKE_USER_HISTORY"
chown prismdrake:prismdrake "$PRISMDRAKE_USER_PINS" "$PRISMDRAKE_USER_HISTORY"
chmod 0600 "$PRISMDRAKE_USER_PINS" "$PRISMDRAKE_USER_HISTORY"

install -m 0600 "$PRISMDRAKE_USER_CONFIG" \
  "$PRISMDRAKE_EVIDENCE_ROOT/before/config.toml"
install -m 0600 "$PRISMDRAKE_USER_PINS" \
  "$PRISMDRAKE_EVIDENCE_ROOT/before/pins.json"
install -m 0600 "$PRISMDRAKE_USER_HISTORY" \
  "$PRISMDRAKE_EVIDENCE_ROOT/before/notification-history.json"
```

Actually unmerge the versioned package before asking the collector to observe
absence. Shared package directories may remain. The collector does not invoke
Portage:

```bash
emerge --ask --unmerge =x11-misc/prismdrake-9999

install -m 0600 "$PRISMDRAKE_USER_CONFIG" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after/config.toml"
install -m 0600 "$PRISMDRAKE_USER_PINS" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after/pins.json"
install -m 0600 "$PRISMDRAKE_USER_HISTORY" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after/notification-history.json"
cmp --silent "$PRISMDRAKE_EVIDENCE_ROOT/before/config.toml" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after/config.toml"
cmp --silent "$PRISMDRAKE_EVIDENCE_ROOT/before/pins.json" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after/pins.json"
cmp --silent "$PRISMDRAKE_EVIDENCE_ROOT/before/notification-history.json" \
  "$PRISMDRAKE_EVIDENCE_ROOT/after/notification-history.json"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py unmerge \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-unmerge-absence.json"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py preservation \
  --before-config-toml "$PRISMDRAKE_EVIDENCE_ROOT/before/config.toml" \
  --before-pins "$PRISMDRAKE_EVIDENCE_ROOT/before/pins.json" \
  --before-notification-history "$PRISMDRAKE_EVIDENCE_ROOT/before/notification-history.json" \
  --after-config-toml "$PRISMDRAKE_EVIDENCE_ROOT/after/config.toml" \
  --after-pins "$PRISMDRAKE_EVIDENCE_ROOT/after/pins.json" \
  --after-notification-history "$PRISMDRAKE_EVIDENCE_ROOT/after/notification-history.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-user-state-preservation.json"
```

After the ordinary non-`USE=test` reinstall, recapture complete ownership and
installed bytes rather than writing reinstall success into a draft:

```bash
USE="-test" FEATURES="-test -keepwork" \
  emerge --ask --verbose --oneshot --newuse \
  =x11-misc/prismdrake-9999::prismdrake-local

cd "$PRISMDRAKE_INPUT_CHECKOUT"
test "$(/usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" rev-parse --verify HEAD)" \
  = "$PRISMDRAKE_REVISION"
test -z "$(/usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" \
  status --porcelain=v1 --untracked-files=all)"
test -x "$PRISMDRAKE_DURABLE_FIXTURE"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py reinstall \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-ordinary-reinstall.json"

python3 tests/integration/pd2_demo.py \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --controlled-window-fixture "$PRISMDRAKE_DURABLE_FIXTURE" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-reinstalled-demonstration.json"

python3 tests/gentoo/pd2_portage_lifecycle_collector.py demonstration \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --phase ordinary_reinstall \
  --evidence "$PRISMDRAKE_EVIDENCE_ROOT/pd2-reinstalled-demonstration.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-reinstalled-demonstration-record.json"
```

The second demonstration deliberately runs from the original clean exact input
checkout, not a retained Portage work directory. Rechecking its revision and
complete tracked/untracked status after reinstall binds every recomputed harness
file to the same source while the durable fixture preserves the exact tested
binary after Portage is free to remove its build tree.

After every phase has actually completed, assemble only the strict collector
stage artifacts into the tracked lifecycle schema:

```bash
python3 tests/gentoo/pd2_portage_lifecycle_collector.py finalize \
  --attestation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-installed-preflight.json" \
  --qa "$PRISMDRAKE_EVIDENCE_ROOT/pd2-portage-qa.json" \
  --resolutions "$PRISMDRAKE_EVIDENCE_ROOT/pd2-resolutions.json" \
  --package-test "$PRISMDRAKE_EVIDENCE_ROOT/non-root/pd2-package-test.json" \
  --linkage "$PRISMDRAKE_EVIDENCE_ROOT/pd2-linkage.json" \
  --sandbox-tests "$PRISMDRAKE_EVIDENCE_ROOT/pd2-sandbox-tests.json" \
  --initial-demonstration "$PRISMDRAKE_EVIDENCE_ROOT/pd2-initial-demonstration.json" \
  --unmerge "$PRISMDRAKE_EVIDENCE_ROOT/pd2-unmerge-absence.json" \
  --preservation "$PRISMDRAKE_EVIDENCE_ROOT/pd2-user-state-preservation.json" \
  --ordinary-reinstall "$PRISMDRAKE_EVIDENCE_ROOT/pd2-ordinary-reinstall.json" \
  --reinstall-demonstration "$PRISMDRAKE_EVIDENCE_ROOT/pd2-reinstalled-demonstration-record.json" \
  --output "$PRISMDRAKE_EVIDENCE_ROOT/pd2-portage-lifecycle.json"
```

Canonical artifact, ownership, linkage, and controlled XDG-tree hashes bind
the stages without preserving private command output. Every collector output
is published atomically with mode 0600. The tracked version-one lifecycle
schema and semantic validator define the success record for the final
`x11-misc/prismdrake-9999` exercise:

```bash
python3 tests/gentoo/pd2_portage_lifecycle_evidence.py \
  "$PRISMDRAKE_EVIDENCE_ROOT/pd2-portage-lifecycle.json"
```

The validator is read-only. It does not run Portage and a passing synthetic
unit fixture is not lifecycle evidence. A real record must bind one exact
40-character source revision and ebuild hash to both pretend resolutions, the
single tested artifact, installed file ownership and executable hashes,
both version-one complete PD2 installed demonstrations, normalized linkage for
all five executables, all four reviewed outside-sandbox tests, unmerge with
byte-identical user configuration/state preservation, and an ordinary
non-`USE=test` reinstall with the complete original ownership manifest.
The ordinary-reinstall collector repeats the VDB ebuild, repository, non-test
USE state, normalized `CONTENTS` ownership, color-disabled `equery`,
installed-kind, provenance, executable, and ownership measurements directly;
it cannot reuse a caller-provided ownership list. The finalizer likewise has
no complete-draft input. Unknown, partial, mismatched, private, build-tree, or
caller-asserted installed data fails closed.

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
only the libraries and broker required by the five installed Experimental
executables. Qt SVG is a direct shell dependency at the project-wide Qt 6.11
floor. Qt Linguist tools are an unconditional build-only dependency used to
compile the installed pseudo-locale catalogs; qttools is not retained in the
runtime dependency set. Unused Qt image-format plugins are not declared.
Libnotify is a conditional `USE=test` build dependency only so the installed
workflow can exercise the standard `notify-send` client; it is not a product
runtime dependency.
GoogleTest, Python, GLib/PyGObject notification clients, Xvfb, the Xorg dummy
driver, Openbox, X11 RandR,
window-tree inspection, bounded XWD capture, protocol tracing, and input tools,
the hicolor icon fixture, AT-SPI/PyGObject
inspection support, fontconfig, and the fixed test font remain conditional
`USE=test` build dependencies. This complete gate prevents a clean reference-VM
package test from silently registering the live startup, multi-output, icon, or
AT-SPI lanes as skipped merely because their evidence tooling was absent.

The ebuild derives `SOURCE_DATE_EPOCH` from the exact checked-out Git revision
before every configure or compile phase. Qt's resource compiler otherwise
stores checkout-time QML mtimes in the shell resource table, making two clean
builds of the same revision differ even though their executable code is
identical. The lifecycle gate therefore compares the installed executables
from the tested and ordinary reinstall paths byte for byte. This packaging
control does not claim reproducibility across different compiler, linker, or Qt
versions.

Run a reviewed resolution and targeted package test with:

```sh
USE="-test" emerge --pretend --verbose --tree --newuse x11-misc/prismdrake
emerge --pretend --verbose --tree --newuse x11-misc/prismdrake
emerge --ask --verbose --oneshot --newuse x11-misc/prismdrake
```

Hash normalized captures from both pretend graphs separately. The default
resolution proves the ordinary runtime boundary; the package-scoped `USE=test`
resolution and install add only build/test tooling and execute the package
suite. Configure that scoped state through the validation `package.env` binding
shown above; never enable `test` globally for the dependency graph.

Portage's mandatory libsandbox injects `LD_PRELOAD` and `SANDBOX_*` entries into
executed children. The ebuild therefore excludes exactly four upstream tests:

- `DetachedApplicationTest.ExecutesExactArgvWorkingDirectoryAndEnvironmentWithoutShell`
  and
  `LauncherPipelineTest.ExpandsPlansAndLaunchesLiteralArgumentsWithoutAShell`
  require byte-exact child environments and reject the injected variables.
- `X11DockOpenboxIntegrationTest` and
  `TaskControllerOpenboxStabilizationIntegrationTest` remain exact Prismdrake
  X11 contracts, but libsandbox's preload changes the third-party Openbox
  process's client/strut behavior.

Run all four separately outside the Portage sandbox on the same source
revision. Run both exact Openbox tests at least five consecutive times against
the same built artifact. No other test exclusion is permitted by this policy.

Before updating the ebuild, verify its installed file list and dynamic linkage,
run the settings and X11 session behavior from installed paths, unmerge it, and
confirm that user configuration and state remain byte-identical. Reinstall once
without `USE=test` to exercise the ordinary package path. Use a disposable VM
checkpoint and preserve the documented Stage 0 baseline. Current exact results
from the original three-executable lifecycle belong in the PD1 Gentoo package
evidence report; the expanded five-executable results belong in the PD2 exit
evidence rather than this normative workflow document.
