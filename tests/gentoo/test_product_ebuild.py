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


if __name__ == "__main__":
    unittest.main()
