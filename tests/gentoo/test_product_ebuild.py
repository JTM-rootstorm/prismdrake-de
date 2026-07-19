#!/usr/bin/env python3
"""Hermetic contract tests for the live Gentoo product ebuild."""

from __future__ import annotations

from pathlib import Path
import re
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

    def test_qt_product_dependencies_keep_the_6_11_floor(self) -> None:
        expected = {
            ">=dev-qt/qtbase-6.11:6",
            ">=dev-qt/qtdeclarative-6.11:6",
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

    def test_qt_linguist_tools_are_an_unconditional_build_dependency(self) -> None:
        bdepend = dependency_body(self.source, "BDEPEND")
        self.assertIn(">=dev-qt/qttools-6.11:6[linguist]", bdepend)
        self.assertNotIn("dev-qt/qttools", dependency_body(self.source, "DEPEND"))
        self.assertNotIn("dev-qt/qttools", dependency_body(self.source, "RDEPEND"))

    def test_unimplemented_qt_image_plugins_and_libnotify_are_not_declared(self) -> None:
        self.assertNotIn("dev-qt/qtimageformats", self.source)
        self.assertNotIn("x11-libs/libnotify", self.source)

    def test_live_observer_display_and_icon_fixtures_are_test_only(self) -> None:
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
            'PRISMDRAKE_PORTAGE_TMPDIR=$(/usr/bin/portageq envvar PORTAGE_TMPDIR)',
            '"$PRISMDRAKE_PORTAGE_WORKDIR/prismdrake-9999_test_build"',
            '--source-checkout "$PRISMDRAKE_SOURCE_CHECKOUT"',
            '--controlled-window-fixture "$PRISMDRAKE_CONTROLLED_FIXTURE"',
            'emerge --ask --unmerge =x11-misc/prismdrake-9999',
            'USE="-test" FEATURES="-test -keepwork"',
            'pinned-applications-v1.json',
            'notifications/history-v1.json',
        ):
            with self.subTest(command=command):
                self.assertIn(command, self.source)
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
        self.assertIn(
            'USE="test" emerge --pretend --verbose --tree --newuse '
            "x11-misc/prismdrake",
            self.source,
        )
        self.assertIn('USE="test" FEATURES="test keepwork"', self.source)


if __name__ == "__main__":
    unittest.main()
