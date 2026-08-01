#!/usr/bin/env python3
"""Hermetic adversarial tests for the ordered PD2 Tier C replay."""

from __future__ import annotations

import dataclasses
import sys
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_atspi import CategoricalResult as AtspiResult  # noqa: E402
from pd2_tier_c_workflow import (  # noqa: E402
    REVIEWED_SAMPLE_COUNT,
    STEP_CONTRACTS,
    TIER_C_ASSERTIONS,
    TIER_C_STEPS,
    XWD_SAMPLE_FINGERPRINT_KIND,
    ClientCleanupObservation,
    KeyboardObservation,
    MaterialObservation,
    PortResult,
    TierCCleanupResult,
    TierCReplayWorkflow,
    TierCStepContract,
    TierCStepResult,
    TierCWorkflowError,
    validate_tier_c_result,
)
from pd2_x11trace import X11TraceSummary  # noqa: E402


def clean_trace() -> X11TraceSummary:
    return X11TraceSummary(
        trace_complete=True,
        request_count=64,
        reply_boundary_count=32,
        forbidden_request_count=0,
        core_get_image_count=0,
        mit_shm_get_image_count=0,
        composite_name_window_pixmap_count=0,
        composite_redirect_window_count=0,
        composite_redirect_subwindows_count=0,
        backdrop_property_mutation_count=0,
        shell_side_capture_observed=False,
        blur_attempt_observed=False,
    )


class FakeTrace:
    def __init__(self) -> None:
        self.poll_calls = 0
        self.finish_calls: list[bool] = []
        self.finished = False
        self.summary = clean_trace()
        self.poll_failure_at: int | None = None
        self.finish_error: Exception | None = None

    def poll(self) -> None:
        self.poll_calls += 1
        if self.poll_calls == self.poll_failure_at:
            raise RuntimeError("private raw trace")

    def finish(self, *, clients_exited: bool) -> X11TraceSummary:
        self.finish_calls.append(clients_exited)
        self.finished = True
        if self.finish_error is not None:
            raise self.finish_error
        return self.summary


class FakePort:
    def __init__(self) -> None:
        self.operations: list[tuple[Any, ...]] = []
        self.prepare_result: Any = PortResult("tier_c_step_prepared")
        self.keyboard_result: Any = None
        self.atspi_results: list[Any] = []
        self.material_result: Any = None
        self.desktop_exclusion_result: Any = PortResult(
            "tier_c_desktop_accessibility_excluded"
        )
        self.complete_result: Any = PortResult("tier_c_step_complete")
        self.cleanup_result: Any = ClientCleanupObservation(
            "tier_c_clients_cleaned", True
        )
        self.raise_operation: str | None = None

    def _maybe_raise(self, operation: str) -> None:
        if self.raise_operation == operation:
            raise RuntimeError("private surface content")

    def prepare_step(self, contract: TierCStepContract) -> PortResult:
        self._maybe_raise("prepare")
        self.operations.append(("prepare", contract))
        return self.prepare_result

    def send_reviewed_keys(
        self, identifier: str, keys: tuple[str, ...]
    ) -> KeyboardObservation:
        self._maybe_raise("keyboard")
        self.operations.append(("keys", identifier, keys))
        if self.keyboard_result is not None:
            return self.keyboard_result
        return KeyboardObservation("tier_c_keyboard_complete", len(keys))

    def observe_atspi(self, selector: Any) -> AtspiResult:
        self._maybe_raise("atspi")
        self.operations.append(("atspi", selector))
        if self.atspi_results:
            return self.atspi_results.pop(0)
        return AtspiResult("atspi_control_ready")

    def capture_reviewed_material(
        self, identifier: str, sample_points: tuple[tuple[int, int], ...]
    ) -> MaterialObservation:
        self._maybe_raise("material")
        self.operations.append(("material", identifier, sample_points))
        if self.material_result is not None:
            return self.material_result
        digest = bytes([TIER_C_STEPS.index(identifier) + 1]) * 32
        return MaterialObservation(
            "tier_c_material_observed",
            len(sample_points),
            len(sample_points),
            XWD_SAMPLE_FINGERPRINT_KIND,
            digest,
            digest,
            bytes([255 - digest[0]]) * 32,
        )

    def complete_step(self, identifier: str) -> PortResult:
        self._maybe_raise("complete")
        self.operations.append(("complete", identifier))
        return self.complete_result

    def observe_desktop_exclusion(self) -> PortResult:
        self._maybe_raise("desktop_exclusion")
        self.operations.append(("desktop_exclusion",))
        return self.desktop_exclusion_result

    def cleanup_clients(self) -> ClientCleanupObservation:
        self._maybe_raise("cleanup")
        self.operations.append(("cleanup",))
        return self.cleanup_result


class TierCWorkflowTests(unittest.TestCase):
    def test_desktop_tab_moves_from_the_reviewed_launcher_control(self) -> None:
        contract = STEP_CONTRACTS["desktop"]

        self.assertEqual(contract.keys, ("TAB",))
        self.assertEqual(contract.focus_selector.name, "Open applications")
        self.assertTrue(contract.focus_selector.require_focused)

    def test_panel_tab_starts_from_the_reviewed_launcher_control(self) -> None:
        contract = STEP_CONTRACTS["panel"]

        self.assertEqual(contract.keys, ("TAB",))
        self.assertEqual(contract.surface_selector.name, "Open applications")
        self.assertEqual(contract.focus_selector.name, "Open applications")
        self.assertEqual(contract.readable_selector.name, "Open applications")
        self.assertTrue(contract.surface_selector.require_focused)
        self.assertTrue(contract.focus_selector.require_focused)
        self.assertTrue(contract.readable_selector.require_focused)

    def test_launcher_enter_activates_the_prepared_controlled_result(self) -> None:
        contract = STEP_CONTRACTS["launcher"]

        self.assertEqual(contract.keys, ("ENTER",))
        self.assertEqual(contract.focus_selector.name, "Search applications")
        self.assertTrue(contract.focus_selector.require_focused)

    def test_task_menu_starts_from_the_focused_authoritative_task(self) -> None:
        contract = STEP_CONTRACTS["task_actions"]

        self.assertEqual(contract.keys, ("MENU", "ESCAPE"))
        self.assertTrue(contract.surface_selector.require_focused)
        self.assertEqual(
            contract.surface_selector.name, "Prismdrake Controlled Application"
        )

    def test_notification_opens_center_from_the_next_panel_control(self) -> None:
        contract = STEP_CONTRACTS["notification"]

        self.assertEqual(contract.keys, ("TAB", "TAB", "ENTER", "SHIFT_TAB"))
        self.assertEqual(contract.focus_selector.name, "Open applications")
        self.assertTrue(contract.focus_selector.require_focused)

    def make_workflow(self) -> tuple[TierCReplayWorkflow, FakePort, FakeTrace]:
        port = FakePort()
        trace = FakeTrace()
        return TierCReplayWorkflow(port, trace), port, trace

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(TierCWorkflowError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)
        self.assertIsNone(raised.exception.__cause__)
        self.assertNotIn("private", str(raised.exception))

    def run_all(self, workflow: TierCReplayWorkflow) -> list[TierCStepResult]:
        return [workflow.run_step(identifier) for identifier in TIER_C_STEPS]

    def test_runs_exact_eight_step_contract_and_finalizes_clean_trace(self) -> None:
        workflow, port, trace = self.make_workflow()
        results = self.run_all(workflow)
        cleanup = workflow.cleanup()

        self.assertEqual(len(results), 8)
        self.assertTrue(all(result == TierCStepResult() for result in results))
        self.assertEqual(cleanup, TierCCleanupResult())
        self.assertEqual(trace.poll_calls, 15)
        self.assertEqual(trace.finish_calls, [True])
        self.assertTrue(trace.finished)
        self.assertEqual(
            [operation[0] for operation in port.operations],
            [
                item
                for step in TIER_C_STEPS
                for item in (
                    "prepare",
                    "atspi",
                    "atspi",
                    "atspi",
                    "material",
                    "keys",
                    *(("desktop_exclusion",) if step == "desktop" else ()),
                    "complete",
                )
            ]
            + ["cleanup"],
        )
        for index, identifier in enumerate(TIER_C_STEPS):
            base = index * 7 + (1 if index > 0 else 0)
            self.assertIs(port.operations[base][1], STEP_CONTRACTS[identifier])
            keyboard_offset = 5
            self.assertEqual(
                port.operations[base + keyboard_offset],
                ("keys", identifier, STEP_CONTRACTS[identifier].keys),
            )
            self.assertEqual(
                port.operations[base + 4],
                ("material", identifier, STEP_CONTRACTS[identifier].sample_points),
            )
        self.assertEqual(port.operations[6], ("desktop_exclusion",))
        for result in (*results, cleanup):
            validate_tier_c_result(result)
            self.assertEqual(set(result.as_dict()), {"category", "status"})

    def test_contracts_cover_every_assertion_with_exact_reviewed_boundaries(self) -> None:
        self.assertEqual(tuple(STEP_CONTRACTS), TIER_C_STEPS)
        self.assertEqual(len(TIER_C_ASSERTIONS), 5)
        for identifier, contract in STEP_CONTRACTS.items():
            with self.subTest(identifier=identifier):
                self.assertEqual(contract.identifier, identifier)
                self.assertTrue(contract.keys)
                self.assertEqual(len(contract.sample_points), REVIEWED_SAMPLE_COUNT)
                self.assertEqual(len(set(contract.sample_points)), REVIEWED_SAMPLE_COUNT)
                self.assertTrue(contract.focus_selector.require_focused)
                self.assertTrue(contract.surface_selector.name)
                self.assertTrue(contract.readable_selector.name)

    def test_rejects_wrong_duplicate_unknown_and_post_complete_order(self) -> None:
        workflow, _port, _trace = self.make_workflow()
        for identifier in ("panel", "unknown", 1):
            with self.subTest(identifier=identifier):
                self.assert_category(
                    "tier_c_order_invalid",
                    lambda identifier=identifier: workflow.run_step(identifier),
                )
        workflow.run_step("desktop")
        self.assert_category(
            "tier_c_order_invalid", lambda: workflow.run_step("desktop")
        )

        complete, _port, _trace = self.make_workflow()
        self.run_all(complete)
        self.assert_category(
            "tier_c_order_invalid", lambda: complete.run_step("logout")
        )
        complete.cleanup()
        self.assert_category("tier_c_cleanup_order_invalid", complete.cleanup)

    def test_rejects_invalid_prepare_and_complete_results(self) -> None:
        cases = (
            ("prepare_result", object()),
            ("prepare_result", PortResult("tier_c_step_complete")),
            ("prepare_result", PortResult("tier_c_step_prepared", "failed")),
            ("complete_result", object()),
            ("complete_result", PortResult("tier_c_step_prepared")),
        )
        for attribute, value in cases:
            with self.subTest(attribute=attribute, value=value):
                workflow, port, _trace = self.make_workflow()
                setattr(port, attribute, value)
                self.assert_category(
                    "tier_c_port_result_invalid",
                    lambda: workflow.run_step("desktop"),
                )

    def test_desktop_exclusion_is_exact_and_only_runs_for_desktop(self) -> None:
        workflow, port, _trace = self.make_workflow()
        workflow.run_step("desktop")
        self.assertEqual(
            [operation for operation in port.operations if operation[0] == "desktop_exclusion"],
            [("desktop_exclusion",)],
        )
        workflow.run_step("panel")
        self.assertEqual(
            [operation for operation in port.operations if operation[0] == "desktop_exclusion"],
            [("desktop_exclusion",)],
        )

        workflow, port, _trace = self.make_workflow()
        port.desktop_exclusion_result = PortResult("tier_c_step_complete")
        self.assert_category(
            "tier_c_port_result_invalid", lambda: workflow.run_step("desktop")
        )

    def test_rejects_incomplete_or_malformed_keyboard_observation(self) -> None:
        cases = (
            object(),
            KeyboardObservation("tier_c_keyboard_complete", 0),
            KeyboardObservation("tier_c_step_complete", 2),
            KeyboardObservation("tier_c_keyboard_complete", True),
            KeyboardObservation("tier_c_keyboard_complete", 2, "failed"),
        )
        for result in cases:
            with self.subTest(result=result):
                workflow, port, _trace = self.make_workflow()
                port.keyboard_result = result
                self.assert_category(
                    "tier_c_keyboard_invalid",
                    lambda: workflow.run_step("desktop"),
                )

    def test_requires_three_exact_shell_owned_atspi_observations_per_step(self) -> None:
        cases = (
            object(),
            AtspiResult("atspi_action_invoked"),
            AtspiResult("atspi_control_ready", "failed"),
        )
        for result in cases:
            with self.subTest(result=result):
                workflow, port, _trace = self.make_workflow()
                port.atspi_results = [result]
                self.assert_category(
                    "tier_c_atspi_invalid", lambda: workflow.run_step("desktop")
                )

        workflow, port, _trace = self.make_workflow()
        workflow.run_step("desktop")
        selectors = [operation[1] for operation in port.operations if operation[0] == "atspi"]
        contract = STEP_CONTRACTS["desktop"]
        self.assertEqual(
            selectors,
            [contract.surface_selector, contract.focus_selector, contract.readable_selector],
        )

    def test_rejects_unsettled_transparent_or_malformed_material_samples(self) -> None:
        stable = b"a" * 32
        reference = b"b" * 32
        cases = (
            object(),
            MaterialObservation(
                "tier_c_material_observed", 3, 3, XWD_SAMPLE_FINGERPRINT_KIND,
                stable, stable, reference
            ),
            MaterialObservation(
                "tier_c_material_observed", 4, 3, XWD_SAMPLE_FINGERPRINT_KIND,
                stable, stable, reference
            ),
            MaterialObservation(
                "tier_c_material_observed", 4, 4, "full_xwd_fingerprint",
                stable, stable, reference
            ),
            MaterialObservation(
                "tier_c_material_observed", 4, 4, XWD_SAMPLE_FINGERPRINT_KIND,
                b"a", b"a", reference
            ),
            MaterialObservation(
                "tier_c_material_observed", 4, 4, XWD_SAMPLE_FINGERPRINT_KIND,
                stable, b"c" * 32, reference
            ),
            MaterialObservation(
                "tier_c_material_observed", 4, 4, XWD_SAMPLE_FINGERPRINT_KIND,
                stable, stable, stable
            ),
            MaterialObservation(
                "tier_c_step_complete", 4, 4, XWD_SAMPLE_FINGERPRINT_KIND,
                stable, stable, reference
            ),
            MaterialObservation(
                "tier_c_material_observed", 4, 4, XWD_SAMPLE_FINGERPRINT_KIND,
                stable, stable, reference, "failed"
            ),
        )
        for result in cases:
            with self.subTest(result=result):
                workflow, port, _trace = self.make_workflow()
                port.material_result = result
                self.assert_category(
                    "tier_c_material_invalid",
                    lambda: workflow.run_step("desktop"),
                )

    def test_periodic_trace_failure_is_redacted_and_cleanup_remains_compatible(self) -> None:
        for poll_failure_at in (1, 2):
            with self.subTest(poll_failure_at=poll_failure_at):
                workflow, _port, trace = self.make_workflow()
                trace.poll_failure_at = poll_failure_at
                self.assert_category(
                    "tier_c_step_failed", lambda: workflow.run_step("desktop")
                )
                cleanup = workflow.cleanup()
                self.assertEqual(cleanup, TierCCleanupResult())
                self.assertEqual(trace.finish_calls, [True])
                self.assertTrue(trace.finished)

    def test_rejects_every_forbidden_or_incomplete_final_trace_shape(self) -> None:
        changes = (
            {"trace_complete": False},
            {"request_count": 0},
            {"request_count": True},
            {"reply_boundary_count": 0},
            {"forbidden_request_count": 1},
            {"core_get_image_count": 1},
            {"mit_shm_get_image_count": 1},
            {"composite_name_window_pixmap_count": 1},
            {"composite_redirect_window_count": 1},
            {"composite_redirect_subwindows_count": 1},
            {"backdrop_property_mutation_count": 1},
            {"shell_side_capture_observed": True},
            {"blur_attempt_observed": True},
        )
        for change in changes:
            with self.subTest(change=change):
                workflow, _port, trace = self.make_workflow()
                self.run_all(workflow)
                trace.summary = dataclasses.replace(clean_trace(), **change)
                self.assert_category("tier_c_trace_invalid", workflow.cleanup)
                self.assertTrue(trace.finished)

    def test_complete_replay_requires_clean_natural_trace_finalization(self) -> None:
        workflow, _port, trace = self.make_workflow()
        self.run_all(workflow)
        trace.finish_error = RuntimeError("private trace")
        self.assert_category("tier_c_trace_incomplete", workflow.cleanup)
        self.assertTrue(trace.finished)

        workflow, _port, trace = self.make_workflow()
        self.run_all(workflow)
        trace.finished = False

        def incomplete_finish(*, clients_exited: bool) -> X11TraceSummary:
            del clients_exited
            return clean_trace()

        trace.finish = incomplete_finish  # type: ignore[method-assign]
        self.assert_category("tier_c_cleanup_failed", workflow.cleanup)

    def test_cleanup_rejects_active_or_invalid_clients_and_attempts_trace_abort(self) -> None:
        cases = (
            object(),
            ClientCleanupObservation("tier_c_clients_cleaned", False),
            ClientCleanupObservation("tier_c_step_complete", True),
            ClientCleanupObservation("tier_c_clients_cleaned", True, "failed"),
        )
        for result in cases:
            with self.subTest(result=result):
                workflow, port, trace = self.make_workflow()
                port.cleanup_result = result
                self.assert_category("tier_c_cleanup_failed", workflow.cleanup)
                self.assertEqual(trace.finish_calls, [False])
                self.assertTrue(trace.finished)

        workflow, port, trace = self.make_workflow()
        port.raise_operation = "cleanup"
        self.assert_category("tier_c_cleanup_failed", workflow.cleanup)
        self.assertEqual(trace.finish_calls, [False])

    def test_incomplete_phase_cleanup_never_manufactures_assertion_evidence(self) -> None:
        workflow, port, trace = self.make_workflow()
        workflow.run_step("desktop")
        port.raise_operation = "prepare"
        self.assert_category(
            "tier_c_step_failed", lambda: workflow.run_step("panel")
        )
        result = workflow.cleanup()
        self.assertEqual(result, TierCCleanupResult())
        self.assertEqual(trace.finish_calls, [True])
        self.assertTrue(trace.finished)

    def test_unknown_port_exceptions_never_escape_private_values(self) -> None:
        for operation in ("prepare", "keyboard", "atspi", "material", "complete"):
            with self.subTest(operation=operation):
                workflow, port, _trace = self.make_workflow()
                port.raise_operation = operation
                self.assert_category(
                    "tier_c_step_failed", lambda: workflow.run_step("desktop")
                )

    def test_rejects_expanded_or_failed_public_results(self) -> None:
        for result in (
            object(),
            TierCStepResult("private"),
            TierCStepResult(status="failed"),
            TierCCleanupResult("private"),
            TierCCleanupResult(status="failed"),
        ):
            with self.subTest(result=result):
                self.assert_category(
                    "tier_c_port_result_invalid",
                    lambda result=result: validate_tier_c_result(result),
                )


if __name__ == "__main__":
    unittest.main()
