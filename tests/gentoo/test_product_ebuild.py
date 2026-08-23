#!/usr/bin/env python3
"""Hermetic contract tests for the live Gentoo product ebuild."""

from __future__ import annotations

from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
EBUILD = (
    ROOT
    / "packaging/gentoo/repository/x11-misc/prismdrake/prismdrake-9999.ebuild"
)
DEV_ENV_EBUILD = (
    ROOT
    / "packaging/gentoo/repository/dev-util/prismdrake-dev-env/"
    "prismdrake-dev-env-0.1.ebuild"
)
PACKAGING_GUIDE = ROOT / "docs/packaging/gentoo-local-repository.md"


def dependency_body(source: str, variable: str) -> str:
    match = re.search(
        rf'^{re.escape(variable)}="\n(?P<body>.*?)^"$',
        source,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing ebuild dependency variable: {variable}")
    return match.group("body")


def phase_body(source: str, phase: str) -> str:
    match = re.search(
        rf"^{re.escape(phase)}\(\) \{{\n(?P<body>.*?)^\}}$",
        source,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing ebuild phase: {phase}")
    return match.group("body")


def use_dependency_body(source: str, flag: str) -> str:
    match = re.search(
        rf"^\t{re.escape(flag)}\? \(\n(?P<body>.*?)^\t\)$",
        source,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing ebuild USE dependency block: {flag}")
    return match.group("body")


class ProductEbuildContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EBUILD.read_text(encoding="utf-8")

    def test_qt_resource_timestamp_comes_from_exact_revision(self) -> None:
        helper = phase_body(self.source, "prismdrake_set_source_date_epoch")
        self.assertIn('git -C "${S}" show -s --format=%ct HEAD', helper)
        self.assertIn("=~ ^[1-9][0-9]*$", helper)
        self.assertIn("export SOURCE_DATE_EPOCH=${source_date_epoch}", helper)

    def test_each_resource_generating_phase_sets_source_date_epoch(self) -> None:
        for phase in ("src_configure", "src_compile", "src_test"):
            with self.subTest(phase=phase):
                body = phase_body(self.source, phase)
                self.assertIn("prismdrake_set_source_date_epoch", body)

    def test_rust_toolchain_floor_is_set_before_cargo_eclass_inheritance(self) -> None:
        floor = self.source.index('RUST_MIN_VER="1.96.0"')
        inheritance = self.source.index("inherit cargo cmake git-r3 python-any-r1")
        self.assertLess(floor, inheritance)
        preinherit = self.source[:inheritance]
        for assignment in (
            'CRATES=""',
            'CRATE_PATHS_OVERRIDE=""',
            'CARGO_BOOTSTRAP=""',
            'CARGO_OPTIONAL=""',
            'RUST_OPTIONAL=""',
            "declare -A GIT_CRATES=()",
        ):
            with self.subTest(assignment=assignment):
                self.assertIn(assignment, preinherit)

    def test_cargo_uses_an_empty_offline_vendor_source(self) -> None:
        self.assertNotIn("cargo_live_src_unpack", self.source)
        prepare = phase_body(self.source, "src_prepare")
        self.assertIn("cmake_src_prepare", prepare)
        self.assertNotRegex(prepare, r"(?m)^\s*default\s*$")
        self.assertIn('mkdir -p "${ECARGO_VENDOR}"', prepare)
        self.assertIn("cargo_gen_config", prepare)
        self.assertIn("prismdrake_prepare_cargo_environment", prepare)

    def test_cargo_workspace_has_one_locked_offline_build_and_test_call(self) -> None:
        setup = phase_body(self.source, "pkg_setup")
        self.assertEqual(setup.count("rust_pkg_setup"), 1)

        configure = phase_body(self.source, "src_configure")
        cargo_configure = re.search(
            r"(?ms)^\tcargo_src_configure \\\n"
            r"\t\t--manifest-path \"\$\{S\}/Cargo\.toml\" \\\n"
            r"\t\t--workspace \\\n"
            r"\t\t--locked \\\n"
            r"\t\t--offline$",
            configure,
        )
        self.assertIsNotNone(cargo_configure)
        self.assertEqual(configure.count("cargo_src_configure"), 1)

        compile_phase = phase_body(self.source, "src_compile")
        self.assertEqual(compile_phase.count("cargo_src_compile"), 1)

        test_phase = phase_body(self.source, "src_test")
        self.assertEqual(test_phase.count("cargo_src_test --all-targets"), 1)

        self.assertNotIn("cargo install", self.source)
        self.assertNotIn("cargo_src_install", self.source)
        self.assertNotIn("rustup", self.source)

    def test_cargo_environment_helper_removes_poison_and_binds_state(self) -> None:
        build_root = ROOT / "build"
        build_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="ebuild-cargo-environment-", dir=build_root
        ) as temporary:
            temporary_path = Path(temporary)
            tool_directory = temporary_path / "toolchain" / "bin"
            tool_directory.mkdir(parents=True)
            for tool in ("cargo", "rustc", "rustdoc"):
                path = tool_directory / tool
                path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                path.chmod(0o755)

            work = temporary_path / "work"
            cargo_home = work / "cargo-home"
            cargo_home.mkdir(parents=True)
            helper = phase_body(
                self.source, "prismdrake_prepare_cargo_environment"
            )
            removed = (
                "RUSTC_WRAPPER",
                "RUSTC_WORKSPACE_WRAPPER",
                "CARGO_BUILD_RUSTC_WRAPPER",
                "CARGO_CREDENTIAL_ALIAS_EVIL",
                "CARGO_REGISTRIES_PRIVATE_TOKEN",
                "CARGO_TARGET_FAKE_RUNNER",
                "CARGO_TARGET_FAKE_LINKER",
                "CARGO_TARGET_FAKE_RUSTFLAGS",
                "ECARGO_EXTRA_ARGS",
                "AWS_SECRET_ACCESS_KEY",
                "GITHUB_TOKEN",
                "GOOGLE_APPLICATION_CREDENTIALS",
                "OPENAI_API_KEY",
                "HTTPS_PROXY",
                "https_proxy",
                "SSH_AUTH_SOCK",
            )
            assertions = "\n".join(
                f'[[ -z "${{{name}+x}}" ]] || exit 41' for name in removed
            )
            script = f"""
set -eu
die() {{ exit 97; }}
LD_PRELOAD=/portage/libsandbox.so
SANDBOX_ON=1
CC=/usr/bin/gcc
CFLAGS=-O2
RUSTFLAGS='-C debuginfo=1'
prismdrake_prepare_cargo_environment() {{
{helper}
}}
prismdrake_prepare_cargo_environment
{assertions}
[[ $LD_PRELOAD == /portage/libsandbox.so ]] || exit 40
[[ $SANDBOX_ON == 1 ]] || exit 40
[[ $CC == /usr/bin/gcc ]] || exit 40
[[ $CFLAGS == -O2 ]] || exit 40
[[ $RUSTFLAGS == '-C debuginfo=1' ]] || exit 40
[[ $CARGO_HOME == {shlex.quote(str(cargo_home))} ]] || exit 42
[[ $CARGO_TARGET_DIR == {shlex.quote(str(work / 'cargo_target'))} ]] || exit 43
[[ $CARGO_NET_OFFLINE == true ]] || exit 44
[[ $CARGO_INCREMENTAL == 0 ]] || exit 45
[[ $RUSTDOC == {shlex.quote(str(tool_directory / 'rustdoc'))} ]] || exit 46
"""
            environment = {
                "CARGO": str(tool_directory / "cargo"),
                "RUSTC": str(tool_directory / "rustc"),
                "ECARGO_HOME": str(cargo_home),
                "WORKDIR": str(work),
                **{name: "/poisoned/value" for name in removed},
            }
            result = subprocess.run(
                ("/bin/bash", "--noprofile", "--norc", "-c", script),
                cwd=ROOT,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=10,
            )
            self.assertEqual(
                result.returncode,
                0,
                result.stderr.decode("utf-8", errors="replace"),
            )

    def test_every_cargo_phase_sanitizes_before_cargo_execution(self) -> None:
        expected_calls = {
            "src_configure": "cargo_src_configure",
            "src_compile": "cargo_src_compile",
            "src_test": "cargo_src_test",
        }
        for phase, cargo_call in expected_calls.items():
            with self.subTest(phase=phase):
                body = phase_body(self.source, phase)
                self.assertLess(
                    body.index("prismdrake_prepare_cargo_environment"),
                    body.index(cargo_call),
                )

    def test_product_and_test_builds_embed_the_exact_source_revision(self) -> None:
        helper = phase_body(self.source, "prismdrake_source_revision")
        self.assertIn('git -C "${S}" rev-parse --verify HEAD', helper)
        self.assertIn("=~ ^[0-9a-f]{40}$", helper)
        for phase in ("src_configure", "src_test"):
            with self.subTest(phase=phase):
                self.assertIn(
                    '-DPRISMDRAKE_SOURCE_REVISION="$(prismdrake_source_revision)"',
                    phase_body(self.source, phase),
                )

    def test_retained_test_build_requires_the_live_atspi_registration(self) -> None:
        self.assertIn(
            "-DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON",
            phase_body(self.source, "src_test"),
        )
        self.assertIn(
            "-DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=OFF",
            phase_body(self.source, "src_configure"),
        )

    def test_qt_product_dependencies_keep_the_6_11_floor(self) -> None:
        expected = {
            ">=dev-qt/qtbase-6.11:6",
            ">=dev-qt/qtdeclarative-6.11:6=",
            ">=dev-qt/qtsvg-6.11:6",
        }
        for variable in ("DEPEND", "RDEPEND"):
            body = dependency_body(self.source, variable)
            with self.subTest(variable=variable):
                qt_atoms = {
                    line.strip().split("[", maxsplit=1)[0]
                    for line in body.splitlines()
                    if "dev-qt/" in line
                }
                self.assertEqual(qt_atoms, expected)

    def test_qtdeclarative_private_abi_changes_trigger_rebuilds(self) -> None:
        for variable in ("DEPEND", "RDEPEND"):
            body = dependency_body(self.source, variable)
            with self.subTest(variable=variable):
                self.assertIn(
                    ">=dev-qt/qtdeclarative-6.11:6=[accessibility,opengl]", body
                )

    def test_qt_linguist_tools_are_an_unconditional_build_dependency(self) -> None:
        bdepend = dependency_body(self.source, "BDEPEND")
        self.assertIn(">=dev-qt/qttools-6.11:6[linguist]", bdepend)
        self.assertNotIn("dev-qt/qttools", dependency_body(self.source, "DEPEND"))
        self.assertNotIn("dev-qt/qttools", dependency_body(self.source, "RDEPEND"))

    def test_unimplemented_qt_image_plugins_are_not_declared(self) -> None:
        self.assertNotIn("dev-qt/qtimageformats", self.source)

    def test_live_observer_and_standards_fixtures_are_test_only(self) -> None:
        bdepend = dependency_body(self.source, "BDEPEND")
        test_bdepend = use_dependency_body(self.source, "test")
        depend = dependency_body(self.source, "DEPEND")
        rdepend = dependency_body(self.source, "RDEPEND")

        for atom in (
            "app-accessibility/at-spi2-core[X,introspection]",
            "dev-cpp/gtest",
            "dev-libs/glib",
            "gnome-base/gsettings-desktop-schemas",
            "media-fonts/dejavu",
            "media-libs/fontconfig",
            "x11-apps/xev",
            "x11-apps/xprop",
            "x11-apps/xrandr",
            "x11-apps/xwininfo",
            "x11-apps/xwd",
            "x11-base/xorg-server[xvfb]",
            "x11-drivers/xf86-video-dummy",
            "x11-libs/libnotify",
            "x11-misc/xdotool",
            "x11-misc/xtrace",
            "x11-themes/hicolor-icon-theme",
            "x11-wm/openbox",
        ):
            with self.subTest(atom=atom):
                self.assertIn(atom, test_bdepend)
                self.assertEqual(bdepend.count(atom), 1)
                self.assertNotIn(atom, depend)
                self.assertNotIn(atom, rdepend)

    def test_protocol_tracer_uses_the_gentoo_xtrace_package(self) -> None:
        bdepend = dependency_body(self.source, "BDEPEND")
        self.assertEqual(bdepend.count("x11-misc/xtrace"), 1)
        self.assertNotIn("sys-libs/glibc[xtrace]", self.source)

    def test_forbidden_gnome_desktop_stack_is_not_a_runtime_dependency(self) -> None:
        runtime = dependency_body(self.source, "DEPEND") + dependency_body(
            self.source, "RDEPEND"
        )
        for atom in (
            "gnome-base/gnome-shell",
            "gnome-base/gnome-control-center",
            "gnome-base/gnome-settings-daemon",
            "gui-libs/libadwaita",
            "x11-wm/mutter",
        ):
            with self.subTest(atom=atom):
                self.assertNotIn(atom, runtime)

    def test_documentation_and_postinstall_notice_cover_the_pd2_boundaries(self) -> None:
        install = phase_body(self.source, "src_install")
        for document in (
            "README.md",
            "docs/index.md",
            "docs/architecture/component-model.md",
            "docs/architecture/configuration.md",
            "docs/architecture/dependency-policy.md",
            "docs/architecture/notification-service.md",
            "docs/architecture/pd2-core-shell.md",
            "docs/architecture/pd2-state-ownership.md",
            "docs/architecture/process-model.md",
            "docs/architecture/shell-control.md",
            "docs/architecture/surface-placement.md",
            "docs/build/building.md",
            "docs/build/dependencies.md",
            "docs/build/testing.md",
            "docs/development/accessibility-testing.md",
            "docs/development/pd2-installed-demonstration.md",
            "docs/packaging/gentoo-local-repository.md",
            "interfaces/README.md",
        ):
            with self.subTest(document=document):
                self.assertIn(document, install)
        for destination in (
            "architecture",
            "build",
            "development",
            "packaging",
            "interfaces",
        ):
            with self.subTest(destination=destination):
                self.assertEqual(install.count(f"docinto {destination}"), 1)

        postinst = phase_body(self.source, "pkg_postinst")
        for executable in (
            "prismdrake-session",
            "prismdrake-settingsd",
            "prismdrake-notifyd",
            "prismdrake-shell",
            "prismdrake-shellctl",
        ):
            with self.subTest(executable=executable):
                self.assertIn(executable, postinst)
        for interface in ("Settings1", "NotificationCenter1", "Shell1", "Session1"):
            with self.subTest(interface=interface):
                self.assertIn(interface, postinst)


class DevelopmentEbuildContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = DEV_ENV_EBUILD.read_text(encoding="utf-8")

    def test_live_x11_observers_are_scoped_to_the_x11_tooling_layer(self) -> None:
        x11 = use_dependency_body(self.source, "x11")
        for atom in ("x11-apps/xwininfo", "x11-misc/xtrace"):
            with self.subTest(atom=atom):
                self.assertEqual(x11.count(atom), 1)

        non_x11 = self.source.replace(f"\tx11? (\n{x11}\t)", "")
        self.assertNotIn("x11-apps/xwininfo", non_x11)
        self.assertNotIn("x11-misc/xtrace", non_x11)

    def test_qt_tooling_layer_includes_linguist_catalog_compiler(self) -> None:
        qt6 = use_dependency_body(self.source, "qt6")
        atoms = {
            line.strip().split("[", maxsplit=1)[0]
            for line in qt6.splitlines()
            if "dev-qt/" in line
        }
        self.assertEqual(
            atoms,
            {
                ">=dev-qt/qtbase-6.11:6",
                ">=dev-qt/qtdeclarative-6.11:6",
                ">=dev-qt/qtsvg-6.11:6",
                ">=dev-qt/qttools-6.11:6",
            },
        )
        self.assertIn(">=dev-qt/qttools-6.11:6[linguist]", qt6)

    def test_x11_protocol_tracer_is_not_promoted_to_other_tooling_layers(self) -> None:
        self.assertNotIn(
            "x11-misc/xtrace", use_dependency_body(self.source, "debug-tools")
        )
        self.assertNotIn(
            "x11-misc/xtrace", use_dependency_body(self.source, "visual-tests")
        )


class PackagingGuideContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = PACKAGING_GUIDE.read_text(encoding="utf-8")

    def test_pd2_lifecycle_commands_use_current_collectors_and_driver(self) -> None:
        for command in (
            "pd2_portage_lifecycle_collector.py preflight",
            "pd2_portage_lifecycle_collector.py qa",
            "pd2_portage_lifecycle_collector.py resolutions",
            "pd2_portage_lifecycle_collector.py package-test",
            "pd2_portage_lifecycle_collector.py sandbox-tests",
            "pd2_portage_lifecycle_collector.py linkage",
            "pd2_portage_lifecycle_collector.py demonstration",
            "pd2_portage_lifecycle_collector.py reinstall",
            "pd2_portage_lifecycle_collector.py preservation",
            "pd2_portage_lifecycle_collector.py unmerge",
            "pd2_portage_lifecycle_collector.py finalize",
            "pd2_portage_lifecycle_evidence.py",
            'EGIT_OVERRIDE_REPO_JTM_ROOTSTORM_PRISMDRAKE_DE=',
            'EGIT_OVERRIDE_COMMIT_JTM_ROOTSTORM_PRISMDRAKE_DE=',
            'git clone --bare "$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle"',
            'file://$PRISMDRAKE_TRANSPORT_REPOSITORY',
            'PRISMDRAKE_PORTAGE_TMPDIR=$(/usr/bin/portageq envvar PORTAGE_TMPDIR)',
            '"$PRISMDRAKE_PORTAGE_WORKDIR/prismdrake-9999_test_build"',
            '--source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT"',
            '--controlled-window-fixture "$PRISMDRAKE_CONTROLLED_FIXTURE"',
            '--controlled-window-fixture "$PRISMDRAKE_DURABLE_FIXTURE"',
            'emerge --ask --unmerge =x11-misc/prismdrake-9999',
            'USE="-test" FEATURES="-test -keepwork"',
            'PRISMDRAKE_DURABLE_FIXTURE=',
            'install -m 0700 "$PRISMDRAKE_CONTROLLED_FIXTURE"',
            'pinned-applications-v1.json',
            'notifications/history-v1.json',
        ):
            with self.subTest(command=command):
                self.assertIn(command, self.source)
        self.assertNotIn(
            'file://$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle', self.source
        )
        for stale in (
            "tests/gentoo/portage_lifecycle_collector.py",
            "tests/gentoo/portage_lifecycle_evidence.py",
            "--owned-files",
            "tests/integration/pd1_demo.py",
            "pd1-portage-lifecycle-draft.json",
            "--owned-paths",
            "--draft",
        ):
            with self.subTest(stale=stale):
                self.assertNotIn(stale, self.source)

    def test_product_resolution_commands_separate_normal_and_test_modes(self) -> None:
        self.assertIn(
            'USE="-test" emerge --pretend --verbose --tree --newuse '
            "x11-misc/prismdrake",
            self.source,
        )
        self.assertIn('USE="test"', self.source)
        self.assertIn('FEATURES="test keepwork"', self.source)
        self.assertIn(
            "x11-misc/prismdrake prismdrake-package-validation.env",
            self.source,
        )
        self.assertNotIn(
            'USE="test" emerge --pretend --verbose --tree --newuse',
            self.source,
        )

    def test_reinstall_demo_uses_durable_fixture_and_exact_input_checkout(self) -> None:
        durable_copy = self.source.index(
            'install -m 0700 "$PRISMDRAKE_CONTROLLED_FIXTURE"'
        )
        unmerge = self.source.index(
            "emerge --ask --unmerge =x11-misc/prismdrake-9999"
        )
        reinstall = self.source.index('USE="-test" FEATURES="-test -keepwork"')
        input_checkout = self.source.index(
            'cd "$PRISMDRAKE_INPUT_CHECKOUT"', reinstall
        )
        revision_check = self.source.index(
            '/usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" rev-parse --verify HEAD',
            input_checkout,
        )
        status_check = self.source.index(
            '/usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT"', revision_check + 1
        )
        second_demo = self.source.index(
            '"$PRISMDRAKE_EVIDENCE_ROOT/pd2-reinstalled-demonstration.json"',
            status_check,
        )

        self.assertLess(durable_copy, unmerge)
        self.assertLess(unmerge, reinstall)
        self.assertLess(reinstall, input_checkout)
        self.assertLess(input_checkout, revision_check)
        self.assertLess(revision_check, status_check)
        self.assertLess(status_check, second_demo)
        self.assertEqual(
            self.source.count(
                '--controlled-window-fixture "$PRISMDRAKE_DURABLE_FIXTURE"'
            ),
            3,
        )
        self.assertEqual(
            self.source.count("python3 tests/integration/pd2_demo.py"), 2
        )
        reinstall_boundary = self.source[input_checkout:second_demo]
        self.assertIn(
            "status --porcelain=v1 --untracked-files=all", reinstall_boundary
        )
        self.assertNotIn("$PRISMDRAKE_SOURCE_CHECKOUT", reinstall_boundary)

    def test_unpublished_source_bundle_is_private_at_creation(self) -> None:
        self.assertIn(
            '(umask 077 && /usr/bin/git -C "$PRISMDRAKE_INPUT_CHECKOUT" '
            "bundle create",
            self.source,
        )
        self.assertIn(
            '"$PRISMDRAKE_EVIDENCE_ROOT/prismdrake.bundle")" = 600',
            self.source,
        )

    def test_root_source_observers_precede_non_root_ownership_handoff(self) -> None:
        sandbox = self.source.index(
            "pd2_portage_lifecycle_collector.py sandbox-tests"
        )
        ownership_handoff = self.source.index("chown -R prismdrake:prismdrake")
        package_test = self.source.index(
            "pd2_portage_lifecycle_collector.py package-test"
        )
        ownership_restore = self.source.index(
            'chown -R root:root "$PRISMDRAKE_SOURCE_CHECKOUT"'
        )
        self.assertLess(sandbox, ownership_handoff)
        self.assertLess(ownership_handoff, package_test)
        self.assertLess(package_test, ownership_restore)
        restored_source = self.source[ownership_restore:]
        self.assertIn('git -C "$PRISMDRAKE_SOURCE_CHECKOUT"', restored_source)
        self.assertIn(
            "status --porcelain=v1 --untracked-files=all", restored_source
        )


if __name__ == "__main__":
    unittest.main()
