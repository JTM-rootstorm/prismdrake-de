#!/usr/bin/env python3
"""Bounded, redacted AT-SPI helpers for the installed PD2 demonstration."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any


APPLICATION_NAME = "prismdrake-shell"
MAX_ACTIONS = 16
MAX_ACTION_NAME_LENGTH = 64
MAX_CHILDREN = 256
MAX_DEPTH = 32
MAX_NAME_LENGTH = 256
MAX_NODES = 1024

REVIEWED_ACTIONS = frozenset({"Press"})
REVIEWED_STATES = frozenset({"CHECKED"})
ROLE_ENUM_NAMES = {
    "check_box": ("CHECK_BOX",),
    "dialog": ("DIALOG",),
    "editable_text": ("ENTRY", "TEXT"),
    "label": ("LABEL",),
    "menu": ("MENU",),
    "menu_item": ("MENU_ITEM", "CHECK_MENU_ITEM", "RADIO_MENU_ITEM"),
    "notification": ("NOTIFICATION",),
    "panel": ("FILLER", "PANEL"),
    "push_button": ("PUSH_BUTTON",),
    "toggle_button": ("TOGGLE_BUTTON",),
}
RESULT_CATEGORIES = frozenset(
    {
        "atspi_action_invoked",
        "atspi_control_absent",
        "atspi_control_ready",
        "atspi_control_state_observed",
    }
)


class LiveAtspiError(RuntimeError):
    """A fail-closed AT-SPI operation failed with a redacted category."""


@dataclass(frozen=True)
class ControlSelector:
    """An exact reviewed accessible-control identity."""

    name: str
    role: str
    action: str | None = None
    require_focused: bool = False


@dataclass(frozen=True)
class CategoricalResult:
    """A serialization-safe result that contains no live runtime identity."""

    category: str
    status: str = "passed"

    def as_dict(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


OwnerProbe = Callable[[Any], bool]


def _fail(category: str) -> None:
    raise LiveAtspiError(category)


def _node_value(operation: Callable[[], Any]) -> Any:
    try:
        return operation()
    except LiveAtspiError:
        raise
    except Exception:
        _fail("atspi_node_unavailable")


def _bounded_count(value: Any, maximum: int, category: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        _fail(category)
    return value


def _bounded_text(value: Any, maximum: int, category: str) -> str:
    if value is None:
        return ""
    if not isinstance(value, str) or len(value) > maximum:
        _fail(category)
    return value


def _children(node: Any) -> list[Any]:
    count = _bounded_count(
        _node_value(node.get_child_count), MAX_CHILDREN, "atspi_child_bound"
    )
    children: list[Any] = []
    for index in range(count):
        child = _node_value(lambda index=index: node.get_child_at_index(index))
        if child is None:
            _fail("atspi_node_unavailable")
        children.append(child)
    return children


def _node_name(node: Any) -> str:
    return _bounded_text(
        _node_value(node.get_name), MAX_NAME_LENGTH, "atspi_name_bound"
    )


def _walk(root: Any) -> list[Any]:
    found: list[Any] = []
    seen: set[int] = set()
    pending: list[tuple[Any, int]] = [(root, 0)]
    while pending:
        node, depth = pending.pop()
        if depth > MAX_DEPTH:
            _fail("atspi_depth_bound")
        identity = id(node)
        if identity in seen:
            _fail("atspi_tree_cycle")
        seen.add(identity)
        _node_name(node)
        found.append(node)
        if len(found) > MAX_NODES:
            _fail("atspi_node_bound")
        pending.extend((child, depth + 1) for child in reversed(_children(node)))
    return found


def _validate_selector(selector: ControlSelector, require_action: bool) -> None:
    if (
        not isinstance(selector, ControlSelector)
        or not isinstance(selector.name, str)
        or not selector.name
        or len(selector.name) > MAX_NAME_LENGTH
        or not isinstance(selector.role, str)
        or selector.role not in ROLE_ENUM_NAMES
        or not isinstance(selector.require_focused, bool)
    ):
        _fail("atspi_selector_invalid")
    if selector.action is not None and (
        not isinstance(selector.action, str) or selector.action not in REVIEWED_ACTIONS
    ):
        _fail("atspi_action_not_reviewed")
    if require_action and selector.action is None:
        _fail("atspi_action_not_reviewed")


def _find_application(atspi: Any) -> Any:
    desktop = _node_value(lambda: atspi.get_desktop(0))
    if desktop is None:
        _fail("atspi_application_missing")
    matches = [
        child for child in _children(desktop) if _node_name(child) == APPLICATION_NAME
    ]
    if not matches:
        _fail("atspi_application_missing")
    if len(matches) != 1:
        _fail("atspi_application_identity")
    return matches[0]


def _verify_owner(application: Any, owner_probe: OwnerProbe) -> None:
    try:
        matches = owner_probe(application)
    except Exception:
        _fail("atspi_owner_probe_unavailable")
    if matches is not True:
        _fail("atspi_owner_identity")


def _role_matches(atspi: Any, node: Any, role: str) -> bool:
    role_namespace = getattr(atspi, "Role", None)
    if role_namespace is None:
        _fail("atspi_role_unavailable")
    expected = tuple(
        value
        for enum_name in ROLE_ENUM_NAMES[role]
        if (value := getattr(role_namespace, enum_name, None)) is not None
    )
    if not expected:
        _fail("atspi_role_unavailable")
    return _node_value(node.get_role) in expected


def _has_state(atspi: Any, node: Any, state_name: str) -> bool:
    state_namespace = getattr(atspi, "StateType", None)
    state = getattr(state_namespace, state_name, None)
    if state is None:
        _fail("atspi_state_unavailable")
    state_set = _node_value(node.get_state_set)
    if state_set is None:
        _fail("atspi_state_unavailable")
    return bool(_node_value(lambda: state_set.contains(state)))


def _interface_action_names(interface: Any) -> list[str]:
    count = _bounded_count(
        _node_value(interface.get_n_actions), MAX_ACTIONS, "atspi_action_bound"
    )
    names = [
        _bounded_text(
            _node_value(lambda index=index: interface.get_action_name(index)),
            MAX_ACTION_NAME_LENGTH,
            "atspi_action_name_bound",
        )
        for index in range(count)
    ]
    if len(names) != len(set(names)):
        _fail("atspi_action_identity")
    return names


def _action_names(node: Any) -> list[str]:
    interface = _node_value(node.get_action_iface)
    return [] if interface is None else _interface_action_names(interface)


def _select_control(atspi: Any, application: Any, selector: ControlSelector) -> Any:
    matches: list[Any] = []
    for node in _walk(application):
        if _node_name(node) != selector.name or not _role_matches(
            atspi, node, selector.role
        ):
            continue
        if not _has_state(atspi, node, "SHOWING") or not _has_state(
            atspi, node, "VISIBLE"
        ):
            continue
        matches.append(node)
    if len(matches) != 1:
        _fail("atspi_control_identity")

    control = matches[0]
    if not _has_state(atspi, control, "ENABLED"):
        _fail("atspi_control_state")
    if selector.require_focused and (
        not _has_state(atspi, control, "FOCUSABLE")
        or not _has_state(atspi, control, "FOCUSED")
    ):
        _fail("atspi_focus_state")
    if selector.action is not None and selector.action not in _action_names(control):
        _fail("atspi_action_missing")
    return control


def _resolve_control(
    atspi: Any,
    owner_probe: OwnerProbe,
    selector: ControlSelector,
    require_action: bool,
) -> tuple[Any, Any]:
    _validate_selector(selector, require_action)
    application = _find_application(atspi)
    _verify_owner(application, owner_probe)
    return application, _select_control(atspi, application, selector)


def observe_showing_control(
    atspi: Any, owner_probe: OwnerProbe, selector: ControlSelector
) -> CategoricalResult:
    """Prove one exact visible shell control without returning live node data."""

    _resolve_control(atspi, owner_probe, selector, require_action=False)
    return CategoricalResult("atspi_control_ready")


def observe_control_absence(
    atspi: Any, owner_probe: OwnerProbe, selector: ControlSelector
) -> CategoricalResult:
    """Prove no exact visible control exists while keeping tree failures fatal."""

    _validate_selector(selector, require_action=False)
    application = _find_application(atspi)
    _verify_owner(application, owner_probe)
    for node in _walk(application):
        if _node_name(node) != selector.name or not _role_matches(
            atspi, node, selector.role
        ):
            continue
        if _has_state(atspi, node, "SHOWING") and _has_state(
            atspi, node, "VISIBLE"
        ):
            _fail("atspi_control_present")
    return CategoricalResult("atspi_control_absent")


def observe_control_state(
    atspi: Any,
    owner_probe: OwnerProbe,
    selector: ControlSelector,
    state_name: str,
    expected: bool,
) -> CategoricalResult:
    """Prove one reviewed state on an exact visible shell control."""

    if state_name not in REVIEWED_STATES or not isinstance(expected, bool):
        _fail("atspi_state_not_reviewed")
    _application, control = _resolve_control(
        atspi, owner_probe, selector, require_action=False
    )
    if _has_state(atspi, control, state_name) != expected:
        _fail("atspi_control_state_mismatch")
    return CategoricalResult("atspi_control_state_observed")


def invoke_reviewed_action(
    atspi: Any, owner_probe: OwnerProbe, selector: ControlSelector
) -> CategoricalResult:
    """Invoke one exact reviewed action after owner and focus-state checks."""

    application, control = _resolve_control(
        atspi, owner_probe, selector, require_action=True
    )
    _verify_owner(application, owner_probe)
    interface = _node_value(control.get_action_iface)
    if interface is None:
        _fail("atspi_action_missing")
    actions = _interface_action_names(interface)
    matches = [index for index, name in enumerate(actions) if name == selector.action]
    if len(matches) != 1:
        _fail("atspi_action_identity")
    try:
        accepted = interface.do_action(matches[0])
    except Exception:
        _fail("atspi_action_unavailable")
    if accepted is not True:
        _fail("atspi_action_rejected")
    return CategoricalResult("atspi_action_invoked")


def validate_categorical_result(result: CategoricalResult) -> None:
    """Reject accidental expansion of the public result into live evidence."""

    if result.status != "passed" or result.category not in RESULT_CATEGORIES:
        _fail("atspi_result_invalid")
    if set(result.as_dict()) != {"category", "status"}:
        _fail("atspi_result_invalid")
