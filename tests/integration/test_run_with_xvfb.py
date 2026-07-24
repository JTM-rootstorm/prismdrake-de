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

    def test_openbox_locale_does_not_replace_test_locale(self) -> None:
        test_environment = {
            "DISPLAY": ":42",
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "PRISMDRAKE_TEST_EXPECT_EWMH_WM": "1",
        }

        selected = run_with_xvfb.openbox_environment(test_environment)

        self.assertEqual(selected["LC_ALL"], "C")
        self.assertEqual(test_environment["LC_ALL"], "C.UTF-8")
        self.assertEqual(selected["DISPLAY"], ":42")
        self.assertEqual(selected["PRISMDRAKE_TEST_EXPECT_EWMH_WM"], "1")

    def test_failed_child_status_distinguishes_exit_and_signal(self) -> None:
        self.assertEqual(
            run_with_xvfb.failed_test_status(7),
            "test exited with status 7",
        )
        self.assertEqual(
            run_with_xvfb.failed_test_status(-11),
            "test terminated by signal 11 (SIGSEGV)",
        )
        self.assertEqual(
            run_with_xvfb.failed_test_status(-4096),
            "test terminated by signal 4096 (UNKNOWN)",
        )

    def test_process_status_is_bounded_and_categorical(self) -> None:
        self.assertEqual(run_with_xvfb.process_status(None), "running")
        self.assertEqual(run_with_xvfb.process_status(0), "exited with status 0")
        self.assertEqual(run_with_xvfb.process_status(9), "exited with status 9")
        self.assertEqual(
            run_with_xvfb.process_status(-15),
            "terminated by signal 15 (SIGTERM)",
        )


if __name__ == "__main__":
    unittest.main()
