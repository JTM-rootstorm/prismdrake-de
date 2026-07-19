#!/usr/bin/env python3
"""Hermetic tests for installed settings workflow steps 25-28."""

from __future__ import annotations

import json
import struct
import sys
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_dbus import METHODS, ProbeResult  # noqa: E402
from pd2_live_settings_workflow import (  # noqa: E402
    InstalledSettingsWorkflow,
    SettingsWorkflowConfig,
    SettingsWorkflowError,
)
from pd2_xwd import fingerprint_xwd_samples  # noqa: E402


SURFACES = ("desktop_primary", "panel_primary", "quick_settings")
SAMPLE_POINTS = tuple((((0, 0),)) for _surface in SURFACES)
STEPS = (
    "open_quick_settings",
    "switch_lustre_to_forge",
    "verify_complete_settings_generation",
    "switch_back_preserving_accessibility",
)


def xwd(pixel: int, unsampled_pixel: int = 0x80) -> bytes:
    name = b"Private controlled surface"
    pixels = bytes(
        (pixel, 0x20, 0x30, 0, unsampled_pixel, 0x50, 0x60, 0)
    )
    fields = (
        100 + len(name) + 1,
        7,
        2,
        24,
        2,
        1,
        0,
        0,
        32,
        0,
        32,
        32,
        8,
        4,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        8,
        256,
        0,
        2,
        1,
        0,
        0,
        0,
    )
    return struct.pack(">25I", *fields) + name + b"\0" + pixels


def snapshot(
    generation: int,
    profile: str,
    *,
    preference_changes: dict[str, Any] | None = None,
    effective_changes: dict[str, Any] | None = None,
) -> dict[str, Any]:
    accessibility = {
        "focus_emphasis": "strong",
        "animation_scale": 0.0,
        "minimum_target_size_px": 48,
    }
    appearance = {
        "high_contrast": True,
        "reduced_motion": True,
        "transparency_enabled": False,
        "text_scale": 1.25,
    }
    if preference_changes:
        for key, value in preference_changes.items():
            target = appearance if key in appearance else accessibility
            target[key] = value
    effective = {
        "high_contrast": appearance["high_contrast"],
        "reduced_motion": appearance["reduced_motion"],
        "transparency_disabled": not appearance["transparency_enabled"],
        "text_scale": appearance["text_scale"],
        "animation_scale": accessibility["animation_scale"],
        "focus_width_px": 4.0 if profile == "lustre" else 5.0,
        "minimum_target_size_px": accessibility["minimum_target_size_px"],
    }
    if effective_changes:
        effective.update(effective_changes)
    return {
        "generation": generation,
        "profile_id": profile,
        "settings": {
            "appearance": appearance,
            "accessibility": accessibility,
        },
        "theme": {"effective_accessibility": effective},
    }


def result(operation: str, payload: tuple[Any, ...]) -> ProbeResult:
    return ProbeResult(operation, METHODS[operation].category, payload)


class FakeDbus:
    def __init__(self, replies: list[ProbeResult]) -> None:
        self.replies = list(replies)
        self.calls: list[tuple[str, tuple[Any, ...]]] = []

    def call(self, operation: str, *arguments: Any) -> ProbeResult:
        self.calls.append((operation, arguments))
        if not self.replies:
            raise RuntimeError("private unexpected D-Bus call")
        return self.replies.pop(0)


class FakeAccessibility:
    def __init__(self) -> None:
        self.result = True
        self.calls: list[tuple[str, str]] = []

    def observe_surface(self, name: str, role: str) -> bool:
        self.calls.append((name, role))
        return self.result


class FakeSurfaces:
    def __init__(self) -> None:
        self.visible: tuple[str, ...] = SURFACES
        self.epochs = [
            {name: xwd(0x10 + index) for index, name in enumerate(SURFACES)},
            {name: xwd(0x40 + index) for index, name in enumerate(SURFACES)},
        ]
        self.capture_index = 0
        self.calls: list[str] = []
        self.unsettled: set[tuple[int, str]] = set()

    def reviewed_surface_sequence(self) -> tuple[str, ...]:
        return self.visible

    def capture_settled_fingerprints(
        self, surface: str, points: tuple[tuple[int, int], ...]
    ) -> tuple[bytes, bytes]:
        self.calls.append(surface)
        epoch = min(self.capture_index // len(SURFACES), len(self.epochs) - 1)
        payload = self.epochs[epoch][surface]
        self.capture_index += 1
        if (epoch, surface) in self.unsettled:
            return (
                fingerprint_xwd_samples(payload, points),
                fingerprint_xwd_samples(xwd(0x70 + self.capture_index), points),
            )
        fingerprint = fingerprint_xwd_samples(payload, points)
        return fingerprint, fingerprint


def successful_replies() -> list[ProbeResult]:
    return [
        result("shell_open_quick_settings", ()),
        result("settings_get_current_snapshot", (10, snapshot(10, "lustre"))),
        result("settings_request_profile_change", (11,)),
        result("settings_get_current_snapshot", (11, snapshot(11, "forge"))),
        result("settings_request_profile_change", (12,)),
        result("settings_get_current_snapshot", (12, snapshot(12, "lustre"))),
    ]


class InstalledSettingsWorkflowTests(unittest.TestCase):
    def make_workflow(
        self, replies: list[ProbeResult] | None = None
    ) -> tuple[InstalledSettingsWorkflow, FakeDbus, FakeAccessibility, FakeSurfaces]:
        dbus = FakeDbus(replies or successful_replies())
        accessibility = FakeAccessibility()
        surfaces = FakeSurfaces()
        workflow = InstalledSettingsWorkflow(
            SettingsWorkflowConfig(SURFACES, SAMPLE_POINTS),
            dbus,
            accessibility,
            surfaces,
        )
        return workflow, dbus, accessibility, surfaces

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(SettingsWorkflowError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)
        self.assertIsNone(raised.exception.__cause__)
        self.assertIsNone(raised.exception.__context__)

    def run_through(self, workflow: InstalledSettingsWorkflow, count: int) -> None:
        for identifier in STEPS[:count]:
            workflow.run_step(identifier)

    def test_runs_exact_order_with_categorical_private_free_results(self) -> None:
        workflow, dbus, accessibility, surfaces = self.make_workflow()
        serialized: list[str] = []
        for identifier in STEPS:
            observed = workflow.run_step(identifier)
            encoded = json.dumps(observed.evidence(), sort_keys=True)
            serialized.append(encoded)
            self.assertEqual(observed.status, "passed")

        self.assertEqual(accessibility.calls, [("Quick settings", "pane")])
        self.assertEqual(surfaces.calls, list(SURFACES) * 2)
        self.assertEqual(
            dbus.calls,
            [
                ("shell_open_quick_settings", ("",)),
                ("settings_get_current_snapshot", (2,)),
                ("settings_request_profile_change", ("forge",)),
                ("settings_get_current_snapshot", (2,)),
                ("settings_request_profile_change", ("lustre",)),
                ("settings_get_current_snapshot", (2,)),
            ],
        )
        evidence = "".join(serialized)
        for forbidden in ("lustre", "forge", "desktop_primary", "fingerprint", "10", "11", "12"):
            self.assertNotIn(forbidden, evidence)

    def test_rejects_unknown_reordered_repeated_and_post_complete_steps(self) -> None:
        workflow, _dbus, _accessibility, _surfaces = self.make_workflow()
        self.assert_category(
            "settings_step_not_reviewed", lambda: workflow.run_step("private_step")
        )
        self.assert_category(
            "settings_step_order_invalid",
            lambda: workflow.run_step("switch_lustre_to_forge"),
        )
        workflow.run_step("open_quick_settings")
        self.assert_category(
            "settings_step_order_invalid",
            lambda: workflow.run_step("open_quick_settings"),
        )

        complete, _dbus, _accessibility, _surfaces = self.make_workflow()
        self.run_through(complete, 4)
        self.assert_category(
            "settings_step_order_invalid",
            lambda: complete.run_step("switch_back_preserving_accessibility"),
        )

    def test_requires_exact_accessible_quick_settings_surface(self) -> None:
        workflow, _dbus, accessibility, _surfaces = self.make_workflow()
        accessibility.result = False
        self.assert_category(
            "settings_quick_settings_not_accessible",
            lambda: workflow.run_step("open_quick_settings"),
        )

        malformed = successful_replies()
        malformed[0] = ProbeResult(
            "shell_open_quick_settings", "private_category", ()
        )
        workflow, _dbus, _accessibility, _surfaces = self.make_workflow(malformed)
        self.assert_category(
            "settings_quick_settings_probe_invalid",
            lambda: workflow.run_step("open_quick_settings"),
        )

    def test_requires_exact_controlled_surface_set_and_complete_transition(self) -> None:
        workflow, _dbus, _accessibility, surfaces = self.make_workflow()
        self.run_through(workflow, 1)
        surfaces.visible = tuple(reversed(SURFACES))
        self.assert_category(
            "settings_surface_set_changed",
            lambda: workflow.run_step("switch_lustre_to_forge"),
        )

        workflow, _dbus, _accessibility, surfaces = self.make_workflow()
        self.run_through(workflow, 2)
        surfaces.epochs[1]["panel_primary"] = surfaces.epochs[0]["panel_primary"]
        self.assert_category(
            "settings_surface_transition_incomplete",
            lambda: workflow.run_step("verify_complete_settings_generation"),
        )

        workflow, _dbus, _accessibility, surfaces = self.make_workflow()
        self.run_through(workflow, 1)
        surfaces.epochs[0]["quick_settings"] = b"private invalid XWD"
        self.assert_category(
            "settings_surface_capture_invalid",
            lambda: workflow.run_step("switch_lustre_to_forge"),
        )

    def test_rejects_dynamic_only_unsettled_post_switch_change(self) -> None:
        workflow, _dbus, _accessibility, surfaces = self.make_workflow()
        self.run_through(workflow, 2)
        surfaces.unsettled.add((1, "panel_primary"))
        self.assert_category(
            "settings_surface_not_settled",
            lambda: workflow.run_step("verify_complete_settings_generation"),
        )
        self.assert_category(
            "settings_step_order_invalid",
            lambda: workflow.run_step("verify_complete_settings_generation"),
        )

    def test_unsampled_dynamic_pixels_cannot_satisfy_profile_transition(self) -> None:
        workflow, _dbus, _accessibility, surfaces = self.make_workflow()
        self.run_through(workflow, 2)
        for index, surface in enumerate(SURFACES):
            surfaces.epochs[1][surface] = xwd(
                0x10 + index, unsampled_pixel=0xE0 + index
            )
        self.assert_category(
            "settings_surface_transition_incomplete",
            lambda: workflow.run_step("verify_complete_settings_generation"),
        )

    def test_requires_one_exact_lustre_to_forge_generation(self) -> None:
        cases: list[tuple[list[ProbeResult], str]] = []
        wrong_initial = successful_replies()
        wrong_initial[1] = result(
            "settings_get_current_snapshot", (10, snapshot(10, "forge"))
        )
        cases.append((wrong_initial, "settings_initial_profile_invalid"))
        skipped = successful_replies()
        skipped[2] = result("settings_request_profile_change", (12,))
        cases.append((skipped, "settings_generation_transition_invalid"))
        stale = successful_replies()
        stale[3] = result(
            "settings_get_current_snapshot", (10, snapshot(10, "forge"))
        )
        cases.append((stale, "settings_generation_transition_invalid"))
        wrong_profile = successful_replies()
        wrong_profile[3] = result(
            "settings_get_current_snapshot", (11, snapshot(11, "lustre"))
        )
        cases.append((wrong_profile, "settings_generation_transition_invalid"))

        for replies, category in cases:
            workflow, _dbus, _accessibility, _surfaces = self.make_workflow(replies)
            self.run_through(workflow, 1)
            self.assert_category(
                category,
                lambda workflow=workflow: workflow.run_step("switch_lustre_to_forge"),
            )

    def test_requires_exact_probe_identity_and_payload_shape(self) -> None:
        replies = successful_replies()
        replies[1] = ProbeResult(
            "settings_get_current_snapshot", "private_category", (10, snapshot(10, "lustre"))
        )
        workflow, _dbus, _accessibility, _surfaces = self.make_workflow(replies)
        self.run_through(workflow, 1)
        self.assert_category(
            "settings_snapshot_probe_invalid",
            lambda: workflow.run_step("switch_lustre_to_forge"),
        )

        replies = successful_replies()
        replies[2] = result("settings_request_profile_change", (True,))
        workflow, _dbus, _accessibility, _surfaces = self.make_workflow(replies)
        self.run_through(workflow, 1)
        self.assert_category(
            "settings_generation_invalid",
            lambda: workflow.run_step("switch_lustre_to_forge"),
        )

    def test_rejects_each_changed_accessibility_preference(self) -> None:
        changes = {
            "focus_emphasis": "standard",
            "animation_scale": 1.0,
            "minimum_target_size_px": 56,
            "high_contrast": False,
            "reduced_motion": False,
            "transparency_enabled": True,
            "text_scale": 1.5,
        }
        for field, value in changes.items():
            replies = successful_replies()
            changed = snapshot(11, "forge", preference_changes={field: value})
            replies[3] = result("settings_get_current_snapshot", (11, changed))
            workflow, _dbus, _accessibility, _surfaces = self.make_workflow(replies)
            self.run_through(workflow, 1)
            self.assert_category(
                "settings_accessibility_preferences_changed",
                lambda workflow=workflow: workflow.run_step("switch_lustre_to_forge"),
            )

    def test_switch_back_requires_next_generation_preferences_and_effective_state(self) -> None:
        cases: list[list[ProbeResult]] = []
        stale_generation = successful_replies()
        stale_generation[4] = result("settings_request_profile_change", (11,))
        cases.append(stale_generation)
        wrong_profile = successful_replies()
        wrong_profile[5] = result(
            "settings_get_current_snapshot", (12, snapshot(12, "forge"))
        )
        cases.append(wrong_profile)
        changed_preference = successful_replies()
        changed_preference[5] = result(
            "settings_get_current_snapshot",
            (12, snapshot(12, "lustre", preference_changes={"text_scale": 1.5})),
        )
        cases.append(changed_preference)
        changed_effective = successful_replies()
        changed_effective[5] = result(
            "settings_get_current_snapshot",
            (12, snapshot(12, "lustre", effective_changes={"focus_width_px": 7.0})),
        )
        cases.append(changed_effective)

        for replies in cases:
            workflow, _dbus, _accessibility, _surfaces = self.make_workflow(replies)
            self.run_through(workflow, 3)
            self.assert_category(
                "settings_generation_transition_invalid"
                if replies is stale_generation or replies is wrong_profile
                else "settings_accessibility_preferences_changed",
                lambda workflow=workflow: workflow.run_step(
                    "switch_back_preserving_accessibility"
                ),
            )

    def test_rejects_effective_accessibility_mismatch_and_redacts_port_errors(self) -> None:
        replies = successful_replies()
        replies[1] = result(
            "settings_get_current_snapshot",
            (10, snapshot(10, "lustre", effective_changes={"text_scale": 2.0})),
        )
        workflow, _dbus, _accessibility, _surfaces = self.make_workflow(replies)
        self.run_through(workflow, 1)
        self.assert_category(
            "settings_accessibility_resolution_mismatch",
            lambda: workflow.run_step("switch_lustre_to_forge"),
        )

        workflow, dbus, _accessibility, _surfaces = self.make_workflow()
        dbus.replies.clear()
        self.assert_category(
            "settings_observer_failed", lambda: workflow.run_step("open_quick_settings")
        )

    def test_rejects_invalid_configuration_surface_names(self) -> None:
        for surfaces in (
            (),
            ("duplicate", "duplicate"),
            ("not-valid",),
            tuple(f"surface_{index}" for index in range(9)),
        ):
            with self.assertRaisesRegex(
                SettingsWorkflowError, "^settings_surface_set_invalid$"
            ):
                SettingsWorkflowConfig(
                    surfaces,
                    tuple((((0, 0),)) for _surface in surfaces),
                )

        invalid_samples = (
            (),
            (((0, 0),),),
            tuple(() for _surface in SURFACES),
            tuple((((0, 0), (0, 0))) for _surface in SURFACES),
            tuple((((8192, 0),)) for _surface in SURFACES),
        )
        for sample_points in invalid_samples:
            with self.assertRaisesRegex(
                SettingsWorkflowError, "^settings_surface_samples_invalid$"
            ):
                SettingsWorkflowConfig(SURFACES, sample_points)


if __name__ == "__main__":
    unittest.main()
