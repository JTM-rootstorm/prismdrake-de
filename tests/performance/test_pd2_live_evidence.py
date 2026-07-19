#!/usr/bin/env python3
"""Hermetic injected-observation tests for the PD2 WP15 live collector."""

from __future__ import annotations

import json
import stat
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Mapping
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_evidence_contract import (  # noqa: E402
    INTERNATIONALIZATION_CHECKS,
    MATRIX_CHECKS,
    SURFACES,
    EvidenceError,
    validate_evidence_document,
)
from pd2_live_evidence import (  # noqa: E402
    LIVE_METHOD_ID,
    PERFORMANCE_GROUPS,
    BoundResult,
    CommandLiveEvidencePort,
    LiveCollectionConfig,
    ObservationBinding,
    ObserverCommands,
    collect_evidence,
    write_evidence,
)
from collect_pd2_live_evidence import (  # noqa: E402
    build_observer_environment,
    observer_commands,
    reviewed_skip,
)


REVISION = "a" * 40
PD1_REVISION = "b" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"


def passed(method: str = "Injected live observer") -> dict[str, str]:
    return {"status": "passed", "method": method}


def measured(unit: str = "milliseconds") -> dict[str, object]:
    return {
        "status": "observed",
        "unit": unit,
        "samples": [1.0, 2.0, 3.0],
        "method": "Injected monotonic live endpoint",
        "pd1_comparison": {
            "status": "no_regression",
            "statistic": "median",
            "pd1_value": 3.0,
        },
    }


class FakeLivePort:
    def __init__(self) -> None:
        self.binding = ObservationBinding(REVISION, ENVIRONMENT_ID)
        self.performance_calls: list[tuple[str, str | None]] = []
        self.accessibility_calls: list[tuple[str, str]] = []
        self.internationalization_calls: list[str] = []
        self.cleanup_calls = 0
        self.results: dict[str, dict[str, object]] = {}
        self.cleanup_result: Mapping[str, object] = {"clean": True}

    def bound(self, key: str, default: dict[str, object]) -> BoundResult:
        return BoundResult(self.binding, self.results.get(key, default))

    def environment(self) -> BoundResult:
        return self.bound(
            "environment",
            {
                "kind": "reference_vm",
                "reference_environment_id": ENVIRONMENT_ID,
                "operating_system": "Gentoo Linux",
                "architecture": "x86_64",
                "qt_version": "6.11.1",
                "display_backend": "x11",
                "window_manager": "openbox",
                "compilers": [{"family": "gcc", "version": "15.3.0"}],
            },
        )

    def performance(self, group: str, endpoint: str | None) -> BoundResult:
        self.performance_calls.append((group, endpoint))
        unit = "wakeups_per_minute" if group == "idle_wakeups" else "milliseconds"
        key = f"performance.{group}" + ("" if endpoint is None else f".{endpoint}")
        return self.bound(key, measured(unit))

    def accessibility(self, surface: str, check: str) -> BoundResult:
        self.accessibility_calls.append((surface, check))
        return self.bound(f"accessibility.{surface}.{check}", passed())

    def live_atspi(self) -> BoundResult:
        return self.bound("accessibility.live_atspi", passed("Live AT SPI tree replay"))

    def screen_reader_workflow(self) -> BoundResult:
        return self.bound(
            "accessibility.screen_reader_workflow",
            passed("Bounded screen reader workflow"),
        )

    def internationalization(self, check: str) -> BoundResult:
        self.internationalization_calls.append(check)
        return self.bound(f"internationalization.{check}", passed())

    def cleanup(self) -> BoundResult:
        self.cleanup_calls += 1
        return BoundResult(
            ObservationBinding(REVISION, ENVIRONMENT_ID), self.cleanup_result
        )


def config(reviewed_skips: Mapping[str, str] | None = None) -> LiveCollectionConfig:
    return LiveCollectionConfig(
        source_revision=REVISION,
        reference_environment_id=ENVIRONMENT_ID,
        pd1_reference={
            "status": "identified",
            "source_revision": PD1_REVISION,
            "evidence_id": "pd1-reviewed-reference",
        },
        search_catalog_sizes=(10, 100),
        reviewed_skips=reviewed_skips or {},
        limitations=("Reference VM measurements do not define release budgets",),
    )


class LiveCollectorTests(unittest.TestCase):
    def test_collects_all_fifteen_groups_and_full_accessibility_matrix(self) -> None:
        port = FakeLivePort()
        document = collect_evidence(config(), port)

        validate_evidence_document(document)
        self.assertEqual(tuple(document["performance"]), PERFORMANCE_GROUPS)
        self.assertEqual(
            port.performance_calls,
            [
                ("session_ready", None),
                ("shell_complete_presentation", None),
                ("launcher_acknowledgement", None),
                ("search_response", "10"),
                ("search_response", "100"),
                ("catalog_refresh", None),
                ("icon_decode_cache", "decode"),
                ("icon_decode_cache", "cache_hit"),
                ("wallpaper_decode_publication", None),
                ("notification_method_latency", None),
                ("notification_center_open", None),
                ("quick_settings_open", None),
                ("profile_switch", None),
                ("dnd_switch", None),
                ("output_reconciliation", "add"),
                ("output_reconciliation", "remove"),
                ("idle_wakeups", None),
                ("common_action_acknowledgement", None),
            ],
        )
        self.assertEqual(
            port.accessibility_calls,
            [(surface, check) for surface in SURFACES for check in MATRIX_CHECKS],
        )
        self.assertEqual(len(port.accessibility_calls), 14 * 26)
        self.assertEqual(port.internationalization_calls, INTERNATIONALIZATION_CHECKS)
        self.assertEqual(port.cleanup_calls, 1)

    def test_rtl_long_string_and_live_atspi_are_independent_observations(self) -> None:
        document = collect_evidence(config(), FakeLivePort())
        for entry in document["accessibility"]["matrix"]:
            self.assertEqual(entry["checks"]["rtl_smoke"]["status"], "passed")
            self.assertEqual(
                entry["checks"]["long_string_layout"]["status"], "passed"
            )
        self.assertEqual(document["accessibility"]["live_atspi"]["status"], "passed")
        self.assertEqual(
            document["internationalization"]["rtl_fixture"]["status"], "passed"
        )
        self.assertEqual(
            document["internationalization"]["long_string_fixture"]["status"],
            "passed",
        )

    def test_binding_mismatch_fails_closed_and_still_cleans(self) -> None:
        port = FakeLivePort()
        port.binding = ObservationBinding("c" * 40, ENVIRONMENT_ID)
        with self.assertRaisesRegex(EvidenceError, "^live_observation_binding_mismatch$"):
            collect_evidence(config(), port)
        self.assertEqual(port.cleanup_calls, 1)

    def test_cleanup_failure_overrides_partial_collection(self) -> None:
        port = FakeLivePort()
        port.results["performance.session_ready"] = {"unexpected": True}
        port.cleanup_result = {"clean": False}
        with self.assertRaisesRegex(EvidenceError, "^live_cleanup_failed$"):
            collect_evidence(config(), port)

    def test_skip_requires_exact_reviewed_policy_entry(self) -> None:
        key = "accessibility.group_popup.state_checked"
        skip = {
            "status": "skipped",
            "reviewed": True,
            "reason_code": "not_applicable",
            "reason": "Group popup does not expose a checked state",
        }
        port = FakeLivePort()
        port.results[key] = skip
        with self.assertRaisesRegex(EvidenceError, "^live_skip_not_reviewed$"):
            collect_evidence(config(), port)

        reviewed_port = FakeLivePort()
        reviewed_port.results[key] = skip
        document = collect_evidence(config({key: "not_applicable"}), reviewed_port)
        self.assertEqual(
            document["accessibility"]["matrix"][4]["checks"]["state_checked"],
            skip,
        )

    def test_reviewed_skip_cannot_silently_turn_into_a_pass(self) -> None:
        key = "accessibility.group_popup.state_checked"
        with self.assertRaisesRegex(EvidenceError, "^reviewed_skip_was_observed$"):
            collect_evidence(config({key: "not_applicable"}), FakeLivePort())

    def test_reviewed_skip_policy_rejects_unknown_probe_key(self) -> None:
        with self.assertRaisesRegex(EvidenceError, "^reviewed_skip_policy_invalid$"):
            config({"accessibility.unknown.state_checked": "not_applicable"})

    def test_qt_floor_and_private_content_are_rejected(self) -> None:
        old_qt = FakeLivePort()
        old_qt.results["environment"] = {
            **old_qt.environment().result,
            "qt_version": "6.10.9",
        }
        with self.assertRaisesRegex(EvidenceError, "^live_qt_floor_not_met$"):
            collect_evidence(config(), old_qt)

        private = FakeLivePort()
        private.results["accessibility.panel.accessible_description"] = passed(
            "Observed through unix: private endpoint"
        )
        with self.assertRaisesRegex(EvidenceError, "evidence_private_data"):
            collect_evidence(config(), private)

    def test_atomic_writer_creates_new_private_file_and_round_trips(self) -> None:
        document = collect_evidence(config(), FakeLivePort())
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "wp15.json"
            write_evidence(output, document)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), document)
            self.assertEqual(list(output.parent.glob(".wp15.json.*.tmp")), [])
            with self.assertRaisesRegex(EvidenceError, "^live_output_path_invalid$"):
                write_evidence(output, document)


class CommandAdapterTests(unittest.TestCase):
    def test_exact_command_protocol_returns_bound_result_without_shell(self) -> None:
        binding = {
            "source_revision": REVISION,
            "reference_environment_id": ENVIRONMENT_ID,
            "method_id": LIVE_METHOD_ID,
        }
        payload = json.dumps({"binding": binding, "result": passed()}).encode("utf-8")
        commands = ObserverCommands(*([Path("/bin/true")] * 5))
        with mock.patch.object(
            subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, payload, b""),
        ) as run:
            result = CommandLiveEvidencePort(commands).accessibility(
                "panel", "accessible_name"
            )
        self.assertEqual(result.binding, ObservationBinding(REVISION, ENVIRONMENT_ID))
        self.assertEqual(result.result, passed())
        self.assertEqual(
            run.call_args.args[0],
            ["/bin/true", "--surface", "panel", "--check", "accessible_name"],
        )
        self.assertFalse(run.call_args.kwargs.get("shell", False))

    def test_unified_observer_selects_role_before_probe_arguments(self) -> None:
        binding = {
            "source_revision": REVISION,
            "reference_environment_id": ENVIRONMENT_ID,
            "method_id": LIVE_METHOD_ID,
        }
        payload = json.dumps({"binding": binding, "result": passed()}).encode("utf-8")
        with mock.patch.object(
            subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, payload, b""),
        ) as run:
            CommandLiveEvidencePort(
                ObserverCommands.unified(Path("/bin/true"))
            ).internationalization("rtl_fixture")
        self.assertEqual(
            run.call_args.args[0],
            [
                "/bin/true",
                "--role",
                "internationalization",
                "--check",
                "rtl_fixture",
            ],
        )

    def test_unified_observer_accepts_only_dedicated_accessibility_override(self) -> None:
        options = SimpleNamespace(
            observer=Path("/observer"),
            environment_observer=None,
            performance_observer=None,
            accessibility_observer=Path("/accessibility"),
            internationalization_observer=None,
            cleanup_observer=None,
        )
        commands = observer_commands(options)
        self.assertEqual(commands.accessibility, Path("/accessibility"))
        self.assertEqual(commands.environment.executable, Path("/observer"))
        self.assertEqual(
            commands.internationalization.prefix_arguments,
            ("--role", "internationalization"),
        )

        for field in (
            "environment_observer",
            "performance_observer",
            "internationalization_observer",
            "cleanup_observer",
        ):
            with self.subTest(field=field):
                conflicting = SimpleNamespace(**vars(options))
                setattr(conflicting, field, Path("/conflict"))
                with self.assertRaisesRegex(Exception, "^observer_mode_conflict$"):
                    observer_commands(conflicting)

    def test_stderr_duplicate_json_and_oversized_output_fail_closed(self) -> None:
        commands = ObserverCommands(*([Path("/bin/true")] * 5))
        cases = (
            subprocess.CompletedProcess([], 0, b"{}", b"private warning"),
            subprocess.CompletedProcess(
                [], 0, b'{"binding":{},"binding":{},"result":{}}', b""
            ),
            subprocess.CompletedProcess([], 0, b"x" * 65_537, b""),
        )
        for completed in cases:
            with self.subTest(size=len(completed.stdout)):
                with (
                    mock.patch.object(subprocess, "run", return_value=completed),
                    self.assertRaises(EvidenceError),
                ):
                    CommandLiveEvidencePort(commands).live_atspi()

    def test_cli_environment_binds_identity_and_drops_unrelated_secrets(self) -> None:
        options = SimpleNamespace(
            source_revision=REVISION,
            environment_id=ENVIRONMENT_ID,
            pd1_reference_revision=PD1_REVISION,
        )
        with mock.patch.dict(
            "os.environ",
            {
                "DISPLAY": ":140",
                "DBUS_SESSION_BUS_ADDRESS": "private-runtime-value",
                "PRISMDRAKE_PD2_RUNTIME_PERFORMANCE_EVIDENCE": "/evidence/runtime.json",
                "PRISMDRAKE_PD2_ASSET_PERFORMANCE_RUNNER": "/runner/assets",
                "PRISMDRAKE_PD2_IDLE_SERIES": "/evidence/idle.json",
                "UNRELATED_SECRET": "must-not-propagate",
            },
            clear=True,
        ):
            environment = build_observer_environment(options)
        self.assertEqual(environment["PRISMDRAKE_PD2_SOURCE_REVISION"], REVISION)
        self.assertEqual(
            environment["PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID"], ENVIRONMENT_ID
        )
        self.assertEqual(environment["PRISMDRAKE_PD2_METHOD_ID"], LIVE_METHOD_ID)
        self.assertEqual(
            environment["PRISMDRAKE_PD2_PD1_REFERENCE_REVISION"], PD1_REVISION
        )
        self.assertEqual(
            environment["PRISMDRAKE_PD2_RUNTIME_PERFORMANCE_EVIDENCE"],
            "/evidence/runtime.json",
        )
        self.assertEqual(
            environment["PRISMDRAKE_PD2_ASSET_PERFORMANCE_RUNNER"],
            "/runner/assets",
        )
        self.assertEqual(
            environment["PRISMDRAKE_PD2_IDLE_SERIES"], "/evidence/idle.json"
        )
        self.assertNotIn("UNRELATED_SECRET", environment)

    def test_reviewed_skip_parser_accepts_only_closed_reason_codes(self) -> None:
        self.assertEqual(
            reviewed_skip("accessibility.panel.state_checked=not_applicable"),
            ("accessibility.panel.state_checked", "not_applicable"),
        )
        with self.assertRaisesRegex(Exception, "invalid_reviewed_skip"):
            reviewed_skip("accessibility.panel.state_checked=automatic_pass")


if __name__ == "__main__":
    unittest.main()
