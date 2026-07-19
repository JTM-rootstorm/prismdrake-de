#!/usr/bin/env python3
"""Hermetic tests for the bounded PD2 live AT-SPI helpers."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_atspi import (  # noqa: E402
    APPLICATION_NAME,
    CategoricalResult,
    ControlSelector,
    LiveAtspiError,
    MAX_ACTIONS,
    MAX_CHILDREN,
    MAX_NAME_LENGTH,
    invoke_reviewed_action,
    observe_control_absence,
    observe_control_state,
    observe_showing_control,
    validate_categorical_result,
)


class FakeRole:
    CHECK_BOX = "check_box"
    CHECK_MENU_ITEM = "check_menu_item"
    DIALOG = "dialog"
    ENTRY = "entry"
    FILLER = "filler"
    LABEL = "label"
    MENU = "menu"
    MENU_ITEM = "menu_item"
    NOTIFICATION = "notification"
    PANEL = "panel"
    PUSH_BUTTON = "push_button"
    RADIO_MENU_ITEM = "radio_menu_item"
    TEXT = "text"
    TOGGLE_BUTTON = "toggle_button"


class FakeStateType:
    CHECKED = "checked"
    ENABLED = "enabled"
    FOCUSABLE = "focusable"
    FOCUSED = "focused"
    SHOWING = "showing"
    VISIBLE = "visible"


class FakeAtspi:
    Role = FakeRole
    StateType = FakeStateType

    def __init__(self, desktop: "FakeNode") -> None:
        self.desktop = desktop

    def get_desktop(self, index: int) -> "FakeNode":
        if index != 0:
            raise AssertionError("unexpected desktop index")
        return self.desktop


class FakeStateSet:
    def __init__(self, states: set[str]) -> None:
        self.states = states

    def contains(self, state: str) -> bool:
        return state in self.states


class FakeAction:
    def __init__(
        self,
        names: list[str] | None = None,
        *,
        accepted: bool = True,
        reported_count: int | None = None,
        disappears: bool = False,
    ) -> None:
        self.names = names or []
        self.accepted = accepted
        self.reported_count = reported_count
        self.disappears = disappears
        self.calls: list[int] = []

    def get_n_actions(self) -> int:
        return self.reported_count if self.reported_count is not None else len(self.names)

    def get_action_name(self, index: int) -> str:
        return self.names[index]

    def do_action(self, index: int) -> bool:
        self.calls.append(index)
        if self.disappears:
            raise RuntimeError("private remote failure")
        return self.accepted


class FakeNode:
    def __init__(
        self,
        name: str,
        role: str = FakeRole.PANEL,
        *,
        children: list["FakeNode"] | None = None,
        states: set[str] | None = None,
        action: FakeAction | None = None,
        reported_children: int | None = None,
        disappears: bool = False,
    ) -> None:
        self.name = name
        self.role = role
        self.children = children or []
        self.states = states or set()
        self.action = action
        self.reported_children = reported_children
        self.disappears = disappears

    def get_name(self) -> str:
        if self.disappears:
            raise RuntimeError("private node path")
        return self.name

    def get_role(self) -> str:
        return self.role

    def get_child_count(self) -> int:
        return (
            self.reported_children
            if self.reported_children is not None
            else len(self.children)
        )

    def get_child_at_index(self, index: int) -> "FakeNode":
        return self.children[index]

    def get_state_set(self) -> FakeStateSet:
        return FakeStateSet(self.states)

    def get_action_iface(self) -> FakeAction | None:
        return self.action


VISIBLE = {
    FakeStateType.ENABLED,
    FakeStateType.FOCUSABLE,
    FakeStateType.FOCUSED,
    FakeStateType.SHOWING,
    FakeStateType.VISIBLE,
}


def fixture(
    controls: list[FakeNode] | None = None,
    *,
    application_name: str = APPLICATION_NAME,
) -> tuple[FakeAtspi, FakeNode]:
    application = FakeNode(application_name, children=controls or [])
    return FakeAtspi(FakeNode("desktop", children=[application])), application


def exact_owner(expected: FakeNode):
    return lambda application: application is expected


class LiveAtspiHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.action = FakeAction(["Press"])
        self.control = FakeNode(
            "Open applications",
            FakeRole.PUSH_BUTTON,
            states=set(VISIBLE),
            action=self.action,
        )
        self.atspi, self.application = fixture([self.control])
        self.selector = ControlSelector(
            "Open applications", "push_button", "Press", require_focused=True
        )

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(LiveAtspiError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)

    def test_observes_exact_showing_control_with_exact_shell_owner(self) -> None:
        calls: list[FakeNode] = []

        def owner_probe(application: FakeNode) -> bool:
            calls.append(application)
            return application is self.application

        result = observe_showing_control(self.atspi, owner_probe, self.selector)

        self.assertEqual(result.as_dict(), {"category": "atspi_control_ready", "status": "passed"})
        self.assertEqual(calls, [self.application])
        validate_categorical_result(result)

    def test_invokes_only_the_exact_reviewed_action_after_rechecking_owner(self) -> None:
        calls: list[FakeNode] = []

        def owner_probe(application: FakeNode) -> bool:
            calls.append(application)
            return application is self.application

        result = invoke_reviewed_action(self.atspi, owner_probe, self.selector)

        self.assertEqual(result.category, "atspi_action_invoked")
        self.assertEqual(self.action.calls, [0])
        self.assertEqual(calls, [self.application, self.application])

    def test_observes_reviewed_checked_state_on_exact_control(self) -> None:
        control = FakeNode(
            "Do not disturb",
            FakeRole.CHECK_BOX,
            states={*VISIBLE, FakeStateType.CHECKED},
        )
        atspi, application = fixture([control])
        selector = ControlSelector("Do not disturb", "check_box")

        result = observe_control_state(
            atspi, exact_owner(application), selector, "CHECKED", True
        )

        self.assertEqual(result.category, "atspi_control_state_observed")
        self.assert_category(
            "atspi_control_state_mismatch",
            lambda: observe_control_state(
                atspi, exact_owner(application), selector, "CHECKED", False
            ),
        )
        self.assert_category(
            "atspi_state_not_reviewed",
            lambda: observe_control_state(
                atspi, exact_owner(application), selector, "FOCUSED", True
            ),
        )

    def test_proves_control_absence_without_treating_tree_failure_as_absence(self) -> None:
        selector = ControlSelector("Transient notifications", "notification")
        hidden = FakeNode(
            "Transient notifications",
            FakeRole.NOTIFICATION,
            states={FakeStateType.ENABLED},
        )
        atspi, application = fixture([hidden])

        result = observe_control_absence(
            atspi, exact_owner(application), selector
        )
        self.assertEqual(result.category, "atspi_control_absent")

        hidden.states = set(VISIBLE)
        self.assert_category(
            "atspi_control_present",
            lambda: observe_control_absence(
                atspi, exact_owner(application), selector
            ),
        )
        application.children.insert(0, FakeNode("private", disappears=True))
        self.assert_category(
            "atspi_node_unavailable",
            lambda: observe_control_absence(
                atspi, exact_owner(application), selector
            ),
        )

    def test_rejects_duplicate_applications_and_duplicate_showing_controls(self) -> None:
        duplicate_application = FakeNode(APPLICATION_NAME)
        duplicate_atspi = FakeAtspi(
            FakeNode("desktop", children=[self.application, duplicate_application])
        )
        self.assert_category(
            "atspi_application_identity",
            lambda: observe_showing_control(
                duplicate_atspi, exact_owner(self.application), self.selector
            ),
        )

        duplicate_control = FakeNode(
            "Open applications",
            FakeRole.PUSH_BUTTON,
            states=set(VISIBLE),
            action=FakeAction(["Press"]),
        )
        self.application.children.append(duplicate_control)
        self.assert_category(
            "atspi_control_identity",
            lambda: observe_showing_control(
                self.atspi, exact_owner(self.application), self.selector
            ),
        )

    def test_hidden_duplicate_does_not_ambiguate_the_showing_control(self) -> None:
        self.application.children.append(
            FakeNode(
                "Open applications",
                FakeRole.PUSH_BUTTON,
                states={FakeStateType.ENABLED},
                action=FakeAction(["Press"]),
            )
        )

        result = observe_showing_control(
            self.atspi, exact_owner(self.application), self.selector
        )

        self.assertEqual(result.category, "atspi_control_ready")

    def test_rejects_child_name_and_action_bounds(self) -> None:
        cases = (
            (
                "atspi_child_bound",
                FakeNode(APPLICATION_NAME, reported_children=MAX_CHILDREN + 1),
                self.selector,
            ),
            (
                "atspi_name_bound",
                FakeNode(
                    APPLICATION_NAME,
                    children=[FakeNode("x" * (MAX_NAME_LENGTH + 1))],
                ),
                self.selector,
            ),
            (
                "atspi_action_bound",
                FakeNode(
                    APPLICATION_NAME,
                    children=[
                        FakeNode(
                            "Open applications",
                            FakeRole.PUSH_BUTTON,
                            states=set(VISIBLE),
                            action=FakeAction(reported_count=MAX_ACTIONS + 1),
                        )
                    ],
                ),
                self.selector,
            ),
        )
        for category, application, selector in cases:
            with self.subTest(category=category):
                atspi = FakeAtspi(FakeNode("desktop", children=[application]))
                self.assert_category(
                    category,
                    lambda atspi=atspi, application=application, selector=selector: (
                        observe_showing_control(atspi, exact_owner(application), selector)
                    ),
                )

    def test_rejects_depth_and_total_node_bounds(self) -> None:
        depth_application = FakeNode(APPLICATION_NAME)
        parent = depth_application
        for index in range(33):
            child = FakeNode(f"branch {index}")
            parent.children = [child]
            parent = child
        depth_atspi = FakeAtspi(FakeNode("desktop", children=[depth_application]))
        self.assert_category(
            "atspi_depth_bound",
            lambda: observe_showing_control(
                depth_atspi, exact_owner(depth_application), self.selector
            ),
        )

        groups = [
            FakeNode(
                f"group {index}",
                children=[FakeNode(f"leaf {index} {leaf}") for leaf in range(3)],
            )
            for index in range(MAX_CHILDREN)
        ]
        node_application = FakeNode(APPLICATION_NAME, children=groups)
        node_atspi = FakeAtspi(FakeNode("desktop", children=[node_application]))
        self.assert_category(
            "atspi_node_bound",
            lambda: observe_showing_control(
                node_atspi, exact_owner(node_application), self.selector
            ),
        )

    def test_rejects_cycles_without_returning_a_partial_tree(self) -> None:
        branch = FakeNode("branch")
        self.application.children = [branch]
        branch.children = [self.application]

        self.assert_category(
            "atspi_tree_cycle",
            lambda: observe_showing_control(
                self.atspi, exact_owner(self.application), self.selector
            ),
        )

    def test_fails_closed_when_a_node_disappears(self) -> None:
        self.application.children.insert(0, FakeNode("private title", disappears=True))

        self.assert_category(
            "atspi_node_unavailable",
            lambda: observe_showing_control(
                self.atspi, exact_owner(self.application), self.selector
            ),
        )

    def test_rejects_wrong_owner_and_redacts_owner_probe_failures(self) -> None:
        self.assert_category(
            "atspi_owner_identity",
            lambda: observe_showing_control(self.atspi, lambda _application: False, self.selector),
        )

        def broken_probe(_application: FakeNode) -> bool:
            raise RuntimeError("pid=1234 /private/path")

        self.assert_category(
            "atspi_owner_probe_unavailable",
            lambda: observe_showing_control(self.atspi, broken_probe, self.selector),
        )

    def test_rejects_missing_rejected_and_disappearing_actions(self) -> None:
        for action, category in (
            (FakeAction([]), "atspi_action_missing"),
            (FakeAction(["Press"], accepted=False), "atspi_action_rejected"),
            (FakeAction(["Press"], disappears=True), "atspi_action_unavailable"),
        ):
            with self.subTest(category=category):
                self.control.action = action
                self.assert_category(
                    category,
                    lambda: invoke_reviewed_action(
                        self.atspi, exact_owner(self.application), self.selector
                    ),
                )

    def test_rejects_unreviewed_action_before_touching_the_accessible_tree(self) -> None:
        selector = ControlSelector(
            "Open applications", "push_button", "Delete everything"
        )

        self.assert_category(
            "atspi_action_not_reviewed",
            lambda: invoke_reviewed_action(
                self.atspi,
                lambda _application: self.fail("owner probe should not run"),
                selector,
            ),
        )
        self.assertEqual(self.action.calls, [])

    def test_rejects_incomplete_focus_and_visibility_states(self) -> None:
        for missing, category in (
            (FakeStateType.FOCUSED, "atspi_focus_state"),
            (FakeStateType.FOCUSABLE, "atspi_focus_state"),
            (FakeStateType.ENABLED, "atspi_control_state"),
            (FakeStateType.SHOWING, "atspi_control_identity"),
            (FakeStateType.VISIBLE, "atspi_control_identity"),
        ):
            with self.subTest(missing=missing):
                self.control.states = VISIBLE - {missing}
                self.assert_category(
                    category,
                    lambda: observe_showing_control(
                        self.atspi, exact_owner(self.application), self.selector
                    ),
                )

    def test_results_and_errors_are_categorical_and_redacted(self) -> None:
        result = CategoricalResult("atspi_control_ready")
        encoded = json.dumps(result.as_dict(), sort_keys=True)
        for forbidden in (
            "title",
            "pid",
            "xid",
            "path",
            "unix:",
            "display",
            "bus",
            APPLICATION_NAME,
        ):
            self.assertNotIn(forbidden, encoded.lower())
        validate_categorical_result(result)

        self.assert_category(
            "atspi_control_identity",
            lambda: observe_showing_control(
                self.atspi,
                exact_owner(self.application),
                ControlSelector("private window title", "push_button", "Press"),
            ),
        )


if __name__ == "__main__":
    unittest.main()
