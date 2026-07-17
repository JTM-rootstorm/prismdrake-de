#!/usr/bin/env python3
"""Inspect the live toolkit-spike AT-SPI tree in an isolated test session."""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Any

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


EXPECTED_NAMES = {
    "Open launcher",
    "Files task",
    "Terminal task",
    "Switch visual profile",
    "Cycle text scale",
    "Reduced motion",
    "Disable transparency",
}

EXPECTED_BUTTONS = EXPECTED_NAMES | {"Settings task"}


def children(node: Atspi.Accessible) -> list[Atspi.Accessible]:
    return [node.get_child_at_index(index) for index in range(node.get_child_count())]


def walk(node: Atspi.Accessible) -> list[Atspi.Accessible]:
    result = [node]
    for child in children(node):
        result.extend(walk(child))
    return result


def state_names(node: Atspi.Accessible) -> list[str]:
    return sorted(state.value_nick for state in node.get_state_set().get_states())


def action_names(node: Atspi.Accessible) -> list[str]:
    action = node.get_action_iface()
    if action is None:
        return []
    return [action.get_action_name(index) for index in range(action.get_n_actions())]


def snapshot(node: Atspi.Accessible) -> dict[str, Any]:
    return {
        "name": node.get_name() or "",
        "description": node.get_description() or "",
        "role": node.get_role_name(),
        "states": state_names(node),
        "actions": action_names(node),
        "interfaces": sorted(node.get_interfaces()),
        "children": [snapshot(child) for child in children(node)],
    }


def find_application(name: str, timeout: float) -> Atspi.Accessible | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        desktop = Atspi.get_desktop(0)
        for application in children(desktop):
            if application.get_name() == name:
                return application
        time.sleep(0.1)
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--application",
        default="prismdrake-pd1-toolkit-spike",
        help="exact AT-SPI application name",
    )
    parser.add_argument("--expect-focused", help="accessible name expected to be focused")
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    application = find_application(args.application, args.timeout)
    if application is None:
        print(f"AT-SPI application not found: {args.application}", file=sys.stderr)
        return 1

    nodes = walk(application)
    names = {node.get_name() or "" for node in nodes}
    missing = sorted(EXPECTED_NAMES - names)
    if missing:
        print(f"missing expected accessible names: {', '.join(missing)}", file=sys.stderr)
        return 1

    for name in sorted(EXPECTED_BUTTONS):
        matches = [node for node in nodes if node.get_name() == name]
        if len(matches) != 1:
            print(f"expected exactly one accessible control named {name!r}", file=sys.stderr)
            return 1
        node = matches[0]
        states = node.get_state_set()
        if node.get_role() != Atspi.Role.PUSH_BUTTON:
            print(f"control is not exposed as a button: {name}", file=sys.stderr)
            return 1
        if not node.get_description():
            print(f"control has no accessible description: {name}", file=sys.stderr)
            return 1
        if "Press" not in action_names(node):
            print(f"control has no Press action: {name}", file=sys.stderr)
            return 1
        for required_state in (Atspi.StateType.ENABLED, Atspi.StateType.FOCUSABLE):
            if not states.contains(required_state):
                print(f"control lacks required state {required_state.value_nick}: {name}", file=sys.stderr)
                return 1

    checked_tasks = [
        node.get_name()
        for node in nodes
        if (node.get_name() or "").endswith(" task")
        and node.get_state_set().contains(Atspi.StateType.CHECKED)
    ]
    if checked_tasks != ["Files task"]:
        print(f"unexpected checked task state: {checked_tasks}", file=sys.stderr)
        return 1

    if args.expect_focused:
        matches = [node for node in nodes if node.get_name() == args.expect_focused]
        if not matches:
            print(f"focused target not found: {args.expect_focused}", file=sys.stderr)
            return 1
        if not any(node.get_state_set().contains(Atspi.StateType.FOCUSED) for node in matches):
            print(f"target is not focused: {args.expect_focused}", file=sys.stderr)
            return 1

    print(json.dumps(snapshot(application), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
