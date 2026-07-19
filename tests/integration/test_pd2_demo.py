#!/usr/bin/env python3
"""Hermetic contract tests for the PD2 installed demonstration scaffold."""

from __future__ import annotations

import copy
import contextlib
import hashlib
import io
import json
import os
import stat
import sys
import tempfile
import unittest
from collections.abc import Mapping
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_demo import (  # noqa: E402
    EXECUTABLE_NAMES,
    FAILURE_SCENARIOS,
    MAX_EVIDENCE_BYTES,
    PROCESS_ROLES,
    TIER_C_ASSERTIONS,
    TIER_C_STEPS,
    WORKFLOW_STEPS,
    HarnessError,
    Observation,
    load_evidence,
    main,
    run_harness,
    validate_evidence,
    validate_schema,
    write_evidence,
)


REVISION = "a" * 40


class FakeHarnessPort:
    """A deterministic injected port; it performs no system operations."""

    def __init__(self) -> None:
        self.step_results: dict[tuple[str, str], Observation | Exception] = {}
        self.cleanup_results: dict[str, Observation | Exception] = {}
        self.step_calls: list[tuple[str, str]] = []
        self.cleanup_calls: list[str] = []
        self.ownership_calls = 0
        self.executable_error: Exception | None = None
        self.executable_values: Mapping[str, str] = {
            name: hashlib.sha256(name.encode("ascii")).hexdigest()
            for name in EXECUTABLE_NAMES
        }
        self.process_values: Mapping[str, object] = {
            role: object() for role in PROCESS_ROLES
        }

    def installed_executable_hashes(self) -> Mapping[str, str]:
        if self.executable_error is not None:
            raise self.executable_error
        return self.executable_values

    def run_step(self, phase: str, identifier: str) -> Observation:
        self.step_calls.append((phase, identifier))
        result = self.step_results.get((phase, identifier), Observation(True, True))
        if isinstance(result, Exception):
            raise result
        return result

    def capture_process_ownership(self) -> Mapping[str, object]:
        self.ownership_calls += 1
        return self.process_values

    def cleanup(self, phase: str) -> Observation:
        self.cleanup_calls.append(phase)
        result = self.cleanup_results.get(phase, Observation(True, True))
        if isinstance(result, Exception):
            raise result
        return result


class HarnessExecutionTests(unittest.TestCase):
    def test_closed_contract_has_exact_required_cardinality(self) -> None:
        self.assertEqual(len(WORKFLOW_STEPS), 47)
        self.assertEqual(len(FAILURE_SCENARIOS), 17)
        self.assertEqual(len(TIER_C_STEPS), 8)
        self.assertEqual(
            TIER_C_ASSERTIONS,
            (
                "opaque_materials",
                "no_shell_side_capture",
                "no_blur_attempt",
                "complete_keyboard_operation",
                "readable_text_and_visible_focus",
            ),
        )
        self.assertEqual(
            EXECUTABLE_NAMES,
            (
                "prismdrake-session",
                "prismdrake-settingsd",
                "prismdrake-notifyd",
                "prismdrake-shell",
                "prismdrake-shellctl",
            ),
        )

    def test_injected_port_completes_all_steps_and_cleanup(self) -> None:
        port = FakeHarnessPort()
        document = run_harness(REVISION, port)

        validate_evidence(document)
        validate_schema(document)
        self.assertEqual(
            port.step_calls,
            [("installed_workflow", item) for item in WORKFLOW_STEPS[:-1]]
            + [("failure_replay", item) for item in FAILURE_SCENARIOS]
            + [("tier_c_replay", item) for item in TIER_C_STEPS],
        )
        self.assertEqual(
            port.cleanup_calls,
            ["installed_workflow", "failure_replay", "tier_c_replay"],
        )
        self.assertEqual(port.ownership_calls, 1)
        self.assertEqual(
            document["environment"]["session_bus"],
            "isolated_private_session_bus",
        )
        self.assertEqual(
            [record["id"] for record in document["workflow"]],
            list(WORKFLOW_STEPS),
        )
        serialized = json.dumps(document)
        self.assertNotIn("identity", serialized)
        self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", serialized)
        self.assertNotIn("unix:path=", serialized)
        self.assertNotIn("/tmp/", serialized)

    def test_unobserved_step_fails_closed_after_cleanup(self) -> None:
        port = FakeHarnessPort()
        identifier = WORKFLOW_STEPS[8]
        port.step_results[("installed_workflow", identifier)] = Observation(False, False)

        with self.assertRaisesRegex(HarnessError, "^step_unobserved$"):
            run_harness(REVISION, port)
        self.assertEqual(port.cleanup_calls, ["installed_workflow"])

    def test_failed_replay_step_still_cleans_its_phase(self) -> None:
        port = FakeHarnessPort()
        identifier = FAILURE_SCENARIOS[3]
        port.step_results[("failure_replay", identifier)] = Observation(True, False)

        with self.assertRaisesRegex(HarnessError, "^step_failed$"):
            run_harness(REVISION, port)
        self.assertEqual(port.cleanup_calls, ["installed_workflow", "failure_replay"])

    def test_unobserved_cleanup_is_not_retried_or_accepted(self) -> None:
        port = FakeHarnessPort()
        port.cleanup_results["installed_workflow"] = Observation(False, False)

        with self.assertRaisesRegex(HarnessError, "^cleanup_failed$"):
            run_harness(REVISION, port)
        self.assertEqual(port.cleanup_calls, ["installed_workflow"])

    def test_backend_exception_fails_closed_after_cleanup(self) -> None:
        port = FakeHarnessPort()
        port.step_results[("installed_workflow", WORKFLOW_STEPS[4])] = RuntimeError("boom")

        with self.assertRaisesRegex(HarnessError, "^backend_operation_failed$"):
            run_harness(REVISION, port)
        self.assertEqual(port.cleanup_calls, ["installed_workflow"])

    def test_requires_exact_five_executable_identities(self) -> None:
        port = FakeHarnessPort()
        port.executable_values = dict(list(port.executable_values.items())[:-1])

        with self.assertRaisesRegex(HarnessError, "^executable_identity_set_invalid$"):
            run_harness(REVISION, port)
        self.assertEqual(port.step_calls, [])
        self.assertEqual(port.cleanup_calls, [])

    def test_executable_attestation_backend_error_fails_closed(self) -> None:
        port = FakeHarnessPort()
        port.executable_error = RuntimeError("boom")

        with self.assertRaisesRegex(HarnessError, "^backend_operation_failed$"):
            run_harness(REVISION, port)
        self.assertEqual(port.step_calls, [])
        self.assertEqual(port.cleanup_calls, [])

    def test_rejects_duplicate_opaque_process_ownership(self) -> None:
        port = FakeHarnessPort()
        identity = object()
        port.process_values = {role: identity for role in PROCESS_ROLES}

        with self.assertRaisesRegex(HarnessError, "^process_ownership_duplicate$"):
            run_harness(REVISION, port)
        self.assertEqual(port.cleanup_calls, ["installed_workflow"])


class EvidenceValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = run_harness(REVISION, FakeHarnessPort())

    def assert_rejected(self, document: object) -> None:
        with self.assertRaises(HarnessError):
            validate_evidence(document)

    def test_rejects_unknown_root_field(self) -> None:
        document = copy.deepcopy(self.document)
        document["extra"] = True
        self.assert_rejected(document)

    def test_rejects_unobserved_or_reordered_result(self) -> None:
        for mutation in ("unobserved", "reordered"):
            with self.subTest(mutation=mutation):
                document = copy.deepcopy(self.document)
                if mutation == "unobserved":
                    document["workflow"][6]["observed"] = False
                else:
                    document["workflow"][5], document["workflow"][6] = (
                        document["workflow"][6],
                        document["workflow"][5],
                    )
                self.assert_rejected(document)

    def test_rejects_result_and_aggregate_hash_tampering(self) -> None:
        for target in ("result", "aggregate"):
            with self.subTest(target=target):
                document = copy.deepcopy(self.document)
                if target == "result":
                    document["failure_replay"][0]["result_sha256"] = "0" * 64
                else:
                    document["normalized_results_sha256"] = "0" * 64
                self.assert_rejected(document)

    def test_rejects_private_runtime_fields(self) -> None:
        document = copy.deepcopy(self.document)
        document["process_ownership"]["pid"] = 123
        self.assert_rejected(document)

    def test_strict_loader_rejects_duplicate_nonfinite_and_oversized_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cases = {
                "duplicate.json": b'{"value":1,"value":2}',
                "nonfinite.json": b'{"value":NaN}',
                "oversized.json": b" " * (MAX_EVIDENCE_BYTES + 1),
            }
            for name, payload in cases.items():
                with self.subTest(name=name):
                    item = root / name
                    item.write_bytes(payload)
                    with self.assertRaises(HarnessError):
                        load_evidence(item)

    def test_writer_is_private_atomic_and_round_trips(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "evidence.json"
            write_evidence(output, self.document)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            self.assertEqual(load_evidence(output), self.document)
            self.assertFalse(output.with_suffix(".json.tmp").exists())

    def test_validator_cli_accepts_only_supplied_closed_evidence(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main([]), 2)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "evidence.json"
            write_evidence(output, self.document)
            self.assertEqual(main(["--validate-evidence", os.fspath(output)]), 0)


if __name__ == "__main__":
    unittest.main()
