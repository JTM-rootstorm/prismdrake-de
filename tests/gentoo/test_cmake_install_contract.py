#!/usr/bin/env python3
"""Hermetic contract tests for the PD2 CMake runtime install surface."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
ROOT_CMAKE = ROOT / "CMakeLists.txt"


def install_blocks(source: str) -> tuple[str, ...]:
    return tuple(
        match.group("body")
        for match in re.finditer(
            r"\binstall\(\s*(?P<body>.*?)\)", source, flags=re.DOTALL
        )
    )


class CMakeInstallContractTests(unittest.TestCase):
    def test_exact_five_pd2_executables_are_installed(self) -> None:
        installed_targets: set[str] = set()
        for path in (ROOT / "src").rglob("CMakeLists.txt"):
            for block in install_blocks(path.read_text(encoding="utf-8")):
                match = re.search(r"\bTARGETS\s+([A-Za-z0-9_-]+)", block)
                if match is not None:
                    installed_targets.add(match.group(1))

        self.assertEqual(
            installed_targets,
            {
                "prismdrake-notifyd",
                "prismdrake-session",
                "prismdrake-settingsd",
                "prismdrake-shell",
                "prismdrake-shellctl",
            },
        )

    def test_runtime_data_manifest_is_complete_and_bounded(self) -> None:
        blocks = install_blocks(ROOT_CMAKE.read_text(encoding="utf-8"))
        self.assertEqual(len(blocks), 6)
        self.assertTrue(all("COMPONENT Runtime" in block for block in blocks))

        schemas = {
            item
            for block in blocks
            for item in re.findall(r"schemas/[A-Za-z0-9.-]+\.json", block)
        }
        self.assertEqual(
            schemas,
            {
                "schemas/prismdrake-config.schema.json",
                "schemas/prismdrake-notification-history.schema.json",
                "schemas/prismdrake-notification-state.schema.json",
                "schemas/prismdrake-pin-state.schema.json",
                "schemas/prismdrake-runtime-snapshot.schema.json",
                "schemas/prismdrake-theme-tokens.schema.json",
            },
        )

        interfaces = {
            item
            for block in blocks
            for item in re.findall(
                r"interfaces/dbus/org\.prismdrake\.[A-Za-z0-9]+\.xml", block
            )
        }
        self.assertEqual(
            interfaces,
            {
                "interfaces/dbus/org.prismdrake.NotificationCenter1.xml",
                "interfaces/dbus/org.prismdrake.Session1.xml",
                "interfaces/dbus/org.prismdrake.Settings1.xml",
                "interfaces/dbus/org.prismdrake.Shell1.xml",
            },
        )

        manifest = "\n".join(blocks)
        self.assertIn("data/defaults/config.toml", manifest)
        self.assertIn("data/xsessions/prismdrake.desktop", manifest)
        for profile in ("accessibility", "base", "forge", "lustre"):
            with self.subTest(profile=profile):
                self.assertIn(f"themes/{profile}.tokens.json", manifest)

        translation_block = next(
            block for block in blocks if "prismdrake_translation_qm_files" in block
        )
        self.assertIn(
            'DESTINATION "${CMAKE_INSTALL_DATADIR}/prismdrake/translations"',
            translation_block,
        )
        root_source = ROOT_CMAKE.read_text(encoding="utf-8")
        self.assertEqual(
            set(re.findall(r"translations/(prismdrake_[A-Za-z_]+\.ts)", root_source)),
            {"prismdrake_ar_XB.ts", "prismdrake_en_XA.ts"},
        )

    def test_install_rules_never_create_user_configuration_or_state(self) -> None:
        sources = [ROOT_CMAKE]
        sources.extend((ROOT / "src").rglob("CMakeLists.txt"))
        manifest = "\n".join(
            block
            for path in sources
            for block in install_blocks(path.read_text(encoding="utf-8"))
        )
        for forbidden in (
            "$ENV{HOME}",
            "XDG_CONFIG_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "pinned-applications",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, manifest)


if __name__ == "__main__":
    unittest.main()
