#!/usr/bin/env python3
"""Contract tests for the bounded Xvfb harness timeout override."""

from __future__ import annotations

import unittest
from pathlib import Path

import run_with_xvfb


class XvfbHarnessTimeoutTests(unittest.TestCase):
    def test_defaults_remain_unchanged(self) -> None:
        plain = run_with_xvfb.parse_harness_arguments(["/xvfb", "/test"])
        openbox = run_with_xvfb.parse_harness_arguments(
            ["--openbox", "/openbox", "--xprop", "/xprop", "/xvfb", "/test"]
        )
        self.assertEqual(run_with_xvfb.selected_test_timeout(plain), 16)
        self.assertEqual(run_with_xvfb.selected_test_timeout(openbox), 12)

    def test_explicit_openbox_override_accepts_exact_eighty_seconds(self) -> None:
        options = run_with_xvfb.parse_harness_arguments(
            [
                "--openbox",
                "/openbox",
                "--xprop",
                "/xprop",
                "--test-timeout-seconds",
                "80",
                "/xvfb",
                "/test",
            ]
        )
        self.assertEqual(options.openbox, Path("/openbox"))
        self.assertEqual(options.test_timeout_seconds, 80)
        self.assertEqual(run_with_xvfb.selected_test_timeout(options), 80)

    def test_rejects_open_or_out_of_scope_timeout_values(self) -> None:
        invalid_arguments = [
            ["--test-timeout-seconds", "80", "/xvfb", "/test"]
        ]
        invalid_arguments.extend(
            [
                "--openbox",
                "/openbox",
                "--test-timeout-seconds",
                value,
                "/xvfb",
                "/test",
            ]
            for value in ("0", "01", "+1", "91", "٨٠")
        )
        for arguments in invalid_arguments:
            with self.subTest(arguments=arguments):
                with self.assertRaises(run_with_xvfb.HarnessArgumentError):
                    run_with_xvfb.parse_harness_arguments(arguments)


if __name__ == "__main__":
    unittest.main()
