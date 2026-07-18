#!/usr/bin/env python3
"""Run the partial bounded, redacted PD1 production-process demonstration lane."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import select
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping


SHELL_APPLICATION = "prismdrake-shell"
PANEL_TITLE = "Prismdrake Panel"
LAUNCHER_TITLE = "Prismdrake Launcher"
NOTIFICATION_TITLE = "Prismdrake Test Notification"
APP_TITLES = ("Prismdrake Demo App One", "Prismdrake Demo App Two")
LIMITATIONS = [
    "pd1_demo_complete_validation_external",
]
SESSION_CANCELLED_STATUS = 7


class DemoError(RuntimeError):
    """A closed PD1 demonstration operation failed."""


def require(condition: bool, code: str) -> None:
    if not condition:
        raise DemoError(code)


@dataclass(frozen=True)
class ProcessIdentity:
    process_id: int
    start_time: int
    pidfd: int


def parse_cardinals(output: str, property_name: str) -> list[int]:
    match = re.fullmatch(
        rf"{re.escape(property_name)}\(CARDINAL\) = (\d+(?:, \d+)*)\n?", output,
    )
    require(match is not None, "xprop_cardinal_format_invalid")
    values = [int(value) for value in match.group(1).split(", ")]
    require(len(values) <= 128 and all(value <= 0xFFFFFFFF for value in values),
            "xprop_cardinal_value_invalid")
    return values


def parse_atom_list(output: str, property_name: str) -> list[str]:
    match = re.fullmatch(
        rf"{re.escape(property_name)}\(ATOM\) = ([A-Z0-9_]+(?:, [A-Z0-9_]+)*)\n?", output,
    )
    require(match is not None, "xprop_atom_format_invalid")
    values = match.group(1).split(", ")
    require(len(values) <= 64 and len(values) == len(set(values)), "xprop_atom_value_invalid")
    return values


def parse_optional_atom_list(output: str, property_name: str) -> list[str]:
    match = re.fullmatch(
        rf"{re.escape(property_name)}\(ATOM\) =(?: "
        rf"([A-Z0-9_]+(?:, [A-Z0-9_]+)*)| )?\n?",
        output,
    )
    require(match is not None, "xprop_atom_format_invalid")
    if match.group(1) is None:
        return []
    values = match.group(1).split(", ")
    require(len(values) <= 64 and len(values) == len(set(values)) and
            all(len(value) <= 128 for value in values),
            "xprop_atom_value_invalid")
    return values


def parse_window_property(output: str, property_name: str) -> str:
    match = re.fullmatch(
        rf"{re.escape(property_name)}\(WINDOW\): window id # (0x[0-9a-f]+)\n?", output,
    )
    require(match is not None and int(match.group(1), 16) > 0, "xprop_window_format_invalid")
    return match.group(1)


def parse_utf8_property(output: str, property_name: str) -> str:
    match = re.fullmatch(
        rf'{re.escape(property_name)}\(UTF8_STRING\) = "([ -~]{{1,64}})"\n?', output,
    )
    require(match is not None and '"' not in match.group(1), "xprop_utf8_format_invalid")
    return match.group(1)


def select_current_workarea(workareas: list[int], desktop_count: list[int],
                            current_desktop: list[int]) -> list[int]:
    require(len(desktop_count) == 1 and 1 <= desktop_count[0] <= 32,
            "workarea_desktop_count_invalid")
    require(len(current_desktop) == 1 and current_desktop[0] < desktop_count[0],
            "workarea_current_desktop_invalid")
    require(len(workareas) == desktop_count[0] * 4, "workarea_cardinality_invalid")
    offset = current_desktop[0] * 4
    return workareas[offset:offset + 4]


def process_start_time(process_id: int) -> int:
    require(1 < process_id <= 0x7FFFFFFF, "process_identity_invalid")
    try:
        document = Path(f"/proc/{process_id}/stat").read_text(encoding="ascii")
    except OSError as error:
        raise DemoError("process_identity_unavailable") from error
    separator = document.rfind(") ")
    require(separator > 1, "process_identity_stat_invalid")
    fields = document[separator + 2:].split()
    require(len(fields) >= 20 and fields[19].isdigit(), "process_identity_stat_invalid")
    value = int(fields[19])
    require(value > 0, "process_identity_stat_invalid")
    return value


def capture_process_identity(process_id: int) -> ProcessIdentity:
    start_time = process_start_time(process_id)
    try:
        descriptor = os.pidfd_open(process_id, 0)
    except OSError as error:
        raise DemoError("process_pidfd_unavailable") from error
    try:
        require(process_start_time(process_id) == start_time, "process_identity_changed")
    except Exception:
        os.close(descriptor)
        raise
    return ProcessIdentity(process_id, start_time, descriptor)


def process_identity_alive(identity: ProcessIdentity) -> bool:
    if select.select([identity.pidfd], [], [], 0)[0]:
        return False
    try:
        return process_start_time(identity.process_id) == identity.start_time
    except DemoError:
        return False


def require_distinct_process_identity(previous: ProcessIdentity,
                                      current: ProcessIdentity) -> None:
    require((previous.process_id, previous.start_time) !=
            (current.process_id, current.start_time), "process_identity_not_replaced")


def parse_direct_child_ids(document: str) -> list[int]:
    require(isinstance(document, str) and document.isascii() and
            len(document.encode("ascii")) <= 4096 and
            re.fullmatch(r"(?:[1-9][0-9]*[ \n]*)*", document) is not None,
            "direct_child_set_invalid")
    values = [int(value) for value in document.split()]
    require(len(values) <= 16 and len(values) == len(set(values)) and
            all(1 < value <= 0x7FFFFFFF for value in values),
            "direct_child_set_invalid")
    return values


def capture_exact_direct_children(parent_process_id: int,
                                  expected_executables: Mapping[str, Path]) \
        -> dict[str, ProcessIdentity]:
    require(1 < parent_process_id <= 0x7FFFFFFF and
            0 < len(expected_executables) <= 8 and
            all(isinstance(name, str) and
                re.fullmatch(r"[a-z][a-z0-9_]{0,31}", name) is not None and
                isinstance(path, Path) for name, path in expected_executables.items()),
            "direct_child_contract_invalid")
    try:
        expected = {name: path.resolve(strict=True)
                    for name, path in expected_executables.items()}
        document = Path(
            f"/proc/{parent_process_id}/task/{parent_process_id}/children"
        ).read_text(encoding="ascii")
    except OSError as error:
        raise DemoError("direct_child_set_unavailable") from error

    identities: dict[str, ProcessIdentity] = {}
    try:
        for process_id in parse_direct_child_ids(document):
            try:
                executable = Path(f"/proc/{process_id}/exe").resolve(strict=True)
            except OSError as error:
                raise DemoError("direct_child_executable_unavailable") from error
            matches = [name for name, path in expected.items() if executable == path]
            require(len(matches) == 1 and matches[0] not in identities,
                    "direct_child_executable_invalid")
            identities[matches[0]] = capture_process_identity(process_id)
        require(set(identities) == set(expected), "direct_child_set_incomplete")
        return identities
    except Exception:
        close_identities(list(identities.values()))
        raise


def validate_wm_identity(root_owner: str, self_reference: str, name: str) -> None:
    require(root_owner == self_reference, "window_manager_self_reference_invalid")
    require(name == "Openbox", "window_manager_name_invalid")


def select_sole_owned_panel(panels: list[str], owner_process_id: int,
                            expected: ProcessIdentity) -> str:
    require(len(panels) == 1, "panel_ownership_not_sole")
    require(owner_process_id == expected.process_id and process_identity_alive(expected),
            "panel_owner_identity_invalid")
    return panels[0]


def validate_session_log(log_text: str) -> None:
    shell_restart_event = ("component=prismdrake-shell severity=error "
                           "event=component_start_failed generation=none profile=none "
                           "recovery=restart_component")
    settings_restart_event = ("component=prismdrake-settingsd severity=error "
                              "event=component_start_failed generation=none profile=none "
                              "recovery=restart_component")
    require(log_text.count(shell_restart_event) == 1,
            "shell_restart_diagnostic_invalid")
    require(log_text.count(settings_restart_event) == 1,
            "settings_restart_diagnostic_invalid")
    require("event=fallback_selected" not in log_text and
            "event=component_restart_exhausted" not in log_text,
            "session_safe_mode_unexpected")


def validate_cleanup_identities(identities: list[ProcessIdentity]) -> None:
    require(len(identities) <= 16, "cleanup_identity_bound")
    keys = [(identity.process_id, identity.start_time) for identity in identities]
    require(len(keys) == len(set(keys)), "cleanup_identity_duplicate")
    require(all(identity.process_id > 1 and identity.start_time > 0 and identity.pidfd >= 0
                for identity in identities), "cleanup_identity_invalid")


def environment_has_marker(document: bytes, marker: bytes) -> bool:
    require(0 < len(document) <= 1024 * 1024 and 0 < len(marker) <= 4096 and b"\0" not in marker,
            "cleanup_environment_invalid")
    entries = document.split(b"\0")
    require(len(entries) <= 4096, "cleanup_environment_invalid")
    return marker in entries


def discover_marked_processes(executable: Path, marker: bytes) -> list[ProcessIdentity]:
    expected = executable.resolve(strict=True)
    identities: list[ProcessIdentity] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            if (entry / "exe").resolve(strict=True) != expected:
                continue
            environment = (entry / "environ").read_bytes()
        except OSError:
            continue
        if not environment_has_marker(environment, marker):
            continue
        try:
            identity = capture_process_identity(int(entry.name))
        except DemoError:
            continue
        identities.append(identity)
        require(len(identities) <= 16, "cleanup_identity_bound")
    validate_cleanup_identities(identities)
    return identities


def terminate_identities(identities: list[ProcessIdentity]) -> None:
    validate_cleanup_identities(identities)
    for requested_signal, timeout in ((signal.SIGTERM, 2.0), (signal.SIGKILL, 1.0)):
        for identity in identities:
            if process_identity_alive(identity):
                signal.pidfd_send_signal(identity.pidfd, requested_signal)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and any(process_identity_alive(item)
                                                   for item in identities):
            time.sleep(0.02)
        if not any(process_identity_alive(item) for item in identities):
            break
    require(not any(process_identity_alive(item) for item in identities),
            "cleanup_descendant_survived")


def close_identities(identities: list[ProcessIdentity]) -> None:
    for identity in identities:
        try:
            os.close(identity.pidfd)
        except OSError:
            pass


def validate_process_group_target(process_id: int, process_group: int) -> None:
    require(process_id > 1 and process_group == process_id, "cleanup_process_group_invalid")


def validate_output_target(path: Path) -> None:
    require(path.is_absolute() and path.name not in {"", ".", ".."}, "output_target_invalid")
    require(path.parent.is_dir() and not path.parent.is_symlink(), "output_parent_invalid")
    if path.exists() or path.is_symlink():
        require(path.is_file() and not path.is_symlink(), "output_target_invalid")
    temporary = path.with_suffix(path.suffix + ".tmp")
    require(not temporary.exists() and not temporary.is_symlink(), "output_temporary_exists")


def write_evidence(path: Path, document: dict[str, Any]) -> None:
    validate_output_target(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(temporary, flags, 0o600)
    except OSError as error:
        raise DemoError("output_create_failed") from error
    try:
        payload = (json.dumps(document, indent=2, ensure_ascii=True, sort_keys=True) +
                   "\n").encode("utf-8")
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            descriptor = -1
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except OSError as error:
        raise DemoError("output_write_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def sha256_regular_file(path: Path) -> str:
    require(path.is_absolute() and not path.is_symlink(), "artifact_file_invalid")
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise DemoError("artifact_file_unavailable") from error
    try:
        metadata = os.fstat(descriptor)
        require(stat.S_ISREG(metadata.st_mode) and 0 < metadata.st_size <= 256 * 1024 * 1024,
                "artifact_file_invalid")
        digest = hashlib.sha256()
        total = 0
        while True:
            block = os.read(descriptor, 64 * 1024)
            if not block:
                break
            total += len(block)
            require(total <= metadata.st_size, "artifact_file_changed")
            digest.update(block)
        require(total == metadata.st_size and os.fstat(descriptor).st_size == metadata.st_size,
                "artifact_file_changed")
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def validate_artifact_provenance(arguments: argparse.Namespace) -> None:
    lifecycle_path = arguments.portage_lifecycle_evidence
    if arguments.artifact_provenance == "build_tree":
        require(lifecycle_path is None, "build_tree_lifecycle_evidence_unexpected")
        return
    require(arguments.artifact_provenance == "portage_installed" and
            isinstance(lifecycle_path, Path) and lifecycle_path.is_absolute() and
            lifecycle_path.is_file() and not lifecycle_path.is_symlink(),
            "portage_lifecycle_evidence_required")

    gentoo_tests = str(Path(__file__).resolve().parents[1] / "gentoo")
    if gentoo_tests not in sys.path:
        sys.path.insert(0, gentoo_tests)
    try:
        from portage_lifecycle_evidence import (  # pylint: disable=import-outside-toplevel
            EvidenceError,
            load_evidence,
            validate_evidence_document,
        )
    except ImportError as error:
        raise DemoError("portage_lifecycle_evidence_invalid") from error
    try:
        lifecycle = load_evidence(lifecycle_path)
        validate_evidence_document(lifecycle)
    except EvidenceError as error:
        raise DemoError("portage_lifecycle_evidence_invalid") from error

    installed = lifecycle["installed"]
    expected_hashes = {record["path"]: record["sha256"]
                       for record in installed["executables"]}
    executable_paths = {
        "/usr/bin/prismdrake-session": arguments.session,
        "/usr/bin/prismdrake-settingsd": arguments.settingsd,
        "/usr/bin/prismdrake-shell": arguments.shell,
    }
    require(all(str(path) == expected for expected, path in executable_paths.items()),
            "installed_executable_path_mismatch")
    require(all(sha256_regular_file(path) == expected_hashes[expected]
                for expected, path in executable_paths.items()),
            "installed_executable_hash_mismatch")
    driver_hash = lifecycle["runtime_validation"]["complete_demo"]["driver_sha256"]
    require(sha256_regular_file(Path(__file__).resolve()) == driver_hash,
            "installed_demo_driver_mismatch")


def example_evidence(provenance: str = "build_tree") -> dict[str, Any]:
    """Return the sole valid version-three semantic fixture."""
    require(isinstance(provenance, str) and
            provenance in {"build_tree", "portage_installed"}, "artifact_provenance_invalid")
    return {
        "environment": {
            "artifact_provenance": provenance,
            "artifacts": "explicit-absolute-executables",
            "display": "isolated-xvfb-1280x720",
            "lane": "partial-bounded",
            "session_bus": "isolated-dbus-run-session",
            "window_manager": "openbox-ewmh",
        },
        "fallback": {
            "settings_policy_materials_opaque": True,
            "settings_policy_optional_capabilities": "withheld",
            "settings_policy_thumbnail": "application_icon_title_state",
            "task_generic_icon_semantics": True,
        },
        "limitations": list(LIMITATIONS),
        "restart": {
            "application_windows_preserved": True,
            "normal_mode_preserved": True,
            "presentation_epoch_rebuilt": True,
            "settings_owner_gap_observed": True,
            "settingsd_restarted": True,
            "shell_preserved_during_settings_restart": True,
            "shell_restarted": True,
            "task_mirror_rebuilt": True,
            "window_manager_owner_preserved": True,
        },
        "schema_version": 3,
        "scope": {
            "complete_multi_output_policy": False,
            "compositor_blur": False,
            "development_harness": True,
            "glasswyrm_native_protocols": False,
            "login_manager_session": False,
            "production_application_compatibility": False,
            "production_notification_service": False,
            "secure_session": False,
        },
        "settings": {
            "accessibility_survived": {
                "high_contrast": True,
                "reduced_motion": True,
                "transparency_disabled": True,
            },
            "owner_epoch_generations": [[1, 2, 3], [1]],
            "profile_sequence": ["lustre", "forge", "lustre", "lustre"],
        },
        "shutdown": {
            "clean_exit": True,
            "detached_descendants_reaped": True,
            "runtime_markers_removed": True,
        },
        "startup": {
            "complete_generation": 1,
            "dock_type": True,
            "normal_mode": True,
            "panel_geometry": [0, 656, 1280, 64],
            "strut": [0, 0, 0, 64],
            "workarea": [0, 0, 1280, 656],
        },
        "surfaces": {
            "launcher_keyboard_launch_count": 2,
            "notification_keyboard_action": True,
            "panel_keyboard_return": True,
        },
        "tasks": {
            "activation_through_shell": True,
            "close_through_shell": True,
            "controlled_window_count": 2,
            "mirror_count": 2,
            "minimization_through_shell": True,
            "reactivation_through_shell": True,
        },
    }


def validate_evidence(document: Any) -> None:
    """Apply closed semantic and privacy checks independent of jsonschema."""
    require(isinstance(document, dict), "evidence_root_invalid")
    environment = document.get("environment")
    require(isinstance(environment, dict), "evidence_environment_invalid")
    provenance = environment.get("artifact_provenance")
    require(isinstance(provenance, str) and
            provenance in {"build_tree", "portage_installed"}, "artifact_provenance_invalid")
    require(document == example_evidence(provenance), "evidence_semantics_mismatch")
    encoded = json.dumps(document, ensure_ascii=True, sort_keys=True)
    forbidden = (
        "DBUS_SESSION_BUS_ADDRESS",
        "AT_SPI_BUS_ADDRESS",
        "unix:",
        '"pid"',
        '"xid"',
        '"path"',
        APP_TITLES[0],
        APP_TITLES[1],
        "Prismdrake test notification",
    )
    require(not any(value in encoded for value in forbidden), "evidence_private_data")
    require(len(encoded.encode("utf-8")) <= 8192, "evidence_oversized")


def validate_schema(document: Any) -> None:
    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema as project_validate  # pylint: disable=import-outside-toplevel

    schema = json.loads(Path(__file__).with_name("pd1-demo-evidence.schema.json").read_text())
    require(not project_validate(document, schema, schema, "pd1_demo_evidence"),
            "evidence_schema_mismatch")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "accessible-config", "dbus-run-session", "gdbus", "openbox", "output", "session",
        "settingsd", "shell", "test-app", "xdotool", "xprop", "xvfb",
    ):
        parser.add_argument(f"--{name}", type=Path)
    parser.add_argument("--session-child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--artifact-provenance", choices=("build_tree", "portage_installed"))
    parser.add_argument("--portage-lifecycle-evidence", type=Path)
    arguments = parser.parse_args()
    required = [
        "accessible_config", "gdbus", "openbox", "output", "session", "settingsd", "shell",
        "test_app", "xdotool", "xprop",
    ]
    if not arguments.session_child:
        required.extend(("dbus_run_session", "xvfb"))
    if arguments.artifact_provenance is None:
        parser.error("--artifact-provenance is required")
    if arguments.artifact_provenance == "portage_installed":
        if arguments.portage_lifecycle_evidence is None:
            parser.error("--portage-lifecycle-evidence is required for installed artifacts")
        if not arguments.portage_lifecycle_evidence.is_absolute():
            parser.error("--portage-lifecycle-evidence must be absolute")
    elif arguments.portage_lifecycle_evidence is not None:
        parser.error("--portage-lifecycle-evidence is only valid for installed artifacts")
    for name in required:
        value = getattr(arguments, name)
        if value is None:
            parser.error(f"--{name.replace('_', '-')} is required")
        if not value.is_absolute():
            parser.error(f"--{name.replace('_', '-')} must be absolute")
    return arguments


def run_checked(command: list[str], timeout: float = 3) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, timeout=timeout, check=False)


def terminate(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def terminate_process_group(process: subprocess.Popen[Any]) -> None:
    process_group = process.pid
    validate_process_group_target(process.pid, process_group)
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process_group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=2)


def wait_until(predicate: Callable[[], Any], code: str, timeout: float = 8,
               retry_demo_errors: bool = True) -> Any:
    deadline = time.monotonic() + timeout
    last_demo_error: DemoError | None = None
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except DemoError as error:
            if not retry_demo_errors:
                raise
            if re.fullmatch(r"[a-z][a-z0-9_]{0,63}", str(error)) is not None:
                last_demo_error = error
        except (OSError, subprocess.SubprocessError):
            pass
        time.sleep(0.05)
    if last_demo_error is not None:
        raise last_demo_error
    raise DemoError(code)


def openbox_command(executable: Path) -> list[str]:
    return [str(executable), "--sm-disable"]


def openbox_environment(source: Mapping[str, str]) -> dict[str, str]:
    require(all(isinstance(key, str) and isinstance(value, str)
                for key, value in source.items()), "openbox_environment_invalid")
    environment = dict(source)
    environment.pop("XDG_DATA_DIRS", None)
    return environment


def read_display_number(server: subprocess.Popen[str]) -> str:
    require(server.stdout is not None, "xvfb_channel_unavailable")
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    require(bool(selector.select(timeout=8)), "xvfb_start_timeout")
    value = server.stdout.readline().strip()
    require(value.isascii() and value.isdecimal(), "xvfb_display_invalid")
    return value


def children(node: Any) -> list[Any]:
    count = node.get_child_count()
    require(0 <= count <= 256, "atspi_child_bound")
    found: list[Any] = []
    for index in range(count):
        child = node.get_child_at_index(index)
        if child is not None:
            found.append(child)
    return found


def walk(root: Any) -> list[Any]:
    found: list[Any] = []
    pending = [(root, 0)]
    while pending:
        node, depth = pending.pop()
        require(depth <= 32, "atspi_depth_bound")
        found.append(node)
        require(len(found) <= 1024, "atspi_node_bound")
        pending.extend((child, depth + 1) for child in reversed(children(node)))
    return found


def application(atspi: Any) -> Any | None:
    matches = [node for node in children(atspi.get_desktop(0))
               if node.get_name() == SHELL_APPLICATION]
    return matches[0] if len(matches) == 1 else None


def named_nodes(atspi: Any, name: str) -> list[Any]:
    owner = application(atspi)
    if owner is None:
        return []
    return [node for node in walk(owner) if (node.get_name() or "") == name]


def unique_named_node(atspi: Any, name: str) -> Any:
    matches = named_nodes(atspi, name)
    require(len(matches) == 1, "atspi_control_identity")
    return matches[0]


def action_names(node: Any) -> list[str]:
    interface = node.get_action_iface()
    if interface is None:
        return []
    count = interface.get_n_actions()
    require(0 <= count <= 16, "atspi_action_bound")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        return [interface.get_action_name(index) for index in range(count)]


def named_action_nodes(atspi: Any, name: str, action: str) -> list[Any]:
    return [node for node in named_nodes(atspi, name) if action in action_names(node)]


def showing_named_action_nodes(atspi: Any, name: str, action: str) -> list[Any]:
    return [
        node for node in named_action_nodes(atspi, name, action)
        if node.get_state_set().contains(atspi.StateType.SHOWING)
        and node.get_state_set().contains(atspi.StateType.VISIBLE)
    ]


def press(atspi: Any, name: str) -> bool:
    matches = named_action_nodes(atspi, name, "Press")
    require(len(matches) == 1, "atspi_control_identity")
    node = matches[0]
    actions = action_names(node)
    return bool(node.get_action_iface().do_action(actions.index("Press")))


def press_showing(atspi: Any, name: str) -> bool:
    matches = showing_named_action_nodes(atspi, name, "Press")
    require(len(matches) == 1, "atspi_control_identity")
    node = matches[0]
    actions = action_names(node)
    return bool(node.get_action_iface().do_action(actions.index("Press")))


def focused(atspi: Any, name: str) -> bool:
    node = unique_named_node(atspi, name)
    return bool(node.get_state_set().contains(atspi.StateType.FOCUSED))


def showing_focused_action_node(atspi: Any, name: str, action: str = "Press") -> Any:
    matches = [
        node for node in showing_named_action_nodes(atspi, name, action)
        if node.get_state_set().contains(atspi.StateType.FOCUSED)
    ]
    require(len(matches) == 1, "atspi_focused_control_identity")
    return matches[0]


def has_showing_focused_action(atspi: Any, name: str, action: str = "Press") -> bool:
    matches = [
        node for node in showing_named_action_nodes(atspi, name, action)
        if node.get_state_set().contains(atspi.StateType.FOCUSED)
    ]
    return len(matches) == 1


def task_accessible_description(atspi: Any, title: str) -> str:
    matches = showing_named_action_nodes(atspi, title, "Press")
    require(len(matches) == 1, "atspi_control_identity")
    description = matches[0].get_description() or ""
    require(isinstance(description, str) and len(description) <= 256,
            "atspi_description_invalid")
    return description


def task_has_generic_icon_semantics(atspi: Any, title: str) -> bool:
    return task_accessible_description(atspi, title).startswith("Generic application icon.")


def window_ids(xdotool: Path, title: str, only_visible: bool = False) -> list[str]:
    command = [str(xdotool), "search"]
    if only_visible:
        command.append("--onlyvisible")
    command.extend(("--name", f"^{title}$"))
    result = run_checked(command)
    if result.returncode != 0:
        return []
    values = [line for line in result.stdout.splitlines()
              if line.isascii() and line.isdecimal()]
    require(len(values) <= 16 and len(values) == len(set(values)), "window_identity_set_invalid")
    return values


def window_id(xdotool: Path, title: str) -> str | None:
    values = window_ids(xdotool, title)
    return values[0] if len(values) == 1 else None


def visible_window_id(xdotool: Path, title: str) -> str | None:
    values = window_ids(xdotool, title, True)
    return values[0] if len(values) == 1 else None


def window_process_identity(xdotool: Path, identifier: str) -> ProcessIdentity:
    result = run_checked([str(xdotool), "getwindowpid", identifier])
    require(result.returncode == 0 and re.fullmatch(r"[1-9]\d*\n?", result.stdout) is not None,
            "window_process_identity_invalid")
    return capture_process_identity(int(result.stdout.strip()))


def window_process_id(xdotool: Path, identifier: str) -> int:
    result = run_checked([str(xdotool), "getwindowpid", identifier])
    require(result.returncode == 0 and re.fullmatch(r"[1-9]\d*\n?", result.stdout) is not None,
            "window_process_identity_invalid")
    return int(result.stdout.strip())


def geometry(xdotool: Path, identifier: str) -> list[int] | None:
    result = run_checked([str(xdotool), "getwindowgeometry", "--shell", identifier])
    if result.returncode != 0:
        return None
    fields: dict[str, int] = {}
    try:
        for line in result.stdout.splitlines():
            key, value = line.split("=", 1)
            if key in {"X", "Y", "WIDTH", "HEIGHT"}:
                fields[key] = int(value)
    except ValueError:
        return None
    if set(fields) != {"X", "Y", "WIDTH", "HEIGHT"}:
        return None
    return [fields["X"], fields["Y"], fields["WIDTH"], fields["HEIGHT"]]


def send_key(xdotool: Path, identifier: str, key: str) -> None:
    result = run_checked([str(xdotool), "key", "--window", identifier, key])
    require(result.returncode == 0, "keyboard_injection_failed")


def send_text(xdotool: Path, identifier: str, value: str) -> None:
    result = run_checked([str(xdotool), "type", "--window", identifier, "--delay", "2", value])
    require(result.returncode == 0, "keyboard_text_injection_failed")


def focused_window_id(xdotool: Path) -> str:
    result = run_checked([str(xdotool), "getwindowfocus"])
    require(result.returncode == 0 and
            re.fullmatch(r"[1-9]\d*\n?", result.stdout) is not None,
            "keyboard_focus_window_invalid")
    return result.stdout.strip()


def send_focused_key(xdotool: Path, key: str) -> None:
    result = run_checked([str(xdotool), "key", "--clearmodifiers", key])
    require(result.returncode == 0, "focused_keyboard_injection_failed")


def require_shell_owner(atspi: Any, xdotool: Path, panel: str,
                        shell_process_id: int) -> None:
    require(shell_process_id > 1, "shell_process_identity_invalid")
    require(window_process_id(xdotool, panel) == shell_process_id,
            "panel_owner_identity_invalid")
    require(process_application_id(atspi) == shell_process_id,
            "atspi_owner_identity_invalid")


def send_focused_window_action_key(atspi: Any, xdotool: Path, expected_window: str,
                                   shell_process_id: int, accessible_name: str,
                                   key: str) -> None:
    require_shell_owner(atspi, xdotool, expected_window, shell_process_id)
    require(focused_window_id(xdotool) == expected_window, "action_window_focus_lost")
    showing_focused_action_node(atspi, accessible_name)
    send_focused_key(xdotool, key)


def send_focused_shell_action_key(atspi: Any, xdotool: Path, panel: str,
                                  shell_process_id: int, accessible_name: str,
                                  key: str) -> None:
    require_shell_owner(atspi, xdotool, panel, shell_process_id)
    focused_window = focused_window_id(xdotool)
    if focused_window != panel:
        require(window_process_id(xdotool, focused_window) == shell_process_id,
                "action_shell_window_owner_invalid")
    showing_focused_action_node(atspi, accessible_name)
    send_focused_key(xdotool, key)


def accept_first_result_from_focused_search(atspi: Any, xdotool: Path,
                                             launcher: str) -> None:
    require(focused_window_id(xdotool) == launcher, "launcher_window_focus_lost")
    require(focused(atspi, "Search applications"), "launcher_search_focus_lost")
    send_focused_key(xdotool, "Return")


def focus_task_from_panel(atspi: Any, xdotool: Path, panel: str,
                          shell_process_id: int, title: str) -> None:
    require(title in APP_TITLES, "task_navigation_target_invalid")
    require_shell_owner(atspi, xdotool, panel, shell_process_id)
    require(press(atspi, "Open applications"), "launcher_press_rejected")
    launcher = wait_until(lambda: visible_window_id(xdotool, LAUNCHER_TITLE),
                          "launcher_window_timeout")
    wait_until(lambda: bool(named_nodes(atspi, "Search applications")) and
               focused(atspi, "Search applications"), "launcher_search_focus_timeout")
    require(focused_window_id(xdotool) == launcher, "launcher_window_focus_lost")
    send_focused_key(xdotool, "Escape")
    wait_until(lambda: visible_window_id(xdotool, LAUNCHER_TITLE) is None,
               "launcher_dismiss_timeout")
    wait_until(lambda: focused_window_id(xdotool) == panel and
               has_showing_focused_action(atspi, "Open applications"),
               "panel_keyboard_return_timeout")

    previous = "Open applications"
    for candidate in APP_TITLES[:APP_TITLES.index(title) + 1]:
        send_focused_window_action_key(
            atspi, xdotool, panel, shell_process_id, previous, "Tab",
        )
        wait_until(lambda candidate=candidate:
                   has_showing_focused_action(atspi, candidate),
                   "task_keyboard_focus_timeout")
        previous = candidate


def open_task_context_menu(atspi: Any, xdotool: Path, panel: str,
                           shell_process_id: int, title: str,
                           expected_action: str = "Minimize") -> None:
    require(expected_action in {"Minimize", "Close"},
            "task_context_action_invalid")
    focus_task_from_panel(atspi, xdotool, panel, shell_process_id, title)
    send_focused_window_action_key(
        atspi, xdotool, panel, shell_process_id, title, "shift+F10",
    )
    require_task_context_menu_focus(
        atspi, xdotool, panel, shell_process_id, title, True, expected_action,
    )


def require_task_context_menu_focus(atspi: Any, xdotool: Path, panel: str,
                                    shell_process_id: int, title: str,
                                    require_keyboard_focus: bool,
                                    expected_action: str = "Minimize") -> None:
    require_shell_owner(atspi, xdotool, panel, shell_process_id)
    action_name = f"{expected_action} {title}"
    try:
        wait_until(lambda: has_showing_focused_action(atspi, action_name),
                   "task_context_menu_focus_timeout")
    except DemoError:
        nodes = named_nodes(atspi, action_name)
        interactive = named_action_nodes(atspi, action_name, "Press")
        showing_count = sum(
            node.get_state_set().contains(atspi.StateType.SHOWING) for node in interactive
        )
        visible_count = sum(
            node.get_state_set().contains(atspi.StateType.VISIBLE) for node in interactive
        )
        focused_count = sum(
            node.get_state_set().contains(atspi.StateType.FOCUSED) for node in interactive
        )
        bare_minimize_count = len(named_nodes(atspi, "Minimize"))
        bare_close_count = len(named_nodes(atspi, "Close"))
        task_description = task_accessible_description(atspi, title)
        print(
            "PD1 task-menu diagnostic: "
            f"named_count={len(nodes)} interactive_count={len(interactive)} "
            f"showing_count={showing_count} visible_count={visible_count} "
            f"focused_count={focused_count} "
            f"bare_counts={bare_minimize_count},{bare_close_count} "
            f"menu_marker={int('Window actions shown.' in task_description)} "
            f"action_sets={';'.join(','.join(action_names(node)) for node in nodes)}",
            file=sys.stderr,
        )
        raise
    if require_keyboard_focus:
        focused_window = focused_window_id(xdotool)
        require(focused_window == panel or
                window_process_id(xdotool, focused_window) == shell_process_id,
                "action_shell_window_owner_invalid")


def task_screen_center(atspi: Any, xdotool: Path, panel: str,
                       title: str) -> tuple[int, int]:
    matches = showing_named_action_nodes(atspi, title, "Press")
    require(len(matches) == 1, "atspi_control_identity")
    component = matches[0].get_component_iface()
    require(component is not None, "atspi_component_unavailable")
    rectangle = component.get_extents(atspi.CoordType.SCREEN)
    values = (rectangle.x, rectangle.y, rectangle.width, rectangle.height)
    require(all(isinstance(value, int) and not isinstance(value, bool) for value in values),
            "atspi_extents_invalid")
    x, y, width, height = values
    require(width > 0 and height > 0 and width <= 1280 and height <= 720,
            "atspi_extents_invalid")
    require(x >= 0 and y >= 0 and x + width <= 1280 and y + height <= 720,
            "atspi_extents_invalid")
    panel_geometry = geometry(xdotool, panel)
    require(panel_geometry is not None, "panel_geometry_unavailable")
    panel_x, panel_y, panel_width, panel_height = panel_geometry
    require(x >= panel_x and y >= panel_y and
            x + width <= panel_x + panel_width and
            y + height <= panel_y + panel_height,
            "task_extents_outside_panel")
    return x + (width // 2), y + (height // 2)


def open_task_context_menu_from_pointer(atspi: Any, xdotool: Path, panel: str,
                                        shell_process_id: int, title: str) -> None:
    require_shell_owner(atspi, xdotool, panel, shell_process_id)
    center_x, center_y = task_screen_center(atspi, xdotool, panel, title)
    result = run_checked([
        str(xdotool), "mousemove", "--sync", str(center_x), str(center_y), "click", "3",
    ])
    require(result.returncode == 0, "pointer_context_injection_failed")
    require_task_context_menu_focus(
        atspi, xdotool, panel, shell_process_id, title, False,
    )


def minimize_task_from_context_menu(atspi: Any, xdotool: Path, panel: str,
                                    shell_process_id: int, title: str) -> None:
    open_task_context_menu(atspi, xdotool, panel, shell_process_id, title)
    send_focused_shell_action_key(
        atspi, xdotool, panel, shell_process_id, f"Minimize {title}", "space",
    )


def close_minimized_task_from_context_menu(atspi: Any, xdotool: Path, panel: str,
                                           shell_process_id: int, title: str) -> None:
    open_task_context_menu(
        atspi, xdotool, panel, shell_process_id, title, "Close",
    )
    send_focused_shell_action_key(
        atspi, xdotool, panel, shell_process_id, f"Close {title}", "space",
    )


def close_task_from_pointer_context_menu(atspi: Any, xdotool: Path, panel: str,
                                         shell_process_id: int, title: str) -> None:
    open_task_context_menu_from_pointer(atspi, xdotool, panel, shell_process_id, title)
    require_shell_owner(atspi, xdotool, panel, shell_process_id)
    require(press_showing(atspi, f"Close {title}"),
            "task_close_accessible_action_rejected")


def xprop_output(xprop: Path, target: list[str], name: str) -> str:
    result = run_checked([str(xprop), *target, name])
    require(result.returncode == 0, "x11_property_unavailable")
    require(not result.stderr, "x11_property_diagnostic")
    return result.stdout


def xprop_cardinals(xprop: Path, target: list[str], name: str) -> list[int]:
    return parse_cardinals(xprop_output(xprop, target, name), name)


def window_state_atoms(xprop: Path, identifier: str) -> list[str]:
    return parse_optional_atom_list(
        xprop_output(xprop, ["-id", identifier], "_NET_WM_STATE"),
        "_NET_WM_STATE",
    )


def current_snapshot(gdbus: Path) -> dict[str, Any]:
    result = run_checked([
        str(gdbus), "call", "--session", "--dest", "org.prismdrake.Settings1",
        "--object-path", "/org/prismdrake/Settings1", "--method",
        "org.prismdrake.SettingsSnapshot1.GetCurrentSnapshot", "1",
    ], timeout=2)
    require(result.returncode == 0, "settings_snapshot_unavailable")
    octets = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", result.stdout))
    require(0 < len(octets) <= 1024 * 1024, "settings_snapshot_encoding_invalid")
    document = json.loads(octets.decode("utf-8"))
    require(isinstance(document, dict), "settings_snapshot_invalid")
    return document


def request_profile(gdbus: Path, profile: str) -> int:
    result = run_checked([
        str(gdbus), "call", "--session", "--dest", "org.prismdrake.Settings1",
        "--object-path", "/org/prismdrake/Settings1", "--method",
        "org.prismdrake.Settings1.RequestProfileChange", profile,
    ], timeout=6)
    require(result.returncode == 0, "profile_change_failed")
    match = re.search(r"uint64\s+(\d+)", result.stdout)
    require(match is not None, "profile_generation_invalid")
    return int(match.group(1))


def desktop_exec(path: Path, instance: str) -> str:
    value = str(path).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{value}" --instance {instance}'


def write_desktop_entries(directory: Path, executable: Path) -> None:
    names = (("one", APP_TITLES[0]), ("two", APP_TITLES[1]))
    for index, (instance, name) in enumerate(names, start=1):
        (directory / f"prismdrake-demo-{index}.desktop").write_text(
            "[Desktop Entry]\nType=Application\n"
            f"Name={name}\nGenericName=Controlled application\n"
            "Comment=Deterministic PD1 demonstration fixture\n"
            f"Exec={desktop_exec(executable, instance)}\nTerminal=false\n",
            encoding="utf-8",
        )


def launch_from_keyboard(atspi: Any, xdotool: Path, title: str) -> str:
    require(press(atspi, "Open applications"), "launcher_press_rejected")
    wait_until(lambda: visible_window_id(xdotool, LAUNCHER_TITLE), "launcher_window_timeout")
    launcher = visible_window_id(xdotool, LAUNCHER_TITLE)
    require(launcher is not None, "launcher_window_missing")
    wait_until(lambda: bool(named_nodes(atspi, "Search applications")),
               "launcher_search_timeout")
    send_key(xdotool, launcher, "ctrl+a")
    send_text(xdotool, launcher, title)
    wait_until(lambda: len(named_nodes(atspi, title)) == 1, "launcher_result_timeout")
    accept_first_result_from_focused_search(atspi, xdotool, launcher)
    return wait_until(lambda: window_id(xdotool, title), "controlled_window_timeout")


def process_application_id(atspi: Any) -> int:
    owner = application(atspi)
    require(owner is not None, "atspi_application_missing")
    identifier = owner.get_process_id()
    require(isinstance(identifier, int) and identifier > 1, "atspi_process_identity_invalid")
    return identifier


def wm_identity(xprop: Path) -> tuple[str, str]:
    owner = parse_window_property(
        xprop_output(xprop, ["-root"], "_NET_SUPPORTING_WM_CHECK"),
        "_NET_SUPPORTING_WM_CHECK",
    )
    self_reference = parse_window_property(
        xprop_output(xprop, ["-id", owner], "_NET_SUPPORTING_WM_CHECK"),
        "_NET_SUPPORTING_WM_CHECK",
    )
    name = parse_utf8_property(
        xprop_output(xprop, ["-id", owner], "_NET_WM_NAME"), "_NET_WM_NAME",
    )
    validate_wm_identity(owner, self_reference, name)
    return owner, name


def runtime_markers(runtime: Path) -> tuple[list[Path], list[Path]]:
    return (list(runtime.glob("*/ready")), list(runtime.glob("*/safe-mode")))


def require_normal_ready(runtime: Path) -> None:
    ready, safe_mode = runtime_markers(runtime)
    require(len(ready) == 1 and not safe_mode, "session_normal_readiness_invalid")


def require_normal_snapshot(snapshot: dict[str, Any]) -> None:
    warnings_value = snapshot.get("theme", {}).get("warnings")
    require(isinstance(warnings_value, list) and "safe_mode_active" not in warnings_value,
            "settings_safe_mode_unexpected")


def validate_panel_contract(xdotool: Path, xprop: Path, panel: str) -> None:
    wait_until(
        lambda: (True if geometry(xdotool, panel) == [0, 656, 1280, 64] else None),
        "panel_geometry_timeout",
    )
    require(xprop_cardinals(xprop, ["-id", panel], "_NET_WM_STRUT") == [0, 0, 0, 64],
            "panel_strut_invalid")
    atoms = parse_atom_list(
        xprop_output(xprop, ["-id", panel], "_NET_WM_WINDOW_TYPE"),
        "_NET_WM_WINDOW_TYPE",
    )
    require(atoms == ["_NET_WM_WINDOW_TYPE_DOCK"], "panel_dock_type_invalid")
    require(
        xprop_cardinals(xprop, ["-id", panel], "_NET_WM_STRUT_PARTIAL")
        == [0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 1279],
        "panel_partial_strut_invalid",
    )

    def expected_workarea() -> bool:
        workareas = xprop_cardinals(xprop, ["-root"], "_NET_WORKAREA")
        desktop_count = xprop_cardinals(xprop, ["-root"], "_NET_NUMBER_OF_DESKTOPS")
        current_desktop = xprop_cardinals(xprop, ["-root"], "_NET_CURRENT_DESKTOP")
        return select_current_workarea(workareas, desktop_count, current_desktop) == [
            0, 0, 1280, 656,
        ]

    wait_until(expected_workarea, "workarea_timeout", retry_demo_errors=False)


def run_session_child(arguments: argparse.Namespace) -> None:
    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi

    root_value = os.environ.get("PRISMDRAKE_DEMO_TEMPORARY", "")
    root = Path(root_value)
    require(root.is_absolute() and root.is_dir(), "private_root_unavailable")
    home = root / "home"
    config = root / "config"
    data = root / "data"
    applications = data / "applications"
    cache = root / "cache"
    state = root / "state"
    data_dirs = root / "data-dirs"
    for directory in (home, config / "prismdrake", applications, cache, state, data_dirs):
        directory.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(arguments.accessible_config, config / "prismdrake" / "config.toml")
    write_desktop_entries(applications, arguments.test_app)
    os.environ.update({
        "HOME": str(home),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "XDG_CACHE_HOME": str(cache),
        "XDG_CONFIG_HOME": str(config),
        "XDG_DATA_DIRS": str(data_dirs),
        "XDG_DATA_HOME": str(data),
        "XDG_STATE_HOME": str(state),
    })

    processes: list[subprocess.Popen[Any]] = []
    detached_identities: list[ProcessIdentity] = []
    observation_identities: list[ProcessIdentity] = []
    session_log = root / "session.log"
    log: Any | None = None
    try:
        openbox = subprocess.Popen(openbox_command(arguments.openbox), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   env=openbox_environment(os.environ))
        processes.append(openbox)

        def ready_window_manager() -> tuple[str, str]:
            require(openbox.poll() is None, "window_manager_exited")
            return wm_identity(arguments.xprop)

        wait_until(ready_window_manager, "window_manager_start_timeout")
        initial_wm_identity = wm_identity(arguments.xprop)
        log = session_log.open("w", encoding="utf-8")
        session = subprocess.Popen([
            str(arguments.session), "--settingsd", str(arguments.settingsd), "--shell",
            str(arguments.shell),
        ], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log)
        processes.append(session)

        prismdrake_runtime = Path(os.environ["XDG_RUNTIME_DIR"]) / "prismdrake"

        def ready_marker() -> bool:
            if session.poll() is not None:
                raise DemoError("session_exited_before_readiness")
            ready, safe_mode = runtime_markers(prismdrake_runtime)
            if safe_mode:
                raise DemoError("session_safe_mode_unexpected")
            return len(ready) == 1

        wait_until(ready_marker, "session_readiness_timeout", retry_demo_errors=False)
        require_normal_ready(prismdrake_runtime)
        panel = wait_until(lambda: window_id(arguments.xdotool, PANEL_TITLE),
                           "panel_window_timeout")
        initial_snapshot = wait_until(lambda: current_snapshot(arguments.gdbus),
                                      "initial_snapshot_timeout")
        require(initial_snapshot.get("generation") == 1 and
                initial_snapshot.get("profile_id") == "lustre", "initial_generation_invalid")
        require_normal_snapshot(initial_snapshot)

        validate_panel_contract(arguments.xdotool, arguments.xprop, panel)

        Atspi.init()
        wait_until(lambda: application(Atspi), "atspi_application_timeout")
        first = launch_from_keyboard(Atspi, arguments.xdotool, APP_TITLES[0])
        second = launch_from_keyboard(Atspi, arguments.xdotool, APP_TITLES[1])
        require(first != second, "controlled_window_identity_collision")
        detached_identities.extend((
            window_process_identity(arguments.xdotool, first),
            window_process_identity(arguments.xdotool, second),
        ))
        validate_cleanup_identities(detached_identities)
        try:
            wait_until(
                lambda: len(showing_named_action_nodes(Atspi, APP_TITLES[0], "Press")) == 1 and
                len(showing_named_action_nodes(Atspi, APP_TITLES[1], "Press")) == 1,
                       "task_mirror_timeout")
        except DemoError:
            interactive_counts = tuple(
                len(showing_named_action_nodes(Atspi, title, "Press")) for title in APP_TITLES
            )
            print(
                "PD1 task diagnostic: expected_count=2 "
                f"interactive_counts={interactive_counts[0]},{interactive_counts[1]}",
                file=sys.stderr,
            )
            raise
        require(press_showing(Atspi, APP_TITLES[0]), "task_activation_rejected")
        wait_until(
            lambda: (first if run_checked([
                str(arguments.xdotool), "getactivewindow",
            ]).stdout.strip() == first else None),
            "task_activation_timeout",
        )
        initial_shell_process_id = process_application_id(Atspi)
        require(task_has_generic_icon_semantics(Atspi, APP_TITLES[0]) and
                task_has_generic_icon_semantics(Atspi, APP_TITLES[1]),
                "task_generic_icon_semantics_invalid")

        minimize_task_from_context_menu(
            Atspi, arguments.xdotool, panel, initial_shell_process_id, APP_TITLES[0],
        )
        wait_until(
            lambda: (True if "_NET_WM_STATE_HIDDEN" in
                     window_state_atoms(arguments.xprop, first) else None),
            "task_minimization_timeout",
        )
        wait_until(
            lambda: (True if "Minimized" in
                     task_accessible_description(Atspi, APP_TITLES[0]) else None),
            "task_minimized_accessibility_timeout",
        )

        focus_task_from_panel(
            Atspi, arguments.xdotool, panel, initial_shell_process_id, APP_TITLES[0],
        )
        send_focused_window_action_key(
            Atspi, arguments.xdotool, panel, initial_shell_process_id,
            APP_TITLES[0], "Return",
        )
        wait_until(
            lambda: (first if run_checked([
                str(arguments.xdotool), "getactivewindow",
            ]).stdout.strip() == first and
            "_NET_WM_STATE_HIDDEN" not in window_state_atoms(arguments.xprop, first)
            else None),
            "task_reactivation_timeout",
        )

        forge_generation = request_profile(arguments.gdbus, "forge")
        forge = wait_until(lambda: current_snapshot(arguments.gdbus).get("generation") == 2 and
                           current_snapshot(arguments.gdbus), "forge_snapshot_timeout")
        lustre_generation = request_profile(arguments.gdbus, "lustre")
        final_snapshot = wait_until(
            lambda: current_snapshot(arguments.gdbus).get("generation") == 3 and
            current_snapshot(arguments.gdbus), "lustre_snapshot_timeout")
        require(forge_generation == 2 and lustre_generation == 3 and
                forge.get("profile_id") == "forge" and final_snapshot.get("profile_id") == "lustre",
                "profile_sequence_invalid")
        for snapshot in (initial_snapshot, forge, final_snapshot):
            effective = snapshot.get("theme", {}).get("effective_accessibility", {})
            require(effective.get("high_contrast") is True and
                    effective.get("reduced_motion") is True and
                    effective.get("transparency_disabled") is True,
                    "accessibility_override_lost")

        require(press(Atspi, "Send test notification"), "notification_press_rejected")
        notification = wait_until(lambda: visible_window_id(arguments.xdotool, NOTIFICATION_TITLE),
                                  "notification_window_timeout")
        wait_until(lambda: bool(named_nodes(Atspi, "Acknowledge")),
                   "notification_action_timeout")
        send_key(arguments.xdotool, notification, "Tab")
        wait_until(lambda: focused(Atspi, "Acknowledge"), "notification_focus_timeout")
        send_key(arguments.xdotool, notification, "space")
        wait_until(lambda: visible_window_id(arguments.xdotool, NOTIFICATION_TITLE) is None,
                   "notification_dismiss_timeout")
        wait_until(lambda: focused(Atspi, "Send test notification"),
                   "notification_panel_return_timeout")

        old_shell_identity = capture_process_identity(process_application_id(Atspi))
        restarted_shell_process_id = 0
        try:
            signal.pidfd_send_signal(old_shell_identity.pidfd, signal.SIGKILL)
            wait_until(lambda: not process_identity_alive(old_shell_identity),
                       "old_shell_exit_timeout", timeout=12)

            def new_shell_owner() -> ProcessIdentity | None:
                owner = application(Atspi)
                if owner is None:
                    return None
                candidate = capture_process_identity(process_application_id(Atspi))
                try:
                    require_distinct_process_identity(old_shell_identity, candidate)
                except DemoError:
                    os.close(candidate.pidfd)
                    return None
                return candidate

            new_shell_identity = wait_until(new_shell_owner, "shell_restart_atspi_timeout",
                                            timeout=12)
            observation_identities.append(new_shell_identity)
        finally:
            os.close(old_shell_identity.pidfd)

        def sole_owned_panel() -> str | None:
            panels = window_ids(arguments.xdotool, PANEL_TITLE)
            if len(panels) != 1:
                return None
            result = run_checked([str(arguments.xdotool), "getwindowpid", panels[0]])
            if result.returncode != 0 or not result.stdout.strip().isdigit():
                return None
            try:
                return select_sole_owned_panel(
                    panels, int(result.stdout.strip()), new_shell_identity,
                )
            except DemoError:
                return None

        new_panel = wait_until(sole_owned_panel, "shell_restart_panel_timeout", timeout=12)
        restarted_shell_process_id = new_shell_identity.process_id
        require_normal_ready(prismdrake_runtime)
        restarted_snapshot = current_snapshot(arguments.gdbus)
        require(restarted_snapshot.get("generation") == 3, "restart_generation_invalid")
        require_normal_snapshot(restarted_snapshot)
        validate_panel_contract(arguments.xdotool, arguments.xprop, new_panel)
        require(window_id(arguments.xdotool, APP_TITLES[0]) == first and
                window_id(arguments.xdotool, APP_TITLES[1]) == second,
                "application_state_lost_on_restart")
        wait_until(
            lambda: len(showing_named_action_nodes(Atspi, APP_TITLES[0], "Press")) == 1 and
            len(showing_named_action_nodes(Atspi, APP_TITLES[1], "Press")) == 1,
                   "task_mirror_rebuild_timeout", timeout=12)
        require(wm_identity(arguments.xprop) == initial_wm_identity,
                "window_manager_owner_changed")
        require(task_has_generic_icon_semantics(Atspi, APP_TITLES[0]) and
                task_has_generic_icon_semantics(Atspi, APP_TITLES[1]),
                "task_generic_icon_semantics_invalid")

        expected_children = {"settingsd": arguments.settingsd, "shell": arguments.shell}
        pre_settings_restart = capture_exact_direct_children(session.pid, expected_children)
        observation_identities.extend(pre_settings_restart.values())
        old_settings_identity = pre_settings_restart["settingsd"]
        observed_shell_identity = pre_settings_restart["shell"]
        require((observed_shell_identity.process_id, observed_shell_identity.start_time) ==
                (new_shell_identity.process_id, new_shell_identity.start_time),
                "direct_shell_identity_mismatch")

        signal.pidfd_send_signal(old_settings_identity.pidfd, signal.SIGKILL)
        wait_until(lambda: not process_identity_alive(old_settings_identity),
                   "old_settings_exit_timeout", timeout=12)
        wait_until(lambda: window_id(arguments.xdotool, PANEL_TITLE) is None,
                   "settings_presentation_gap_timeout", timeout=12)
        require(process_identity_alive(new_shell_identity),
                "shell_lost_during_settings_restart")
        require(window_id(arguments.xdotool, APP_TITLES[0]) == first and
                window_id(arguments.xdotool, APP_TITLES[1]) == second,
                "application_state_lost_on_settings_restart")
        require(wm_identity(arguments.xprop) == initial_wm_identity,
                "window_manager_owner_changed")
        require_normal_ready(prismdrake_runtime)

        def recovered_settings_snapshot() -> dict[str, Any] | None:
            snapshot = current_snapshot(arguments.gdbus)
            if snapshot.get("generation") != 1 or snapshot.get("profile_id") != "lustre":
                return None
            return snapshot

        recovered_snapshot = wait_until(
            recovered_settings_snapshot, "settings_owner_recovery_timeout", timeout=12,
        )
        require_normal_snapshot(recovered_snapshot)
        recovered_effective = recovered_snapshot.get("theme", {}).get(
            "effective_accessibility", {}
        )
        require(recovered_effective.get("high_contrast") is True and
                recovered_effective.get("reduced_motion") is True and
                recovered_effective.get("transparency_disabled") is True,
                "accessibility_override_lost_on_settings_restart")

        post_settings_restart = wait_until(
            lambda: capture_exact_direct_children(session.pid, expected_children),
            "settings_restart_child_set_timeout", timeout=12,
        )
        observation_identities.extend(post_settings_restart.values())
        require_distinct_process_identity(
            old_settings_identity, post_settings_restart["settingsd"],
        )
        require((post_settings_restart["shell"].process_id,
                 post_settings_restart["shell"].start_time) ==
                (new_shell_identity.process_id, new_shell_identity.start_time),
                "shell_replaced_during_settings_restart")
        new_panel = wait_until(sole_owned_panel, "settings_restart_panel_timeout", timeout=12)
        validate_panel_contract(arguments.xdotool, arguments.xprop, new_panel)
        wait_until(
            lambda: len(showing_named_action_nodes(Atspi, APP_TITLES[0], "Press")) == 1 and
            len(showing_named_action_nodes(Atspi, APP_TITLES[1], "Press")) == 1,
            "settings_restart_task_mirror_timeout", timeout=12,
        )
        require(process_identity_alive(new_shell_identity) and
                task_has_generic_icon_semantics(Atspi, APP_TITLES[0]) and
                task_has_generic_icon_semantics(Atspi, APP_TITLES[1]),
                "settings_restart_presentation_invalid")

        materials = recovered_snapshot["theme"]["resolved_materials"]
        require("thumbnail_fallback_active" in recovered_snapshot["theme"]["warnings"] and
                all(material["used_fallback"] and material["opacity"] == 1.0
                    for material in materials.values()), "opaque_fallback_invalid")
        thumbnail = recovered_snapshot["theme"]["thumbnail_presentation"]

        close_task_from_pointer_context_menu(
            Atspi, arguments.xdotool, new_panel, restarted_shell_process_id, APP_TITLES[1],
        )
        wait_until(lambda: window_id(arguments.xdotool, APP_TITLES[1]) is None and
                   not named_action_nodes(Atspi, APP_TITLES[1], "Press") and
                   not process_identity_alive(detached_identities[1]),
                   "pointer_controlled_window_close_timeout")
        minimize_task_from_context_menu(
            Atspi, arguments.xdotool, new_panel, restarted_shell_process_id, APP_TITLES[0],
        )
        wait_until(
            lambda: (True if "_NET_WM_STATE_HIDDEN" in
                     window_state_atoms(arguments.xprop, first) else None),
            "task_cleanup_minimization_timeout",
        )
        wait_until(
            lambda: (True if "Minimized" in
                     task_accessible_description(Atspi, APP_TITLES[0]) else None),
            "task_cleanup_minimized_accessibility_timeout",
        )
        close_minimized_task_from_context_menu(
            Atspi, arguments.xdotool, new_panel, restarted_shell_process_id, APP_TITLES[0],
        )
        try:
            wait_until(lambda: window_id(arguments.xdotool, APP_TITLES[0]) is None and
                       not showing_named_action_nodes(Atspi, APP_TITLES[0], "Press") and
                       not process_identity_alive(detached_identities[0]),
                       "keyboard_controlled_window_close_timeout")
        except DemoError:
            showing_task_count = len(
                showing_named_action_nodes(Atspi, APP_TITLES[0], "Press")
            )
            print(
                "PD1 keyboard-close diagnostic: "
                f"window_count={len(window_ids(arguments.xdotool, APP_TITLES[0]))} "
                f"showing_task_count={showing_task_count} "
                f"process_alive={int(process_identity_alive(detached_identities[0]))}",
                file=sys.stderr,
            )
            raise
        wait_until(lambda: not any(process_identity_alive(item) for item in detached_identities),
                   "controlled_process_shutdown_timeout")

        session.terminate()
        status = session.wait(timeout=6)
        log.flush()
        log.close()
        log = None
        processes.remove(session)
        marker_count = len(list(prismdrake_runtime.glob("*/ready"))) + len(
            list(prismdrake_runtime.glob("*/safe-mode")))
        log_text = session_log.read_text(encoding="utf-8")
        validate_session_log(log_text)
        if status != SESSION_CANCELLED_STATUS or marker_count != 0:
            print(
                f"PD1 shutdown diagnostic: session_status={status} marker_count={marker_count}",
                file=sys.stderr,
            )
        require(status == SESSION_CANCELLED_STATUS and marker_count == 0,
                "session_shutdown_invalid")

        evidence = example_evidence(arguments.artifact_provenance)
        evidence["fallback"]["settings_policy_thumbnail"] = thumbnail
        validate_evidence(evidence)
        write_evidence(arguments.output, evidence)
    finally:
        cleanup_error: DemoError | None = None
        if detached_identities:
            try:
                terminate_identities(detached_identities)
            except DemoError as error:
                cleanup_error = error
            finally:
                close_identities(detached_identities)
        close_identities(observation_identities)
        if log is not None:
            log.close()
        for process in reversed(processes):
            terminate(process)
        if cleanup_error is not None:
            raise cleanup_error


def run_parent(arguments: argparse.Namespace) -> None:
    validate_artifact_provenance(arguments)
    executable_names = (
        "dbus_run_session", "gdbus", "openbox", "session", "settingsd", "shell", "test_app",
        "xdotool", "xprop", "xvfb",
    )
    for name in executable_names:
        path = getattr(arguments, name)
        require(path.is_file() and os.access(path, os.X_OK), "required_executable_unavailable")
    require(arguments.accessible_config.is_file(), "accessible_config_unavailable")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    validate_output_target(arguments.output)
    with tempfile.TemporaryDirectory(prefix="prismdrake-pd1-demo-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        (runtime / "prismdrake").mkdir(mode=0o700)
        server = subprocess.Popen([
            str(arguments.xvfb), "-displayfd", "1", "-screen", "0", "1280x720x24", "-noreset",
            "-nolisten", "tcp",
        ], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            start_new_session=True)
        child: subprocess.Popen[Any] | None = None
        try:
            display = read_display_number(server)
            environment = os.environ.copy()
            environment.update({
                "DISPLAY": f":{display}",
                "PRISMDRAKE_DEMO_TEMPORARY": temporary,
                "QT_ACCESSIBILITY": "1",
                "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
                "QT_QPA_PLATFORM": "xcb",
                "QT_QUICK_BACKEND": "software",
                "QT_QUICK_CONTROLS_STYLE": "Basic",
                "XDG_RUNTIME_DIR": str(runtime),
            })
            command = [
                str(arguments.dbus_run_session), "--", sys.executable, str(Path(__file__).resolve()),
                "--session-child",
            ]
            for name in (
                "accessible_config", "gdbus", "openbox", "output", "session", "settingsd",
                "shell", "test_app", "xdotool", "xprop",
            ):
                command.extend((f"--{name.replace('_', '-')}", str(getattr(arguments, name))))
            command.extend(("--artifact-provenance", arguments.artifact_provenance))
            child = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL,
                                     start_new_session=True)
            require(child.wait(timeout=65) == 0, "isolated_demo_failed")
        except subprocess.TimeoutExpired as error:
            raise DemoError("isolated_demo_timeout") from error
        finally:
            cleanup_error: DemoError | None = None
            if child is not None:
                try:
                    terminate_process_group(child)
                except (OSError, subprocess.SubprocessError, DemoError):
                    cleanup_error = DemoError("cleanup_process_group_failed")
            escaped: list[ProcessIdentity] = []
            try:
                marker = f"PRISMDRAKE_DEMO_TEMPORARY={temporary}".encode("utf-8")
                escaped = discover_marked_processes(arguments.test_app, marker)
                terminate_identities(escaped)
            except DemoError as error:
                cleanup_error = error
            finally:
                close_identities(escaped)
                try:
                    terminate_process_group(server)
                except (OSError, subprocess.SubprocessError, DemoError):
                    if cleanup_error is None:
                        cleanup_error = DemoError("cleanup_xvfb_group_failed")
            if cleanup_error is not None:
                raise cleanup_error
    document = json.loads(arguments.output.read_text(encoding="utf-8"))
    validate_schema(document)
    validate_evidence(document)
    require(document["environment"]["artifact_provenance"] == arguments.artifact_provenance,
            "artifact_provenance_mismatch")


def main() -> int:
    try:
        arguments = parse_arguments()
        if arguments.session_child:
            run_session_child(arguments)
        else:
            run_parent(arguments)
    except DemoError as error:
        print(f"PD1 development demonstration failed closed: {error}", file=sys.stderr)
        return 2
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError, KeyError):
        print("PD1 development demonstration failed closed", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
