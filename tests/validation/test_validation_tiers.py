#!/usr/bin/env python3
"""Tests for the explicit Prismdrake validation-tier boundary."""

from __future__ import annotations

import importlib.util
import io
import json
import subprocess
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "validation_tiers.py"
SPEC = importlib.util.spec_from_file_location("prismdrake_validation_tiers", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
validation_tiers = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validation_tiers
SPEC.loader.exec_module(validation_tiers)


class ValidationTierTests(unittest.TestCase):
    def test_all_tiers_are_present_and_names_are_unique(self) -> None:
        self.assertEqual(
            {entry.tier for entry in validation_tiers.SELECTORS},
            {"T0", "T1", "T2", "T3", "T4"},
        )
        names = [entry.name for entry in validation_tiers.SELECTORS]
        self.assertEqual(len(names), len(set(names)))

    def test_external_tiers_have_no_local_command(self) -> None:
        external = [
            entry
            for entry in validation_tiers.SELECTORS
            if entry.tier in {"T3", "T4"}
        ]
        self.assertEqual(len(external), 2)
        for entry in external:
            self.assertIsNone(entry.command)
            self.assertFalse(entry.runnable_locally)

    def test_local_selector_executes_fixed_argv_without_a_shell(self) -> None:
        completed = subprocess.CompletedProcess(args=(), returncode=17)
        with mock.patch.object(
            validation_tiers.subprocess, "run", return_value=completed
        ) as run:
            with redirect_stdout(io.StringIO()):
                result = validation_tiers.main(
                    ["--selector", "contracts", "--tier", "T0", "--run"]
                )

        self.assertEqual(result, 17)
        run.assert_called_once_with(
            (sys.executable, "tools/validate.py"),
            cwd=ROOT,
            check=False,
        )
        self.assertNotIn("shell", run.call_args.kwargs)

    def test_external_selector_refuses_to_run(self) -> None:
        error = io.StringIO()
        with mock.patch.object(validation_tiers.subprocess, "run") as run:
            with redirect_stdout(io.StringIO()), redirect_stderr(error):
                result = validation_tiers.main(
                    ["--selector", "package-checkpoint", "--run"]
                )

        self.assertEqual(result, 2)
        self.assertIn("refusing implicit T3 escalation", error.getvalue())
        run.assert_not_called()

    def test_tier_mismatch_does_not_run(self) -> None:
        with mock.patch.object(validation_tiers.subprocess, "run") as run:
            with redirect_stderr(io.StringIO()):
                result = validation_tiers.main(
                    ["--selector", "contracts", "--tier", "T1", "--run"]
                )

        self.assertEqual(result, 2)
        run.assert_not_called()

    def test_json_listing_is_bounded_and_machine_readable(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            result = validation_tiers.main(["--list", "--tier", "T4", "--json"])

        self.assertEqual(result, 0)
        records = json.loads(output.getvalue())
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["name"], "live-environment")
        self.assertIsNone(records[0]["command"])


if __name__ == "__main__":
    unittest.main()
