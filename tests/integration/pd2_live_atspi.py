#!/usr/bin/env python3
"""Bounded, redacted AT-SPI helpers for the installed PD2 demonstration."""

from __future__ import annotations

import json
import sys
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any


APPLICATION_NAME = "prismdrake-shell"
MAX_ACTIONS = 16
MAX_ACTION_NAME_LENGTH = 64
MAX_CHILDREN = 256
MAX_DESCRIPTION_LENGTH = 512
MAX_DEPTH = 32
MAX_NAME_LENGTH = 256
MAX_NODES = 1024

REVIEWED_ACTIONS = frozenset({"Press"})
REVIEWED_STATES = frozenset({"CHECKED", "ENABLED"})
ROLE_ENUM_NAMES = {
    "check_box": ("CHECK_BOX",),
    "dialog": ("DIALOG",),
    "editable_text": ("ENTRY", "TEXT"),
    "label": ("LABEL",),
    "menu": ("MENU",),
    "menu_item": ("MENU_ITEM", "CHECK_MENU_ITEM", "RADIO_MENU_ITEM"),
    "notification": ("NOTIFICATION",),
    "panel": ("FILLER", "PANEL"),
    "pane": ("FILLER", "PANEL"),
    "push_button": ("PUSH_BUTTON",),
    "radio_button": ("RADIO_BUTTON",),
    "toggle_button": ("TOGGLE_BUTTON",),
}
RESULT_CATEGORIES = frozenset(
    {
        "atspi_action_invoked",
        "atspi_application_ready",
        "atspi_control_absent",
        "atspi_control_reference_ready",
        "atspi_control_ready",
        "atspi_control_state_observed",
        "atspi_stale_control_rejected",
    }
)
APPLICATION_ERROR_CATEGORIES = frozenset(
    {
        "atspi_application_identity",
        "atspi_application_missing",
        "atspi_child_bound",
        "atspi_name_bound",
        "atspi_node_unavailable",
        "atspi_owner_identity",
        "atspi_owner_probe_unavailable",
    }
)
FAILURE_CATEGORIES = frozenset(
    {
        "atspi_action_bound",
        "atspi_action_identity",
        "atspi_action_missing",
        "atspi_action_name_bound",
        "atspi_action_not_reviewed",
        "atspi_action_rejected",
        "atspi_action_unavailable",
        "atspi_application_identity",
        "atspi_application_missing",
        "atspi_child_bound",
        "atspi_control_identity",
        "atspi_control_present",
        "atspi_control_state",
        "atspi_control_state_mismatch",
        "atspi_depth_bound",
        "atspi_description_bound",
        "atspi_description_missing",
        "atspi_focus_state",
        "atspi_name_bound",
        "atspi_node_bound",
        "atspi_node_unavailable",
        "atspi_owner_identity",
        "atspi_owner_probe_unavailable",
        "atspi_reference_invalid",
        "atspi_reference_bound",
        "atspi_reference_state",
        "atspi_result_invalid",
        "atspi_role_unavailable",
        "atspi_selector_invalid",
        "atspi_stale_control_live",
        "atspi_state_not_reviewed",
        "atspi_state_unavailable",
        "atspi_tree_cycle",
    }
)
WORKER_CATEGORIES = frozenset(
    {
        "atspi_worker_initialization_failed",
        "atspi_worker_internal_failure",
        "atspi_worker_protocol_invalid",
        "atspi_worker_ready",
    }
)
ALL_CATEGORIES = RESULT_CATEGORIES | FAILURE_CATEGORIES | WORKER_CATEGORIES
MAX_WORKER_MESSAGE_BYTES = 4096
MAX_WORKER_REFERENCES = 64


class LiveAtspiError(RuntimeError):
    """A fail-closed AT-SPI operation failed with a redacted category."""


@dataclass(frozen=True)
class ControlSelector:
    """An exact reviewed accessible-control identity."""

    name: str
    role: str
    action: str | None = None
    require_focused: bool = False
    require_description: bool = False


@dataclass(frozen=True)
class CategoricalResult:
    """A serialization-safe result that contains no live runtime identity."""

    category: str
    status: str = "passed"

    def as_dict(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


@dataclass(frozen=True, repr=False)
class ControlReference:
    """One live AT-SPI node retained only across a bounded lifecycle mutation."""

    application: Any
    control: Any
    selector: ControlSelector


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


def _node_description(node: Any) -> str:
    return _bounded_text(
        _node_value(node.get_description),
        MAX_DESCRIPTION_LENGTH,
        "atspi_description_bound",
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
        or not isinstance(selector.require_description, bool)
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
    return names


def _action_names(node: Any) -> list[str]:
    interface = _node_value(node.get_action_iface)
    return [] if interface is None else _interface_action_names(interface)


def _select_control(
    atspi: Any,
    application: Any,
    selector: ControlSelector,
    *,
    require_enabled: bool = True,
) -> Any:
    matches = _matching_visible_controls(atspi, application, selector)
    if selector.require_focused:
        focused = [
            node
            for node in matches
            if _has_state(atspi, node, "FOCUSABLE")
            and _has_state(atspi, node, "FOCUSED")
        ]
        if len(matches) == 1 and not focused:
            _fail("atspi_focus_state")
        matches = focused
    if len(matches) != 1:
        _fail("atspi_control_identity")

    control = matches[0]
    if require_enabled and not _has_state(atspi, control, "ENABLED"):
        _fail("atspi_control_state")
    if selector.require_description and not _node_description(control):
        _fail("atspi_description_missing")
    if selector.action is not None and selector.action not in _action_names(control):
        _fail("atspi_action_missing")
    return control


def _matching_visible_controls(
    atspi: Any, application: Any, selector: ControlSelector
) -> list[Any]:
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
    return matches


def _resolve_control(
    atspi: Any,
    owner_probe: OwnerProbe,
    selector: ControlSelector,
    require_action: bool,
    *,
    require_enabled: bool = True,
) -> tuple[Any, Any]:
    _validate_selector(selector, require_action)
    application = _find_application(atspi)
    _verify_owner(application, owner_probe)
    return application, _select_control(
        atspi,
        application,
        selector,
        require_enabled=require_enabled,
    )


def observe_showing_control(
    atspi: Any, owner_probe: OwnerProbe, selector: ControlSelector
) -> CategoricalResult:
    """Prove one exact visible shell control without returning live node data."""

    _resolve_control(atspi, owner_probe, selector, require_action=False)
    return CategoricalResult("atspi_control_ready")


def observe_application_owner(
    atspi: Any, owner_probe: OwnerProbe
) -> CategoricalResult:
    """Prove one exact shell application without assuming control cardinality."""

    application = _find_application(atspi)
    _verify_owner(application, owner_probe)
    return CategoricalResult("atspi_application_ready")


def capture_control_reference(
    atspi: Any, owner_probe: OwnerProbe, selector: ControlSelector
) -> ControlReference:
    """Retain one exact focused control for a subsequent lifecycle check."""

    application, control = _resolve_control(
        atspi, owner_probe, selector, require_action=False
    )
    return ControlReference(application, control, selector)


def observe_control_reference(
    atspi: Any, owner_probe: OwnerProbe, reference: ControlReference
) -> CategoricalResult:
    """Prove a retained node is still owned, visible, enabled, and focused."""

    if not isinstance(reference, ControlReference):
        _fail("atspi_reference_invalid")
    _validate_selector(reference.selector, require_action=False)
    _verify_owner(reference.application, owner_probe)
    control = reference.control
    if (
        not _has_state(atspi, control, "SHOWING")
        or not _has_state(atspi, control, "VISIBLE")
        or not _has_state(atspi, control, "ENABLED")
        or not _has_state(atspi, control, "FOCUSABLE")
        or not _has_state(atspi, control, "FOCUSED")
    ):
        _fail("atspi_reference_state")
    return CategoricalResult("atspi_control_reference_ready")


def reject_stale_control_reference(
    atspi: Any, reference: ControlReference
) -> CategoricalResult:
    """Prove a retained control became defunct or inaccessible after replacement."""

    if not isinstance(reference, ControlReference):
        _fail("atspi_reference_invalid")
    try:
        if _has_state(atspi, reference.control, "DEFUNCT"):
            return CategoricalResult("atspi_stale_control_rejected")
        _node_name(reference.control)
    except LiveAtspiError:
        return CategoricalResult("atspi_stale_control_rejected")
    _fail("atspi_stale_control_live")


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
        atspi,
        owner_probe,
        selector,
        require_action=False,
        require_enabled=state_name != "ENABLED",
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
    if not matches:
        _fail("atspi_action_identity")
    try:
        # Qt Quick controls can advertise their native action and an explicit
        # Accessible handler under the same reviewed name. The attached handler
        # is appended, so choose the last exact match deterministically.
        accepted = interface.do_action(matches[-1])
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


def _worker_write(category: str, status: str, **values: Any) -> None:
    """Write one bounded categorical response to the private parent pipe."""

    if category not in ALL_CATEGORIES or status not in {"passed", "failed"}:
        category = "atspi_worker_internal_failure"
        status = "failed"
        values = {}
    document = {"category": category, "status": status, **values}
    payload = json.dumps(document, separators=(",", ":"), sort_keys=True).encode(
        "ascii"
    )
    if len(payload) + 1 > MAX_WORKER_MESSAGE_BYTES:
        payload = b'{"category":"atspi_worker_internal_failure","status":"failed"}'
    sys.stdout.buffer.write(payload + b"\n")
    sys.stdout.buffer.flush()


def _worker_selector(value: Any) -> ControlSelector:
    if not isinstance(value, dict) or set(value) != {
        "action",
        "name",
        "require_description",
        "require_focused",
        "role",
    }:
        _fail("atspi_selector_invalid")
    selector = ControlSelector(
        name=value["name"],
        role=value["role"],
        action=value["action"],
        require_focused=value["require_focused"],
        require_description=value["require_description"],
    )
    _validate_selector(selector, require_action=False)
    return selector


def _worker_owner(shell_pid: int) -> OwnerProbe:
    if isinstance(shell_pid, bool) or not isinstance(shell_pid, int) or shell_pid <= 0:
        _fail("atspi_owner_identity")

    def matches(application: Any) -> bool:
        return application.get_process_id() == shell_pid

    return matches


def _worker_request(
    atspi: Any, request: Any, references: dict[int, ControlReference | None]
) -> tuple[CategoricalResult, dict[str, int]]:
    if not isinstance(request, dict) or not isinstance(
        operation := request.get("operation"), str
    ):
        _fail("atspi_worker_protocol_invalid")
    expected_keys = {
        "observe_application": {"operation", "shell_pid"},
        "observe": {"operation", "selector", "shell_pid"},
        "invoke": {"operation", "selector", "shell_pid"},
        "observe_state": {
            "expected",
            "operation",
            "selector",
            "shell_pid",
            "state_name",
        },
        "observe_absent": {"operation", "selector", "shell_pid"},
        "reject_stale_action": {"operation", "selector", "shell_pid"},
        "capture_reference": {"operation", "selector", "shell_pid"},
        "observe_reference": {"operation", "reference_id", "shell_pid"},
        "reject_stale_reference": {"operation", "reference_id"},
    }
    if operation not in expected_keys or set(request) != expected_keys[operation]:
        _fail("atspi_worker_protocol_invalid")

    owner = None
    if "shell_pid" in request:
        owner = _worker_owner(request["shell_pid"])
    selector = None
    if "selector" in request:
        selector = _worker_selector(request["selector"])

    if operation == "observe_application":
        return observe_application_owner(atspi, owner), {}
    if operation == "observe":
        return observe_showing_control(atspi, owner, selector), {}
    if operation == "invoke":
        return invoke_reviewed_action(atspi, owner, selector), {}
    if operation == "observe_state":
        return (
            observe_control_state(
                atspi,
                owner,
                selector,
                request["state_name"],
                request["expected"],
            ),
            {},
        )
    if operation == "observe_absent":
        return observe_control_absence(atspi, owner, selector), {}
    if operation == "reject_stale_action":
        _validate_selector(selector, True)
        application = _find_application(atspi)
        _verify_owner(application, owner)
        if not _matching_visible_controls(atspi, application, selector):
            return CategoricalResult("atspi_stale_control_rejected"), {}
        invoke_reviewed_action(atspi, owner, selector)
        _fail("atspi_stale_control_live")
    if operation == "capture_reference":
        if len(references) >= MAX_WORKER_REFERENCES:
            _fail("atspi_reference_bound")
        reference = capture_control_reference(atspi, owner, selector)
        reference_id = max(references, default=0) + 1
        references[reference_id] = reference
        return CategoricalResult("atspi_control_reference_ready"), {
            "reference_id": reference_id
        }

    reference_id = request["reference_id"]
    if (
        isinstance(reference_id, bool)
        or not isinstance(reference_id, int)
        or reference_id <= 0
        or (reference := references.get(reference_id)) is None
    ):
        _fail("atspi_reference_invalid")
    if operation == "observe_reference":
        result = observe_control_reference(atspi, owner, reference)
    else:
        result = reject_stale_control_reference(atspi, reference)
    references[reference_id] = None
    return result, {}


def run_worker() -> int:
    """Serve one AT-SPI bus epoch; process exit is the only rebind boundary."""

    try:
        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi

        if Atspi.init() != 0:
            raise RuntimeError("AT-SPI initialization failed")
    except Exception:
        _worker_write("atspi_worker_initialization_failed", "failed")
        return 1
    _worker_write("atspi_worker_ready", "passed")
    references: dict[int, ControlReference | None] = {}
    while payload := sys.stdin.buffer.readline(MAX_WORKER_MESSAGE_BYTES + 1):
        if len(payload) > MAX_WORKER_MESSAGE_BYTES or not payload.endswith(b"\n"):
            _worker_write("atspi_worker_protocol_invalid", "failed")
            return 1
        try:
            request = json.loads(payload)
        except (UnicodeError, json.JSONDecodeError):
            _worker_write("atspi_worker_protocol_invalid", "failed")
            return 1
        try:
            result, values = _worker_request(Atspi, request, references)
            validate_categorical_result(result)
        except LiveAtspiError as error:
            category = str(error)
            _worker_write(
                category
                if category in ALL_CATEGORIES
                else "atspi_worker_internal_failure",
                "failed",
            )
            continue
        except Exception:
            _worker_write("atspi_worker_internal_failure", "failed")
            continue
        _worker_write(result.category, "passed", **values)
    return 0


if __name__ == "__main__":
    raise SystemExit(run_worker())
