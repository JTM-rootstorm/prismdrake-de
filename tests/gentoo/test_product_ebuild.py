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

    def test_unimplemented_qt_image_plugins_and_libnotify_are_not_declared(self) -> None:
        self.assertNotIn("dev-qt/qtimageformats", self.source)
        self.assertNotIn("x11-libs/libnotify", self.source)

    def test_display_and_icon_fixtures_cannot_leak_into_runtime_dependencies(self) -> None:
        bdepend = dependency_body(self.source, "BDEPEND")
        depend = dependency_body(self.source, "DEPEND")
        rdepend = dependency_body(self.source, "RDEPEND")

        for atom in (
            "x11-apps/xrandr",
            "x11-base/xorg-server[xvfb]",
            "x11-drivers/xf86-video-dummy",
            "x11-themes/hicolor-icon-theme",
        ):
            with self.subTest(atom=atom):
                self.assertIn(atom, bdepend)
                self.assertNotIn(atom, depend)
                self.assertNotIn(atom, rdepend)

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
            "docs/architecture/component-model.md",
            "docs/architecture/process-model.md",
            "docs/architecture/shell-control.md",
            "interfaces/README.md",
        ):
            with self.subTest(document=document):
                self.assertIn(document, install)

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


if __name__ == "__main__":
    unittest.main()
