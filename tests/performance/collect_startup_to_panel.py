#!/usr/bin/env python3
"""Collect one event-driven session-startup to mapped-panel trial."""

from __future__ import annotations

import ctypes
import json
import os
import re
import select
import selectors
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO
from typing import Callable

from collector_common import (
    ClosedArgumentParser,
    CollectorError,
    environment_id,
    executable,
    positive_integer,
    redaction_contract,
    revision,
)


IN_CREATE = 0x00000100
IN_MOVED_TO = 0x00000080
IN_CLOSE_WRITE = 0x00000008
IN_ATTRIB = 0x00000004
IN_ONLYDIR = 0x01000000
IN_NONBLOCK = os.O_NONBLOCK
IN_CLOEXEC = os.O_CLOEXEC
WATCH_MASK = IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB
MAXIMUM_INOTIFY_BYTES = 64 * 1024
MAXIMUM_SESSION_DIRECTORIES = 1
MAXIMUM_DIAGNOSTIC_BYTES = 64 * 1024
MAXIMUM_X11_BUFFER_BYTES = 16 * 1024
MAXIMUM_SESSION_TASKS = 64
MAXIMUM_SESSION_CHILDREN = 16
MAXIMUM_CHILDREN_FILE_BYTES = 4096
MAXIMUM_XPROP_BYTES = 64 * 1024
MAXIMUM_MAPPED_CLIENTS = 256
OBSERVER_HANDSHAKE_SECONDS = 2
STARTUP_DEADLINE_SECONDS = 10
SESSION_SHUTDOWN_GRACE_SECONDS = 7
X11_OBSERVER_SHUTDOWN_GRACE_SECONDS = 1
SESSION_CLEAN_EXIT_CODES = (0, 7)
OBSERVER_READY_PROPERTY = "_PRISMDRAKE_PD1_OBSERVER_READY"
MAP_EVENT_PATTERN = re.compile(rb"(?:^|\n)MapNotify event,", re.MULTILINE)
CLIENT_LIST_EVENT_PATTERN = re.compile(
    rb"(?:^|\n)PropertyNotify event,[^\n]*\n[ \t]+atom [^\n]*"
    rb"\(_NET_CLIENT_LIST_STACKING\)",
    re.MULTILINE,
)
OBSERVER_READY_PATTERN = re.compile(
    rb"PropertyNotify.*?atom[^\n]*\(" + OBSERVER_READY_PROPERTY.encode("ascii") + rb"\)",
    re.DOTALL,
)
CHILD_RESTART_TOKEN = b" recovery=restart_component"
PROPERTY_PATTERN = re.compile(
    r"^(_NET_WM_WINDOW_TYPE|_NET_WM_STRUT|_NET_WM_STRUT_PARTIAL|_NET_WM_PID)"
    r"\((ATOM|CARDINAL)\) = (.*)$"
)
ATOM_PATTERN = re.compile(r"_NET_WM_[A-Z0-9_]+")
CLIENT_LIST_PATTERN = re.compile(
    r"^_NET_CLIENT_LIST_STACKING\(WINDOW\): window id #(?: (.*))?$"
)
WINDOW_ID_PATTERN = re.compile(r"0x[0-9a-f]+", re.IGNORECASE)
WM_STATE_HEADER = "WM_STATE(WM_STATE):"
WM_STATE_VALUE_PATTERN = re.compile(r"[ \t]+window state: (Withdrawn|Normal|Iconic)")
WM_STATE_ICON_PATTERN = re.compile(r"[ \t]+icon window: 0x[0-9a-f]+", re.IGNORECASE)


@dataclass(frozen=True)
class DockProperties:
    process_id: int
    strut: tuple[int, ...]
    partial_strut: tuple[int, ...]


@dataclass(frozen=True)
class LiveSession:
    session_pid: int
    settingsd_pid: int
    shell_pid: int
    runtime_directory: Path
    environment: dict[str, str]


def observer_ready_event(payload: bytes) -> bool:
    return OBSERVER_READY_PATTERN.search(payload) is not None


class InotifyRuntimeTree:
    def __init__(self, runtime_directory: Path) -> None:
        self._runtime_directory = runtime_directory
        self._libc = ctypes.CDLL(None, use_errno=True)
        self._libc.inotify_init1.argtypes = [ctypes.c_int]
        self._libc.inotify_init1.restype = ctypes.c_int
        self._libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        self._libc.inotify_add_watch.restype = ctypes.c_int
        self.fd = self._libc.inotify_init1(IN_NONBLOCK | IN_CLOEXEC)
        if self.fd < 0:
            raise CollectorError("inotify_unavailable")
        self._watched: set[Path] = set()
        self._add_watch(runtime_directory)

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def _add_watch(self, path: Path) -> None:
        if path in self._watched:
            return
        descriptor = self._libc.inotify_add_watch(
            self.fd, os.fsencode(path), WATCH_MASK | IN_ONLYDIR
        )
        if descriptor < 0:
            raise CollectorError("inotify_watch_failed")
        self._watched.add(path)

    def consume(self) -> None:
        try:
            payload = os.read(self.fd, MAXIMUM_INOTIFY_BYTES)
        except BlockingIOError:
            return
        offset = 0
        while offset < len(payload):
            if len(payload) - offset < 16:
                raise CollectorError("invalid_inotify_event")
            _watch, _mask, _cookie, name_length = struct.unpack_from("iIII", payload, offset)
            offset += 16 + name_length
            if offset > len(payload):
                raise CollectorError("invalid_inotify_event")

    def state(self) -> tuple[bool, bool]:
        root = self._runtime_directory / "prismdrake"
        if root.is_dir():
            self._add_watch(root)
        sessions = sorted(root.glob("session-*")) if root.is_dir() else []
        if len(sessions) > MAXIMUM_SESSION_DIRECTORIES:
            raise CollectorError("multiple_session_instances")
        if not sessions:
            return False, False
        session = sessions[0]
        if not session.is_dir() or session.is_symlink():
            raise CollectorError("invalid_session_instance")
        self._add_watch(session)
        safe_mode = session.joinpath("safe-mode").exists()
        ready = session.joinpath("ready").is_file()
        return ready, safe_mode


def parse_arguments() -> object:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument("--session", required=True, type=executable)
    parser.add_argument("--settingsd", required=True, type=executable)
    parser.add_argument("--shell", required=True, type=executable)
    parser.add_argument("--xev", required=True, type=executable)
    parser.add_argument("--xprop", required=True, type=executable)
    parser.add_argument("--stdbuf", required=True, type=executable)
    parser.add_argument("--revision", required=True, type=revision)
    parser.add_argument("--environment-id", required=True, type=environment_id)
    parser.add_argument("--timeout-seconds", default=str(STARTUP_DEADLINE_SECONDS))
    options = parser.parse_args()
    options.timeout_seconds = positive_integer(
        options.timeout_seconds, STARTUP_DEADLINE_SECONDS
    )
    if options.timeout_seconds != STARTUP_DEADLINE_SECONDS:
        raise CollectorError("invalid_timeout")
    return options


def _cardinals(value: str, expected_count: int) -> tuple[int, ...]:
    fields = value.split(", ")
    if len(fields) != expected_count or any(not field.isascii() or not field.isdigit() for field in fields):
        raise CollectorError("invalid_mapped_dock_contract")
    values = tuple(int(field, 10) for field in fields)
    if any(value > 2**32 - 1 for value in values):
        raise CollectorError("invalid_mapped_dock_contract")
    return values


def _wm_state(output: str) -> str | None:
    lines = output.splitlines()
    headers = [index for index, line in enumerate(lines) if line == WM_STATE_HEADER]
    if not headers:
        return None
    if len(headers) != 1 or headers[0] + 2 >= len(lines):
        raise CollectorError("invalid_mapped_dock_contract")
    value = WM_STATE_VALUE_PATTERN.fullmatch(lines[headers[0] + 1])
    icon = WM_STATE_ICON_PATTERN.fullmatch(lines[headers[0] + 2])
    if value is None or icon is None:
        raise CollectorError("invalid_mapped_dock_contract")
    return value.group(1)


def mapped_inventory_event(payload: bytes) -> bool:
    return (
        MAP_EVENT_PATTERN.search(payload) is not None
        or CLIENT_LIST_EVENT_PATTERN.search(payload) is not None
    )


def root_map_event(payload: bytes) -> bool:
    return MAP_EVENT_PATTERN.search(payload) is not None


def bounded_xprop_capture(
    command: list[str], environment: dict[str, str]
) -> tuple[int, str]:
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=environment,
    )
    if process.stdout is None:
        process.kill()
        process.wait(timeout=1)
        raise CollectorError("dock_property_read_failed")
    deadline = time.monotonic() + 1
    payload = bytearray()
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CollectorError("dock_property_read_unbounded")
            readable, _writable, _exceptional = select.select(
                [process.stdout], [], [], remaining
            )
            if not readable:
                raise CollectorError("dock_property_read_unbounded")
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                break
            payload.extend(chunk)
            if len(payload) > MAXIMUM_XPROP_BYTES:
                raise CollectorError("dock_property_output_too_large")
        process.wait(timeout=max(0.001, deadline - time.monotonic()))
    except CollectorError:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=1)
        process.stdout.close()
        raise
    except subprocess.TimeoutExpired as error:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=1)
        process.stdout.close()
        raise CollectorError("dock_property_read_unbounded") from error
    process.stdout.close()
    try:
        return process.returncode, payload.decode("ascii")
    except UnicodeDecodeError as error:
        raise CollectorError("dock_property_output_invalid") from error


def parse_dock_properties(output: str) -> DockProperties | None:
    properties: dict[str, tuple[str, str]] = {}
    for line in output.splitlines():
        match = PROPERTY_PATTERN.fullmatch(line)
        if match is None:
            continue
        name = match.group(1)
        if name in properties:
            raise CollectorError("invalid_mapped_dock_contract")
        properties[name] = (match.group(2), match.group(3))
    window_type = properties.get("_NET_WM_WINDOW_TYPE")
    if window_type is None:
        return None
    if window_type[0] != "ATOM":
        raise CollectorError("invalid_mapped_dock_contract")
    atoms = tuple(field.strip() for field in window_type[1].split(","))
    if any(ATOM_PATTERN.fullmatch(atom) is None for atom in atoms):
        raise CollectorError("invalid_mapped_dock_contract")
    if "_NET_WM_WINDOW_TYPE_DOCK" not in atoms:
        return None
    if atoms != ("_NET_WM_WINDOW_TYPE_DOCK",):
        raise CollectorError("invalid_mapped_dock_contract")
    if _wm_state(output) != "Normal":
        return None
    expected_types = {
        "_NET_WM_STRUT": "CARDINAL",
        "_NET_WM_STRUT_PARTIAL": "CARDINAL",
        "_NET_WM_PID": "CARDINAL",
    }
    if any(
        name not in properties or properties[name][0] != expected_type
        for name, expected_type in expected_types.items()
    ):
        raise CollectorError("invalid_mapped_dock_contract")
    strut = _cardinals(properties["_NET_WM_STRUT"][1], 4)
    partial = _cardinals(properties["_NET_WM_STRUT_PARTIAL"][1], 12)
    process_values = _cardinals(properties["_NET_WM_PID"][1], 1)
    if not any(strut) or partial[:4] != strut or process_values[0] <= 0:
        raise CollectorError("invalid_mapped_dock_contract")
    return DockProperties(process_values[0], strut, partial)


def inspect_mapped_dock(
    xprop: Path, environment: dict[str, str], window: str
) -> DockProperties | None:
    tool_environment = environment.copy()
    tool_environment["LANG"] = "C"
    tool_environment["LC_ALL"] = "C"
    returncode, output = bounded_xprop_capture(
        [
            str(xprop),
            "-id",
            window,
            "_NET_WM_WINDOW_TYPE",
            "_NET_WM_STRUT",
            "_NET_WM_STRUT_PARTIAL",
            "_NET_WM_PID",
            "WM_STATE",
        ],
        tool_environment,
    )
    if returncode != 0:
        raise CollectorError("dock_property_read_failed")
    return parse_dock_properties(output)


def enumerate_mapped_docks(
    xprop: Path, environment: dict[str, str]
) -> list[DockProperties]:
    tool_environment = environment.copy()
    tool_environment["LANG"] = "C"
    tool_environment["LC_ALL"] = "C"
    returncode, output = bounded_xprop_capture(
        [str(xprop), "-root", "_NET_CLIENT_LIST_STACKING"], tool_environment
    )
    if returncode != 0:
        raise CollectorError("mapped_client_inventory_failed")
    match = CLIENT_LIST_PATTERN.fullmatch(output.rstrip("\n"))
    if match is None:
        raise CollectorError("mapped_client_inventory_invalid")
    value = match.group(1)
    windows = [] if value is None else value.split(", ")
    if len(windows) > MAXIMUM_MAPPED_CLIENTS or any(
        WINDOW_ID_PATTERN.fullmatch(window) is None for window in windows
    ):
        raise CollectorError("mapped_client_inventory_invalid")
    docks: list[DockProperties] = []
    for window in windows:
        dock = inspect_mapped_dock(xprop, environment, window)
        if dock is not None:
            docks.append(dock)
    return docks


def validate_mapped_dock_inventory(
    mapped_docks: list[DockProperties], shell_pid: int
) -> None:
    if len(mapped_docks) != 1:
        raise CollectorError(
            "foreign_mapped_dock"
            if not mapped_docks
            or any(dock.process_id != shell_pid for dock in mapped_docks)
            else "duplicate_mapped_dock"
        )
    if mapped_docks[0].process_id != shell_pid:
        raise CollectorError("foreign_mapped_dock")


def bind_inventory_dock(
    session_pid: int,
    settingsd_executable: Path,
    shell_executable: Path,
    xprop: Path,
    environment: dict[str, str],
) -> tuple[int, int] | None:
    mapped_docks = enumerate_mapped_docks(xprop, environment)
    if not mapped_docks:
        return None
    if len(mapped_docks) != 1:
        _settingsd_pid, expected_shell_pid = supervised_component_pids(
            session_pid, settingsd_executable, shell_executable
        )
        validate_mapped_dock_inventory(mapped_docks, expected_shell_pid)
    dock = mapped_docks[0]
    descriptor = bind_dock_to_shell(session_pid, dock, shell_executable)
    return dock.process_id, descriptor


def pidfd_alive(descriptor: int) -> bool:
    poller = select.poll()
    poller.register(descriptor, select.POLLIN | select.POLLERR | select.POLLHUP)
    return not poller.poll(0)


def direct_child_pids(parent_pid: int) -> set[int]:
    task_root = Path("/proc") / str(parent_pid) / "task"
    try:
        tasks = sorted(task for task in task_root.iterdir() if task.name.isdigit())
    except OSError as error:
        raise CollectorError("session_child_ownership_unavailable") from error
    if not tasks or len(tasks) > MAXIMUM_SESSION_TASKS:
        raise CollectorError("session_child_ownership_unavailable")
    children: set[int] = set()
    try:
        for task in tasks:
            with task.joinpath("children").open("r", encoding="ascii") as stream:
                payload = stream.read(MAXIMUM_CHILDREN_FILE_BYTES + 1)
            if len(payload) > MAXIMUM_CHILDREN_FILE_BYTES:
                raise CollectorError("session_child_ownership_unavailable")
            for field in payload.split():
                if not field.isascii() or not field.isdigit():
                    raise CollectorError("session_child_ownership_unavailable")
                children.add(int(field, 10))
    except (OSError, UnicodeError) as error:
        raise CollectorError("session_child_ownership_unavailable") from error
    if len(children) > MAXIMUM_SESSION_CHILDREN:
        raise CollectorError("session_child_ownership_unavailable")
    return children


def bind_dock_to_shell(
    session_pid: int, dock: DockProperties, expected_shell: Path
) -> int:
    children = direct_child_pids(session_pid)
    shell_children: list[int] = []
    for child in children:
        try:
            if os.path.samefile(Path("/proc") / str(child) / "exe", expected_shell):
                shell_children.append(child)
        except OSError:
            continue
    if shell_children != [dock.process_id]:
        raise CollectorError("mapped_dock_not_supervised_shell")
    try:
        descriptor = os.pidfd_open(dock.process_id, 0)
    except OSError as error:
        raise CollectorError("mapped_dock_shell_identity_unavailable") from error
    try:
        if (
            not pidfd_alive(descriptor)
            or dock.process_id not in direct_child_pids(session_pid)
            or not os.path.samefile(
                Path("/proc") / str(dock.process_id) / "exe", expected_shell
            )
        ):
            raise CollectorError("mapped_dock_not_supervised_shell")
    except (OSError, CollectorError):
        os.close(descriptor)
        raise
    return descriptor


def supervised_component_pids(
    session_pid: int, expected_settingsd: Path, expected_shell: Path
) -> tuple[int, int]:
    children = direct_child_pids(session_pid)
    matches: dict[Path, list[int]] = {expected_settingsd: [], expected_shell: []}
    for child in children:
        for expected in matches:
            try:
                if os.path.samefile(Path("/proc") / str(child) / "exe", expected):
                    matches[expected].append(child)
            except OSError:
                continue
    settingsd = matches[expected_settingsd]
    shell = matches[expected_shell]
    if len(settingsd) != 1 or len(shell) != 1 or children != {settingsd[0], shell[0]}:
        raise CollectorError("supervised_component_tree_invalid")
    return settingsd[0], shell[0]


def verify_dock_shell_identity(
    session_pid: int, shell_pid: int, expected_shell: Path, descriptor: int
) -> None:
    try:
        executable_matches = os.path.samefile(
            Path("/proc") / str(shell_pid) / "exe", expected_shell
        )
    except OSError as error:
        raise CollectorError("mapped_dock_shell_identity_changed") from error
    if (
        not pidfd_alive(descriptor)
        or shell_pid not in direct_child_pids(session_pid)
        or not executable_matches
    ):
        raise CollectorError("mapped_dock_shell_identity_changed")


def _change_root_probe(
    xprop: Path, environment: dict[str, str], value: str | None
) -> None:
    command = [str(xprop), "-root"]
    if value is None:
        command.extend(["-remove", OBSERVER_READY_PROPERTY])
    else:
        command.extend(
            ["-f", OBSERVER_READY_PROPERTY, "8s", "-set", OBSERVER_READY_PROPERTY, value]
        )
    completed = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=environment,
        timeout=1,
        check=False,
    )
    if completed.returncode != 0:
        raise CollectorError("x11_observer_handshake_failed")


def wait_for_x11_observer(
    xprop: Path,
    environment: dict[str, str],
    xev: subprocess.Popen[bytes],
    selector: selectors.BaseSelector,
    observer: InotifyRuntimeTree,
) -> None:
    if xev.stdout is None:
        raise CollectorError("x11_observer_failed")
    deadline = time.monotonic() + OBSERVER_HANDSHAKE_SECONDS
    x11_buffer = b""
    attempt = 0
    while time.monotonic() < deadline:
        if xev.poll() is not None:
            raise CollectorError("x11_observer_exited")
        _change_root_probe(xprop, environment, str(attempt % 2))
        attempt += 1
        attempt_deadline = min(deadline, time.monotonic() + 0.25)
        while time.monotonic() < attempt_deadline:
            events = selector.select(attempt_deadline - time.monotonic())
            if not events:
                break
            for key, _mask in events:
                if key.data == "inotify":
                    observer.consume()
                    continue
                if key.data != "x11":
                    continue
                chunk = os.read(xev.stdout.fileno(), 4096)
                if not chunk:
                    raise CollectorError("x11_observer_exited")
                x11_buffer = (x11_buffer + chunk)[-MAXIMUM_X11_BUFFER_BYTES:]
                if observer_ready_event(x11_buffer):
                    _change_root_probe(xprop, environment, None)
                    return
    raise CollectorError("x11_observer_handshake_failed")


def session_diagnostic_state(stream: BinaryIO, byte_count: int) -> tuple[bool, bool]:
    if byte_count > MAXIMUM_DIAGNOSTIC_BYTES:
        raise CollectorError("session_diagnostics_too_large")
    stream.seek(0)
    payload = stream.read(byte_count)
    if len(payload) != byte_count:
        raise CollectorError("session_diagnostics_read_failed")
    return CHILD_RESTART_TOKEN in payload, bool(payload)


def stop_process(
    process: subprocess.Popen[object], timeout: int, unbounded_identifier: str
) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        process.kill()
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired as kill_error:
            raise CollectorError(unbounded_identifier) from kill_error
        raise CollectorError(unbounded_identifier) from error


def collect(
    options: object,
    endpoint_callback: Callable[[LiveSession], object] | None = None,
) -> dict[str, object] | tuple[dict[str, object], object]:
    if "DISPLAY" not in os.environ or "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
        raise CollectorError("isolated_display_or_bus_missing")
    with tempfile.TemporaryDirectory(prefix="prismdrake-startup-evidence-") as temporary:
        runtime = Path(temporary)
        runtime.chmod(0o700)
        observer = InotifyRuntimeTree(runtime)
        selector = selectors.DefaultSelector()
        selector.register(observer.fd, selectors.EVENT_READ, "inotify")
        environment = os.environ.copy()
        environment["XDG_RUNTIME_DIR"] = str(runtime)
        environment["LANG"] = "C.UTF-8"
        environment["LC_ALL"] = "C.UTF-8"
        xev: subprocess.Popen[bytes] | None = None
        session: subprocess.Popen[bytes] | None = None
        session_pidfd: int | None = None
        shell_pidfd: int | None = None
        dock_shell_pid: int | None = None
        callback_result: object | None = None
        try:
            xev = subprocess.Popen(
                [
                    str(options.stdbuf),
                    "-oL",
                    "--",
                    str(options.xev),
                    "-root",
                    "-event",
                    "substructure",
                    "-event",
                    "property",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=environment,
            )
            if xev.stdout is None:
                raise CollectorError("x11_observer_failed")
            selector.register(xev.stdout, selectors.EVENT_READ, "x11")
            wait_for_x11_observer(options.xprop, environment, xev, selector, observer)
            with tempfile.TemporaryFile() as session_diagnostics:
                started_ns = time.monotonic_ns()
                session = subprocess.Popen(
                    [
                        str(options.session),
                        "--settingsd",
                        str(options.settingsd),
                        "--shell",
                        str(options.shell),
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=session_diagnostics,
                    env=environment,
                )
                session_pidfd = os.pidfd_open(session.pid, 0)
                selector.register(session_pidfd, selectors.EVENT_READ, "session")
                deadline_ns = started_ns + options.timeout_seconds * 1_000_000_000
                ready_ns: int | None = None
                mapped_ns: int | None = None
                xev_buffer = b""
                root_map_observed = False
                while ready_ns is None or mapped_ns is None:
                    remaining_ns = deadline_ns - time.monotonic_ns()
                    if remaining_ns <= 0:
                        raise CollectorError("startup_deadline_exceeded")
                    events = selector.select(remaining_ns / 1_000_000_000)
                    if not events:
                        raise CollectorError("startup_deadline_exceeded")
                    for key, _mask in events:
                        if key.data == "session":
                            raise CollectorError("session_exited_before_endpoint")
                        if key.data == "shell":
                            raise CollectorError("mapped_dock_shell_identity_changed")
                        if key.data == "inotify":
                            observer.consume()
                            ready, safe_mode = observer.state()
                            if safe_mode:
                                raise CollectorError("safe_mode_activated")
                            if ready and ready_ns is None:
                                ready_ns = time.monotonic_ns()
                        elif key.data == "x11":
                            chunk = os.read(xev.stdout.fileno(), 4096)
                            if not chunk:
                                raise CollectorError("x11_observer_exited")
                            xev_buffer = (xev_buffer + chunk)[-MAXIMUM_X11_BUFFER_BYTES:]
                            if not mapped_inventory_event(xev_buffer):
                                continue
                            root_map_observed = root_map_observed or root_map_event(
                                xev_buffer
                            )
                            xev_buffer = b""
                            if not root_map_observed:
                                continue
                            if mapped_ns is not None:
                                validate_mapped_dock_inventory(
                                    enumerate_mapped_docks(options.xprop, environment),
                                    dock_shell_pid,
                                )
                                continue
                            binding = bind_inventory_dock(
                                session.pid,
                                options.settingsd,
                                options.shell,
                                options.xprop,
                                environment,
                            )
                            if binding is None:
                                continue
                            dock_shell_pid, shell_pidfd = binding
                            selector.register(shell_pidfd, selectors.EVENT_READ, "shell")
                            mapped_ns = time.monotonic_ns()
                endpoint_ns = max(ready_ns, mapped_ns)
                if shell_pidfd is None or dock_shell_pid is None:
                    raise CollectorError("mapped_dock_not_supervised_shell")
                verify_dock_shell_identity(
                    session.pid, dock_shell_pid, options.shell, shell_pidfd
                )
                mapped_docks = enumerate_mapped_docks(options.xprop, environment)
                validate_mapped_dock_inventory(mapped_docks, dock_shell_pid)
                settingsd_pid, shell_pid = supervised_component_pids(
                    session.pid, options.settingsd, options.shell
                )
                if shell_pid != dock_shell_pid:
                    raise CollectorError("mapped_dock_not_supervised_shell")
                if endpoint_callback is not None:
                    callback_result = endpoint_callback(
                        LiveSession(
                            session.pid,
                            settingsd_pid,
                            shell_pid,
                            runtime,
                            environment.copy(),
                        )
                    )
                session_diagnostics.flush()
                child_restart, diagnostics_present = session_diagnostic_state(
                    session_diagnostics, session_diagnostics.tell()
                )
                if child_restart:
                    raise CollectorError("child_restart_observed")
                if diagnostics_present:
                    raise CollectorError("session_diagnostics_present")
        finally:
            cleanup_error: CollectorError | None = None
            if shell_pidfd is not None:
                try:
                    selector.unregister(shell_pidfd)
                    os.close(shell_pidfd)
                except (KeyError, OSError):
                    cleanup_error = CollectorError("startup_cleanup_failed")
            if session_pidfd is not None:
                try:
                    selector.unregister(session_pidfd)
                    os.close(session_pidfd)
                except (KeyError, OSError):
                    cleanup_error = CollectorError("startup_cleanup_failed")
            if session is not None:
                try:
                    stop_process(
                        session,
                        SESSION_SHUTDOWN_GRACE_SECONDS,
                        "session_shutdown_unbounded",
                    )
                except CollectorError as error:
                    cleanup_error = error
            if xev is not None:
                try:
                    stop_process(
                        xev,
                        X11_OBSERVER_SHUTDOWN_GRACE_SECONDS,
                        "x11_observer_shutdown_unbounded",
                    )
                except CollectorError as error:
                    cleanup_error = cleanup_error or error
            selector.close()
            observer.close()
            if cleanup_error is not None:
                raise cleanup_error
        if session is None or session.returncode not in SESSION_CLEAN_EXIT_CODES:
            raise CollectorError("session_shutdown_failed")
    artifact = {
        "schema_version": 1,
        "evidence_kind": "startup_to_mapped_panel",
        "release_budget": False,
        "source_revision": options.revision,
        "reference_environment_id": options.environment_id,
        "method": {
            "clock": "python_monotonic_ns",
            "deadline_ns": options.timeout_seconds * 1_000_000_000,
            "fresh_private_runtime": True,
            "x11_observation": "root_map_notify_with_ewmh_client_resolution",
            "filesystem_observation": "inotify",
            "x11_observer_readiness": "root_property_notify_handshake_before_timestamp",
            "child_restart_observation": "structured_session_diagnostics_until_endpoint",
            "mapped_dock_ownership": "net_wm_pid_exact_direct_shell_child_with_pidfd",
            "mapped_dock_inventory": "post_endpoint_net_client_list_stacking_barrier",
        },
        "result": {
            "duration_ns": endpoint_ns - started_ns,
            "ready_marker_ns": ready_ns - started_ns,
            "mapped_dock_ns": mapped_ns - started_ns,
            "safe_mode": False,
            "child_restart_observed": False,
            "mapped_dock_count": 1,
            "foreign_dock_observed": False,
            "duplicate_dock_observed": False,
        },
        "limitations": [
            "mapped_and_standards_valid_on_xvfb_not_a_reviewed_physical_pixel",
            "caller_must_create_a_fresh_xvfb_openbox_and_dbus_session_per_document",
        ],
        "redaction": redaction_contract(),
    }
    if endpoint_callback is not None:
        if callback_result is None:
            raise CollectorError("live_endpoint_callback_failed")
        return artifact, callback_result
    return artifact


def main() -> int:
    try:
        options = parse_arguments()
        print(json.dumps(collect(options), indent=2, sort_keys=True))
        return 0
    except (CollectorError, OSError, subprocess.SubprocessError) as error:
        identifier = str(error) if isinstance(error, CollectorError) else "external_operation_failed"
        print(f"collect_startup_to_panel: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
