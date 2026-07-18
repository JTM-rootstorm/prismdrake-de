#!/usr/bin/env python3
"""Capture bounded, redacted AT-SPI evidence from the production shell."""

from __future__ import annotations

import argparse
import json
import os
import selectors
import subprocess
import sys
import tempfile
import time
import warnings
from pathlib import Path
from typing import Any, Callable


APPLICATION_NAME = "prismdrake-shell"
LAUNCHER_TITLE = "Prismdrake Launcher"
FIXTURE_RESULT_NAME = "Prismdrake Evidence App"
DIAGNOSTICS_NAME = "Prismdrake Lustre, generation 1"
PHASE_IDS = (
    "panel_initial",
    "launcher_search_focus",
    "launcher_result_focus",
    "launcher_reverse_focus",
    "panel_return_focus",
    "panel_forward_focus",
    "panel_reverse_focus",
)
PHASE_FOCUS = (
    None,
    "Search applications",
    FIXTURE_RESULT_NAME,
    "Search applications",
    "Open applications",
    DIAGNOSTICS_NAME,
    "Open applications",
)
PHASE_CONTROL_NAMES = (
    ("Open applications", DIAGNOSTICS_NAME),
    ("Applications", "Search applications", FIXTURE_RESULT_NAME),
    ("Search applications", FIXTURE_RESULT_NAME),
    ("Search applications", FIXTURE_RESULT_NAME),
    ("Open applications", DIAGNOSTICS_NAME),
    ("Open applications", DIAGNOSTICS_NAME),
    ("Open applications", DIAGNOSTICS_NAME),
)
TOP_LEVEL_KEYS = {"application", "environment", "fixture", "phases", "schema_version"}
CONTROL_KEYS = {"actions", "description", "name", "role", "states"}
STATE_KEYS = {"enabled", "focusable", "focused"}
EXPECTED_DESCRIPTIONS = {
    "Applications": {
        "Application launcher",
        "Application launcher. Opaque fallback active",
    },
    DIAGNOSTICS_NAME: {
        "Theme diagnostics",
        "Theme diagnostics. Opaque fallback active",
    },
    "Open applications": {"Open the application launcher"},
    FIXTURE_RESULT_NAME: {"Evidence utility. Deterministic accessibility fixture."},
    "Search applications": {"Enter up to 256 characters"},
}


class EvidenceError(RuntimeError):
    """A bounded accessibility evidence operation failed."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def validate_evidence_document(document: Any) -> None:
    """Validate the strict semantic contract without third-party Python packages."""

    _require(isinstance(document, dict), "evidence root must be an object")
    _require(set(document) == TOP_LEVEL_KEYS, "evidence root fields do not match version 1")
    encoded = json.dumps(document, ensure_ascii=True, sort_keys=True)
    for forbidden in ("unix:", "DBUS_SESSION_BUS_ADDRESS", "AT_SPI_BUS_ADDRESS", '"pid"', '"path"'):
        _require(forbidden not in encoded, "evidence contains private runtime data")
    _require(document["schema_version"] == 1, "unsupported evidence schema version")
    _require(document["application"] == APPLICATION_NAME, "unexpected application identity")
    _require(
        document["environment"]
        == {
            "accessibility_bridge": "at-spi2",
            "display": "isolated-xvfb",
            "keyboard_injection": "xdotool",
            "session_bus": "isolated-dbus-run-session",
        },
        "unexpected evidence environment",
    )
    _require(
        document["fixture"] == {"desktop_entry_count": 1, "profile": "lustre"},
        "unexpected evidence fixture",
    )
    phases = document["phases"]
    _require(isinstance(phases, list) and len(phases) == len(PHASE_IDS), "wrong phase count")
    for index, phase in enumerate(phases):
        _require(isinstance(phase, dict), "phase must be an object")
        _require(set(phase) == {"controls", "focused_control", "id"}, "wrong phase fields")
        _require(phase["id"] == PHASE_IDS[index], "phase order is not deterministic")
        _require(phase["focused_control"] == PHASE_FOCUS[index], "unexpected focused control")
        controls = phase["controls"]
        _require(isinstance(controls, list), "phase controls must be an array")
        names = tuple(control.get("name") for control in controls if isinstance(control, dict))
        _require(names == PHASE_CONTROL_NAMES[index], "phase controls are missing or reordered")
        _require(len(set(names)) == len(names), "phase controls must have unique names")
        for control in controls:
            _require(set(control) == CONTROL_KEYS, "wrong control fields")
            _require(control["role"] in {"editable_text", "pane", "push_button"},
                     "unexpected accessible role")
            _require(isinstance(control["description"], str) and control["description"],
                     "accessible description is empty")
            _require(len(control["description"]) <= 160, "accessible description is oversized")
            _require(
                control["description"] in EXPECTED_DESCRIPTIONS[control["name"]],
                "accessible description is outside the redacted fixture allow-list",
            )
            _require(control["actions"] in ([], ["Press"]), "unexpected accessible action")
            _require(set(control["states"]) == STATE_KEYS, "wrong accessible-state fields")
            _require(
                all(isinstance(value, bool) for value in control["states"].values()),
                "accessible states must be booleans",
            )
            _require(control["states"]["enabled"], "required control is not enabled")
            if control["role"] != "pane":
                _require(control["states"]["focusable"], "required control is not focusable")
            if control["role"] == "push_button":
                _require(control["actions"] == ["Press"], "button has no Press action")


def validate_evidence_schema(document: Any) -> None:
    """Validate the emitted document against the tracked strict JSON schema."""

    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    schema = json.loads(
        Path(__file__).with_name("atspi-evidence.schema.json").read_text(encoding="utf-8")
    )
    _require(
        not validate_schema(document, schema, schema, "atspi_evidence"),
        "evidence does not satisfy the tracked JSON schema",
    )


def example_evidence_document() -> dict[str, Any]:
    """Return a valid synthetic document used only by contract tests."""

    descriptions = {
        "Applications": "Application launcher",
        DIAGNOSTICS_NAME: "Theme diagnostics",
        "Open applications": "Open the application launcher",
        FIXTURE_RESULT_NAME: "Evidence utility. Deterministic accessibility fixture.",
        "Search applications": "Enter up to 256 characters",
    }
    roles = {
        "Applications": "pane",
        "Search applications": "editable_text",
    }
    phases: list[dict[str, Any]] = []
    for phase_id, focused_name, names in zip(PHASE_IDS, PHASE_FOCUS, PHASE_CONTROL_NAMES):
        controls = []
        for name in names:
            role = roles.get(name, "push_button")
            controls.append(
                {
                    "actions": [] if role != "push_button" else ["Press"],
                    "description": descriptions[name],
                    "name": name,
                    "role": role,
                    "states": {
                        "enabled": True,
                        "focusable": role != "pane",
                        "focused": name == focused_name,
                    },
                }
            )
        phases.append({"controls": controls, "focused_control": focused_name, "id": phase_id})
    return {
        "application": APPLICATION_NAME,
        "environment": {
            "accessibility_bridge": "at-spi2",
            "display": "isolated-xvfb",
            "keyboard_injection": "xdotool",
            "session_bus": "isolated-dbus-run-session",
        },
        "fixture": {"desktop_entry_count": 1, "profile": "lustre"},
        "phases": phases,
        "schema_version": 1,
    }


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dbus-run-session", type=Path)
    parser.add_argument("--gdbus", type=Path)
    parser.add_argument("--openbox", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--settingsd", type=Path)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("--xdotool", type=Path)
    parser.add_argument("--xvfb", type=Path)
    parser.add_argument("--session-child", action="store_true", help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    required = ("gdbus", "openbox", "output", "settingsd", "shell", "xdotool")
    if not arguments.session_child:
        required += ("dbus_run_session", "xvfb")
    for name in required:
        value = getattr(arguments, name)
        if value is None:
            parser.error(f"--{name.replace('_', '-')} is required")
        if not value.is_absolute():
            parser.error(f"--{name.replace('_', '-')} must be an absolute path")
    return arguments


def _terminate(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def _read_display_number(server: subprocess.Popen[str]) -> str:
    _require(server.stdout is not None, "Xvfb display channel is unavailable")
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    _require(bool(selector.select(timeout=8)), "Xvfb did not publish a display number")
    display_number = server.stdout.readline().strip()
    _require(display_number.isascii() and display_number.isdecimal(),
             "Xvfb published an invalid display number")
    return display_number


def _run_parent(arguments: argparse.Namespace) -> None:
    for path in (
        arguments.dbus_run_session,
        arguments.gdbus,
        arguments.openbox,
        arguments.settingsd,
        arguments.shell,
        arguments.xdotool,
        arguments.xvfb,
    ):
        _require(
            path.is_file() and os.access(path, os.X_OK),
            "a required executable is unavailable",
        )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prismdrake-atspi-") as temporary:
        temporary_path = Path(temporary)
        runtime = temporary_path / "runtime"
        runtime.mkdir(mode=0o700)
        (runtime / "prismdrake").mkdir(mode=0o700)
        server = subprocess.Popen(
            [
                str(arguments.xvfb),
                "-displayfd",
                "1",
                "-screen",
                "0",
                "1280x720x24",
                "-noreset",
                "-nolisten",
                "tcp",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        session: subprocess.Popen[Any] | None = None
        try:
            display_number = _read_display_number(server)
            environment = os.environ.copy()
            environment.update(
                {
                    "DISPLAY": f":{display_number}",
                    "PRISMDRAKE_ATSPI_TEMPORARY": temporary,
                    "QT_ACCESSIBILITY": "1",
                    "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
                    "QT_QPA_PLATFORM": "xcb",
                    "QT_QUICK_BACKEND": "software",
                    "QT_QUICK_CONTROLS_STYLE": "Basic",
                    "XDG_RUNTIME_DIR": str(runtime),
                }
            )
            child_arguments = [
                str(arguments.dbus_run_session),
                "--",
                sys.executable,
                str(Path(__file__).resolve()),
                "--session-child",
                "--gdbus",
                str(arguments.gdbus),
                "--openbox",
                str(arguments.openbox),
                "--output",
                str(arguments.output),
                "--settingsd",
                str(arguments.settingsd),
                "--shell",
                str(arguments.shell),
                "--xdotool",
                str(arguments.xdotool),
            ]
            session = subprocess.Popen(child_arguments, env=environment)
            status = session.wait(timeout=35)
            _require(status == 0, f"isolated AT-SPI session exited with status {status}")
        except subprocess.TimeoutExpired as error:
            raise EvidenceError("isolated AT-SPI session exceeded 35 seconds") from error
        finally:
            if session is not None:
                _terminate(session)
            _terminate(server)
    document = json.loads(arguments.output.read_text(encoding="utf-8"))
    validate_evidence_schema(document)
    validate_evidence_document(document)


def _run_checked(command: list[str], timeout: float = 3) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def _wait_until(predicate: Callable[[], Any], description: str, timeout: float = 8) -> Any:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except Exception as error:  # AT-SPI nodes can disappear between bounded queries.
            last_error = error
        time.sleep(0.05)
    suffix = f": {last_error}" if last_error else ""
    raise EvidenceError(f"timed out waiting for {description}{suffix}")


def _children(node: Any) -> list[Any]:
    count = node.get_child_count()
    _require(0 <= count <= 256, "AT-SPI node exceeds the child bound")
    return [node.get_child_at_index(index) for index in range(count)]


def _walk(root: Any) -> list[Any]:
    result: list[Any] = []
    pending: list[tuple[Any, int]] = [(root, 0)]
    while pending:
        node, depth = pending.pop()
        _require(depth <= 32, "AT-SPI tree exceeds the depth bound")
        result.append(node)
        _require(len(result) <= 1024, "AT-SPI tree exceeds the node bound")
        pending.extend((child, depth + 1) for child in reversed(_children(node)))
    return result


def _find_application(atspi: Any) -> Any:
    desktop = atspi.get_desktop(0)
    matches = [child for child in _children(desktop) if child.get_name() == APPLICATION_NAME]
    if len(matches) == 1:
        return matches[0]
    return None


def _role_matches(atspi: Any, node: Any, expected_role: str) -> bool:
    role = node.get_role()
    return {
        "editable_text": role in (atspi.Role.ENTRY, atspi.Role.TEXT),
        "pane": role in (atspi.Role.FILLER, atspi.Role.PANEL),
        "push_button": role == atspi.Role.PUSH_BUTTON,
    }[expected_role]


def _find_unique(atspi: Any, nodes: list[Any], name: str, expected_role: str) -> Any:
    matches = [
        node
        for node in nodes
        if (node.get_name() or "") == name and _role_matches(atspi, node, expected_role)
    ]
    _require(
        len(matches) == 1,
        f"expected exactly one {expected_role} accessible control named {name!r}",
    )
    return matches[0]


def _action_names(node: Any) -> list[str]:
    interface = node.get_action_iface()
    if interface is None:
        return []
    count = interface.get_n_actions()
    _require(0 <= count <= 16, "AT-SPI action count exceeds the bound")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        return [interface.get_action_name(index) for index in range(count)]


def _has_state(node: Any, state: Any) -> bool:
    return bool(node.get_state_set().contains(state))


def _control(atspi: Any, node: Any, expected_role: str) -> dict[str, Any]:
    _require(_role_matches(atspi, node, expected_role),
             f"accessible control has an unexpected role: {node.get_name()!r}")
    description = (node.get_description() or "").strip()
    _require(0 < len(description) <= 160, "accessible description is empty or oversized")
    actions = _action_names(node)
    if expected_role == "push_button":
        _require("Press" in actions, f"button has no Press action: {node.get_name()!r}")
        actions = ["Press"]
    else:
        actions = []
    states = {
        "enabled": _has_state(node, atspi.StateType.ENABLED),
        "focusable": _has_state(node, atspi.StateType.FOCUSABLE),
        "focused": _has_state(node, atspi.StateType.FOCUSED),
    }
    return {
        "actions": actions,
        "description": description,
        "name": node.get_name() or "",
        "role": expected_role,
        "states": states,
    }


def _capture_phase(atspi: Any, phase_id: str, focused: str | None,
                   specs: tuple[tuple[str, str], ...]) -> dict[str, Any] | None:
    application = _find_application(atspi)
    if application is None:
        return None
    nodes = _walk(application)
    controls = [
        _control(atspi, _find_unique(atspi, nodes, name, role), role) for name, role in specs
    ]
    focused_controls = [
        control["name"]
        for control in controls
        if control["role"] != "pane" and control["states"]["focused"]
    ]
    return {
        "controls": controls,
        "focused_control": (
            None
            if not focused_controls
            else focused_controls[0]
            if len(focused_controls) == 1
            else "multiple-controls"
        ),
        "id": phase_id,
    }


def _invoke_press(atspi: Any, name: str) -> bool:
    application = _find_application(atspi)
    if application is None:
        return False
    node = _find_unique(atspi, _walk(application), name, "push_button")
    interface = node.get_action_iface()
    _require(interface is not None, f"control has no action interface: {name!r}")
    actions = _action_names(node)
    _require("Press" in actions, f"control has no Press action: {name!r}")
    return bool(interface.do_action(actions.index("Press")))


def _window_id(xdotool: Path, title: str) -> str | None:
    # AT-SPI focus is the visibility contract below. Avoid xdotool's
    # --onlyvisible filter because some WMs publish Qt tool windows after the
    # accessible tree becomes actionable.
    result = _run_checked([str(xdotool), "search", "--name", f"^{title}$"])
    if result.returncode != 0:
        return None
    identifiers = [
        line for line in result.stdout.splitlines() if line.isascii() and line.isdecimal()
    ]
    return identifiers[0] if len(identifiers) == 1 else None


def _window_geometry(xdotool: Path, window: str) -> tuple[int, int, int, int] | None:
    result = _run_checked([str(xdotool), "getwindowgeometry", "--shell", window])
    if result.returncode != 0:
        return None
    fields: dict[str, int] = {}
    try:
        for line in result.stdout.splitlines():
            key, separator, value = line.partition("=")
            if separator and key in {"X", "Y", "WIDTH", "HEIGHT"}:
                if key in fields:
                    return None
                fields[key] = int(value, 10)
    except ValueError:
        return None
    if set(fields) != {"X", "Y", "WIDTH", "HEIGHT"}:
        return None
    if fields["WIDTH"] <= 0 or fields["HEIGHT"] <= 0:
        return None
    return fields["X"], fields["Y"], fields["WIDTH"], fields["HEIGHT"]


def _window_process_matches(xdotool: Path, window: str, expected_process_id: int) -> bool:
    owner = _run_checked([str(xdotool), "getwindowpid", window])
    return owner.returncode == 0 and owner.stdout.strip() == str(expected_process_id)


def _probe_window_ids(result: subprocess.CompletedProcess[str]) -> tuple[bool, int, list[str]]:
    lines = result.stdout.splitlines()
    count = min(len(lines), 17)
    shape_valid = (
        result.returncode == 0
        and len(lines) <= 16
        and all(line.isascii() and line.isdecimal() for line in lines)
    )
    return shape_valid, count, lines if shape_valid else []


def _run_probe(command: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    try:
        return _run_checked(command, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return subprocess.CompletedProcess(command, 124, "", "")


def _probe_owned_windows(
    xdotool: Path, identifiers: list[str], expected_process_id: int
) -> tuple[int, bool]:
    owned = 0
    for identifier in identifiers[:4]:
        owner = _run_probe(
            [str(xdotool), "getwindowpid", identifier], timeout=0.5
        )
        if owner.returncode == 0 and owner.stdout.strip() == str(expected_process_id):
            owned += 1
    return owned, len(identifiers) <= 4


def _launcher_probe_summary(
    atspi: Any, xdotool: Path, node: Any, panel: str, expected_process_id: int
) -> str:
    """Return bounded categorical diagnostics without runtime identifiers."""

    try:
        accessible_owner_matches = node.get_process_id() == expected_process_id
        accessible_focused = _has_state(node, atspi.StateType.FOCUSED)
        component = node.get_component_iface()
    except Exception:  # The remote AT-SPI node can disappear while diagnosing a timeout.
        accessible_owner_matches = False
        accessible_focused = False
        component = None
    component_available = component is not None
    extents_positive = False
    if component_available:
        try:
            extents = component.get_extents(atspi.CoordType.SCREEN)
            extents_positive = extents.width > 0 and extents.height > 0
        except Exception:  # Keep diagnostics categorical if the remote node vanishes.
            extents_positive = False

    titled = _run_probe(
        [str(xdotool), "search", "--all", "--name", f"^{LAUNCHER_TITLE}$"],
        timeout=1,
    )
    titled_shape, titled_count, titled_ids = _probe_window_ids(titled)
    titled_non_panel = [identifier for identifier in titled_ids if identifier != panel]
    titled_owned, titled_owner_complete = _probe_owned_windows(
        xdotool, titled_non_panel, expected_process_id
    )

    shell_windows = _run_probe(
        [str(xdotool), "search", "--all", "--class", f"^{APPLICATION_NAME}$"],
        timeout=1,
    )
    shell_shape, shell_count, shell_ids = _probe_window_ids(shell_windows)
    shell_non_panel = [identifier for identifier in shell_ids if identifier != panel]
    shell_owned, shell_owner_complete = _probe_owned_windows(
        xdotool, shell_non_panel, expected_process_id
    )

    visible = _run_probe(
        [
            str(xdotool),
            "search",
            "--all",
            "--onlyvisible",
            "--class",
            f"^{APPLICATION_NAME}$",
        ],
        timeout=1,
    )
    visible_shape, visible_count, visible_ids = _probe_window_ids(visible)
    visible_non_panel = [identifier for identifier in visible_ids if identifier != panel]
    visible_owned, visible_owner_complete = _probe_owned_windows(
        xdotool, visible_non_panel, expected_process_id
    )

    focused = _run_probe([str(xdotool), "getwindowfocus"], timeout=1)
    focused_identifier = focused.stdout.strip()
    focused_shape = (
        focused.returncode == 0
        and focused_identifier.isascii()
        and focused_identifier.isdecimal()
    )
    focused_non_panel = focused_shape and focused_identifier != panel
    focused_owned = focused_non_panel and _window_process_matches(
        xdotool, focused_identifier, expected_process_id
    )
    focused_class_matches = False
    if focused_owned:
        window_class = _run_probe(
            [str(xdotool), "getwindowclassname", focused_identifier], timeout=0.5
        )
        focused_class_matches = (
            window_class.returncode == 0
            and window_class.stdout.strip() == APPLICATION_NAME
        )

    fields = (
        ("accessible_owner", accessible_owner_matches),
        ("accessible_focused", accessible_focused),
        ("component", component_available),
        ("extents_positive", extents_positive),
        ("title_query", titled.returncode == 0),
        ("title_shape", titled_shape),
        ("title_count", titled_count),
        ("title_non_panel_count", len(titled_non_panel)),
        ("title_owned_count", titled_owned),
        ("title_owner_complete", titled_owner_complete),
        ("class_query", shell_windows.returncode == 0),
        ("class_shape", shell_shape),
        ("class_count", shell_count),
        ("class_non_panel_count", len(shell_non_panel)),
        ("class_owned_count", shell_owned),
        ("class_owner_complete", shell_owner_complete),
        ("visible_query", visible.returncode == 0),
        ("visible_shape", visible_shape),
        ("visible_count", visible_count),
        ("visible_non_panel_count", len(visible_non_panel)),
        ("visible_owned_count", visible_owned),
        ("visible_owner_complete", visible_owner_complete),
        ("focus_shape", focused_shape),
        ("focus_non_panel", focused_non_panel),
        ("focus_owned", focused_owned),
        ("focus_class", focused_class_matches),
    )
    return "launcher_probe " + " ".join(
        f"{name}={int(value) if isinstance(value, bool) else value}" for name, value in fields
    )


def _focused_window_for_accessible(
    atspi: Any, xdotool: Path, node: Any, panel: str, expected_process_id: int
) -> str | None:
    if node.get_process_id() != expected_process_id:
        return None
    if not _has_state(node, atspi.StateType.FOCUSED):
        return None
    focused = _run_checked([str(xdotool), "getwindowfocus"])
    identifier = focused.stdout.strip()
    if (
        focused.returncode != 0
        or not identifier.isascii()
        or not identifier.isdecimal()
        or identifier == panel
    ):
        return None
    if not _window_process_matches(xdotool, identifier, expected_process_id):
        return None
    window_class = _run_checked([str(xdotool), "getwindowclassname", identifier])
    if window_class.returncode != 0 or window_class.stdout.strip() != APPLICATION_NAME:
        return None
    return identifier


def _titled_window_for_accessible(
    xdotool: Path, node: Any, panel: str, expected_process_id: int
) -> str | None:
    """Resolve Qt tool windows that Openbox publishes on every desktop."""

    if node.get_process_id() != expected_process_id:
        return None
    result = _run_checked(
        [str(xdotool), "search", "--all", "--name", f"^{LAUNCHER_TITLE}$"]
    )
    if result.returncode != 0:
        return None
    identifiers = result.stdout.splitlines()
    if (
        len(identifiers) != 1
        or not identifiers[0].isascii()
        or not identifiers[0].isdecimal()
        or identifiers[0] == panel
    ):
        return None
    identifier = identifiers[0]
    return (
        identifier
        if _window_process_matches(xdotool, identifier, expected_process_id)
        else None
    )


def _visible_window_for_accessible(
    xdotool: Path, node: Any, panel: str, expected_process_id: int
) -> str | None:
    if node.get_process_id() != expected_process_id:
        return None
    result = _run_checked(
        [
            str(xdotool),
            "search",
            "--all",
            "--onlyvisible",
            "--class",
            "^prismdrake-shell$",
        ]
    )
    if result.returncode != 0:
        return None
    lines = result.stdout.splitlines()
    if not lines or any(not line.isascii() or not line.isdecimal() for line in lines):
        return None
    if len(lines) > 16:
        raise EvidenceError("the visible shell X11 window count exceeds the bound")
    identifiers = [identifier for identifier in lines if identifier != panel]
    for identifier in identifiers:
        if not _window_process_matches(xdotool, identifier, expected_process_id):
            return None
    return identifiers[0] if len(identifiers) == 1 else None


def _embedded_panel_for_accessible(
    atspi: Any, xdotool: Path, node: Any, panel: str, expected_process_id: int
) -> str | None:
    """Accept Qt's embedded accessible surface only with an exact X11 inventory."""

    if node.get_process_id() != expected_process_id:
        return None
    if not _has_state(node, atspi.StateType.FOCUSED):
        return None
    component = node.get_component_iface()
    if component is None:
        return None
    extents = component.get_extents(atspi.CoordType.SCREEN)
    if extents.width <= 0 or extents.height <= 0:
        return None
    if not _window_process_matches(xdotool, panel, expected_process_id):
        return None

    titled = _run_checked(
        [str(xdotool), "search", "--all", "--name", f"^{LAUNCHER_TITLE}$"]
    )
    if titled.returncode != 1 or titled.stdout:
        return None
    focused = _run_checked([str(xdotool), "getwindowfocus"])
    if focused.returncode != 1 or focused.stdout:
        return None

    inventories = (
        [str(xdotool), "search", "--all", "--class", f"^{APPLICATION_NAME}$"],
        [
            str(xdotool),
            "search",
            "--all",
            "--onlyvisible",
            "--class",
            f"^{APPLICATION_NAME}$",
        ],
    )
    for command in inventories:
        result = _run_checked(command)
        if result.returncode != 0 or result.stdout.splitlines() != [panel]:
            return None
    return panel


def _window_for_accessible(
    atspi: Any, xdotool: Path, node: Any, panel: str, expected_process_id: int
) -> str | None:
    if node.get_process_id() != expected_process_id:
        return None
    component = node.get_component_iface()
    if component is None:
        return None
    extents = component.get_extents(atspi.CoordType.SCREEN)
    if extents.width <= 0 or extents.height <= 0:
        return None
    center_x = extents.x + extents.width // 2
    center_y = extents.y + extents.height // 2
    result = _run_checked(
        [str(xdotool), "search", "--all", "--class", "^prismdrake-shell$"]
    )
    if result.returncode != 0:
        return None
    identifiers = [
        line
        for line in result.stdout.splitlines()
        if line.isascii() and line.isdecimal() and line != panel
    ]
    if len(identifiers) > 16:
        raise EvidenceError("the shell X11 window count exceeds the bound")
    matches = []
    for identifier in identifiers:
        if not _window_process_matches(xdotool, identifier, expected_process_id):
            continue
        geometry = _window_geometry(xdotool, identifier)
        if geometry is None:
            continue
        x, y, width, height = geometry
        if x <= center_x < x + width and y <= center_y < y + height:
            matches.append(identifier)
    return matches[0] if len(matches) == 1 else None


def _grab_focus(node: Any) -> bool:
    component = node.get_component_iface()
    if component is None:
        return False
    return bool(component.grab_focus())


def _send_key(xdotool: Path, key: str, window: str) -> None:
    command = [str(xdotool), "key", "--window", window, key]
    result = _run_checked(command)
    _require(result.returncode == 0, f"keyboard injection failed for {key}")


def _phase(atspi: Any, phase_id: str, focused: str | None,
           specs: tuple[tuple[str, str], ...]) -> dict[str, Any]:
    observed: dict[str, Any] | None = None

    def capture_expected_focus() -> dict[str, Any] | None:
        nonlocal observed
        observed = _capture_phase(atspi, phase_id, focused, specs)
        if observed is None or observed["focused_control"] != focused:
            return None
        return observed

    try:
        return _wait_until(capture_expected_focus, phase_id)
    except EvidenceError as error:
        actual = observed["focused_control"] if observed is not None else None
        raise EvidenceError(
            f"{error}; expected focused control {focused!r}, observed {actual!r}"
        ) from error


def _run_session_child(arguments: argparse.Namespace) -> None:
    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi

    temporary_value = os.environ.get("PRISMDRAKE_ATSPI_TEMPORARY", "")
    temporary = Path(temporary_value)
    _require(temporary.is_absolute() and temporary.is_dir(), "private test root is unavailable")
    home = temporary / "home"
    config = temporary / "config"
    data = temporary / "data"
    data_dirs = temporary / "data-dirs"
    cache = temporary / "cache"
    state = temporary / "state"
    applications = data / "applications"
    for directory in (home, config, applications, data_dirs, cache, state):
        directory.mkdir(parents=True, exist_ok=True)
    (applications / "prismdrake-evidence.desktop").write_text(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Prismdrake Evidence App\n"
        "GenericName=Evidence utility\n"
        "Comment=Deterministic accessibility fixture\n"
        "Exec=/bin/true\n"
        "OnlyShowIn=Prismdrake;\n",
        encoding="utf-8",
    )
    os.environ.update(
        {
            "HOME": str(home),
            "LC_ALL": "C.UTF-8",
            "XDG_CACHE_HOME": str(cache),
            "XDG_CONFIG_HOME": str(config),
            "XDG_CURRENT_DESKTOP": "Prismdrake",
            "XDG_DATA_DIRS": str(data_dirs),
            "XDG_DATA_HOME": str(data),
            "XDG_STATE_HOME": str(state),
        }
    )
    activation = _run_checked(
        [
            str(arguments.gdbus),
            "call",
            "--session",
            "--dest",
            "org.a11y.Bus",
            "--object-path",
            "/org/a11y/bus",
            "--method",
            "org.a11y.Bus.GetAddress",
        ]
    )
    _require(activation.returncode == 0, "the isolated AT-SPI bus could not be activated")

    processes: list[subprocess.Popen[Any]] = []
    try:
        openbox = subprocess.Popen(
            [str(arguments.openbox), "--sm-disable"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        processes.append(openbox)
        settingsd = subprocess.Popen(
            [str(arguments.settingsd), "--foreground"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        processes.append(settingsd)

        def settings_ready() -> bool:
            if settingsd.poll() is not None:
                raise EvidenceError(
                    f"prismdrake-settingsd exited with status {settingsd.returncode}"
                )
            result = _run_checked(
                [
                    str(arguments.gdbus),
                    "call",
                    "--session",
                    "--dest",
                    "org.prismdrake.Settings1",
                    "--object-path",
                    "/org/prismdrake/Settings1",
                    "--method",
                    "org.prismdrake.Settings1.GetCurrentProfile",
                ],
                timeout=1,
            )
            return result.returncode == 0

        _wait_until(settings_ready, "the settings service")
        shell = subprocess.Popen(
            [str(arguments.shell)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        processes.append(shell)

        def panel_window() -> str | None:
            if shell.poll() is not None:
                raise EvidenceError(f"prismdrake-shell exited with status {shell.returncode}")
            return _window_id(arguments.xdotool, "Prismdrake Panel")

        panel = _wait_until(panel_window, "the Prismdrake panel window")
        Atspi.init()
        _wait_until(lambda: _find_application(Atspi), "the Prismdrake AT-SPI application")
        panel_specs = (
            ("Open applications", "push_button"),
            (DIAGNOSTICS_NAME, "push_button"),
        )
        launcher_specs = (
            ("Applications", "pane"),
            ("Search applications", "editable_text"),
            (FIXTURE_RESULT_NAME, "push_button"),
        )
        result_specs = launcher_specs[1:]
        phases = [_phase(Atspi, PHASE_IDS[0], PHASE_FOCUS[0], panel_specs)]
        _require(_invoke_press(Atspi, "Open applications"), "launcher Press action was rejected")
        launcher_search = _wait_until(
            lambda: _find_unique(
                Atspi,
                _walk(_find_application(Atspi)),
                "Search applications",
                "editable_text",
            ),
            "the Prismdrake launcher accessible tree",
        )
        _require(_grab_focus(launcher_search), "launcher search focus request was rejected")
        try:
            launcher = _wait_until(
                lambda: _titled_window_for_accessible(
                    arguments.xdotool, launcher_search, panel, shell.pid
                )
                or _focused_window_for_accessible(
                    Atspi, arguments.xdotool, launcher_search, panel, shell.pid
                )
                or _window_for_accessible(
                    Atspi, arguments.xdotool, launcher_search, panel, shell.pid
                )
                or _visible_window_for_accessible(
                    arguments.xdotool, launcher_search, panel, shell.pid
                )
                or _embedded_panel_for_accessible(
                    Atspi, arguments.xdotool, launcher_search, panel, shell.pid
                ),
                "the Prismdrake launcher window",
            )
        except EvidenceError as error:
            summary = _launcher_probe_summary(
                Atspi, arguments.xdotool, launcher_search, panel, shell.pid
            )
            raise EvidenceError(f"{error}; {summary}") from error
        phases.append(_phase(Atspi, PHASE_IDS[1], PHASE_FOCUS[1], launcher_specs))
        _send_key(arguments.xdotool, "Tab", launcher)
        phases.append(_phase(Atspi, PHASE_IDS[2], PHASE_FOCUS[2], result_specs))
        _send_key(arguments.xdotool, "shift+Tab", launcher)
        phases.append(_phase(Atspi, PHASE_IDS[3], PHASE_FOCUS[3], result_specs))
        _send_key(arguments.xdotool, "Escape", launcher)
        phases.append(_phase(Atspi, PHASE_IDS[4], PHASE_FOCUS[4], panel_specs))
        _send_key(arguments.xdotool, "Tab", panel)
        phases.append(_phase(Atspi, PHASE_IDS[5], PHASE_FOCUS[5], panel_specs))
        _send_key(arguments.xdotool, "shift+Tab", panel)
        phases.append(_phase(Atspi, PHASE_IDS[6], PHASE_FOCUS[6], panel_specs))

        evidence = {
            "application": APPLICATION_NAME,
            "environment": {
                "accessibility_bridge": "at-spi2",
                "display": "isolated-xvfb",
                "keyboard_injection": "xdotool",
                "session_bus": "isolated-dbus-run-session",
            },
            "fixture": {"desktop_entry_count": 1, "profile": "lustre"},
            "phases": phases,
            "schema_version": 1,
        }
        validate_evidence_document(evidence)
        temporary_output = arguments.output.with_suffix(arguments.output.suffix + ".tmp")
        temporary_output.write_text(
            json.dumps(evidence, indent=2, ensure_ascii=True, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temporary_output.replace(arguments.output)
    finally:
        for process in reversed(processes):
            _terminate(process)


def main() -> int:
    try:
        arguments = _parse_arguments()
        if arguments.session_child:
            _run_session_child(arguments)
        else:
            _run_parent(arguments)
    except (EvidenceError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"live AT-SPI evidence: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
