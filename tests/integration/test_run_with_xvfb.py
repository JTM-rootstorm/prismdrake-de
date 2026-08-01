#!/usr/bin/env python3
"""Contract tests for the bounded Xvfb harness timeout override."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import run_with_xvfb


class XvfbHarnessTimeoutTests(unittest.TestCase):
    def test_defaults_remain_unchanged(self) -> None:
        plain = run_with_xvfb.parse_harness_arguments(["/xvfb", "/test"])
        openbox = run_with_xvfb.parse_harness_arguments(
            ["--openbox", "/openbox", "--xprop", "/xprop", "/xvfb", "/test"]
        )
        self.assertEqual(run_with_xvfb.selected_test_timeout(plain), 16)
        self.assertEqual(run_with_xvfb.selected_test_timeout(openbox), 12)

    def test_explicit_openbox_override_accepts_exact_wp15_maximum(self) -> None:
        options = run_with_xvfb.parse_harness_arguments(
            [
                "--openbox",
                "/openbox",
                "--xprop",
                "/xprop",
                "--test-timeout-seconds",
                "4200",
                "/xvfb",
                "/test",
            ]
        )
        self.assertEqual(options.openbox, Path("/openbox"))
        self.assertEqual(options.test_timeout_seconds, 4200)
        self.assertEqual(run_with_xvfb.selected_test_timeout(options), 4200)

    def test_explicit_openbox_override_rejects_above_wp15_maximum(self) -> None:
        with self.assertRaisesRegex(
            run_with_xvfb.HarnessArgumentError,
            "the test timeout exceeds the fixed 4200-second bound",
        ):
            run_with_xvfb.parse_harness_arguments(
                [
                    "--openbox",
                    "/openbox",
                    "--xprop",
                    "/xprop",
                    "--test-timeout-seconds",
                    "4201",
                    "/xvfb",
                    "/test",
                ]
            )

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
            for value in ("0", "01", "+1", "٨٠")
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

    def test_wrapped_command_starts_as_cleanup_owned_process_group(self) -> None:
        process = mock.Mock()
        environment = {"DISPLAY": ":42"}
        with mock.patch.object(
            run_with_xvfb.subprocess, "Popen", return_value=process
        ) as popen:
            self.assertIs(
                run_with_xvfb.start_test_process(
                    Path("/test"), ["--fixture"], environment
                ),
                process,
            )

        popen.assert_called_once_with(
            ["/test", "--fixture"],
            stdin=subprocess.DEVNULL,
            env=environment,
            start_new_session=True,
        )

    def test_process_group_cleanup_terminates_a_live_descendant(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            descendant_pid_path = Path(temporary) / "descendant.pid"
            source = (
                "import pathlib,subprocess,sys,time; "
                "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); "
                "pathlib.Path(sys.argv[1]).write_text(str(child.pid), encoding='ascii'); "
                "time.sleep(60)"
            )
            process = subprocess.Popen(
                [sys.executable, "-c", source, str(descendant_pid_path)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 2.0
                while not descendant_pid_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(descendant_pid_path.is_file())
                descendant_pid = int(descendant_pid_path.read_text(encoding="ascii"))

                self.assertTrue(
                    run_with_xvfb.terminate_test_process_group(
                        process,
                        terminate_timeout_seconds=2.0,
                        kill_timeout_seconds=1.0,
                    )
                )
                self.assertIsNotNone(process.returncode)
                with self.assertRaises(ProcessLookupError):
                    os.kill(descendant_pid, 0)
            finally:
                if run_with_xvfb.process_group_exists(process.pid):
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2.0)

    def test_process_group_cleanup_escalates_to_kill_after_term_bound(self) -> None:
        process = mock.Mock(pid=1234)
        with (
            mock.patch.object(run_with_xvfb, "process_group_exists", return_value=True),
            mock.patch.object(
                run_with_xvfb,
                "wait_for_test_process_group_exit",
                side_effect=(False, True),
            ) as wait_for_exit,
            mock.patch.object(run_with_xvfb.os, "killpg") as kill_group,
        ):
            self.assertTrue(run_with_xvfb.terminate_test_process_group(process))

        self.assertEqual(
            kill_group.call_args_list,
            [mock.call(1234, signal.SIGTERM), mock.call(1234, signal.SIGKILL)],
        )
        self.assertEqual(
            wait_for_exit.call_args_list,
            [
                mock.call(process, run_with_xvfb.TERMINATE_TIMEOUT_SECONDS),
                mock.call(process, run_with_xvfb.KILL_TIMEOUT_SECONDS),
            ],
        )


if __name__ == "__main__":
    unittest.main()
