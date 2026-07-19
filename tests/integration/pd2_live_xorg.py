#!/usr/bin/env python3
"""Live Xorg-dummy adapter for the PD2 installed demonstration contract.

This module owns only observations it can make directly through Xorg, RandR,
and the reference EWMH window manager.  Every shell, service, and accessibility
predicate remains unsupported until a live observer is added; unsupported
steps fail closed and therefore cannot produce PD2 completion evidence.
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import signal
import stat
import subprocess
import tempfile
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from pd2_demo import EXECUTABLE_NAMES, HarnessError, Observation


MAX_COMMAND_OUTPUT_BYTES = 65_536
MAX_EXECUTABLE_BYTES = 128 * 1024 * 1024
MAX_PROCESS_SCAN_ENTRIES = 32_768
MAX_PROCESS_ANCESTRY_DEPTH = 64
MAX_SURFACE_WINDOWS = 64
OUTPUT_MODE = "1600x900"
OUTPUT_WIDTH = 1600
OUTPUT_HEIGHT = 900
PANEL_HEIGHT = 48
PRIMARY_OUTPUT = "DUMMY0"
SECONDARY_OUTPUT = "DUMMY1"


@dataclass(frozen=True)
class CommandResult:
    """Bounded command result used by the adapter."""

    returncode: int
    stdout: bytes = b""
    stderr: bytes = b""


@dataclass(frozen=True)
class ProcessIdentity:
    """Ephemeral identity used only to prove distinct live process owners."""

    pid: int
    start_time: int


class ChildProcess(Protocol):
    """Minimal exact-child interface needed for bounded cleanup."""

    pid: int

    def poll(self) -> int | None: ...

    def wait(self, timeout: float | None = None) -> int: ...


class Operations(Protocol):
    """Injectable process boundary; production uses real subprocesses."""

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> ChildProcess: ...

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult: ...

    def stop(self, child: ChildProcess, timeout: float) -> bool: ...

    def display_available(self, number: int) -> bool: ...

    def identify(
        self, child: ChildProcess, executable: Path
    ) -> ProcessIdentity | None: ...

    def descendants(
        self, child: ChildProcess, executable: Path
    ) -> Sequence[ProcessIdentity]: ...

    def monotonic(self) -> float: ...

    def pause(self, duration: float) -> None: ...


class SystemOperations:
    """Real bounded process operations for a private live demonstration run."""

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> ChildProcess:
        try:
            return subprocess.Popen(  # noqa: S603 - exact reviewed argv, no shell
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                close_fds=True,
            )
        except OSError as error:
            raise HarnessError("live_process_start_failed") from error

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult:
        try:
            completed = subprocess.run(  # noqa: S603 - exact reviewed argv, no shell
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise HarnessError("live_command_timeout") from error
        except OSError as error:
            raise HarnessError("live_command_failed") from error
        if len(completed.stdout) + len(completed.stderr) > MAX_COMMAND_OUTPUT_BYTES:
            raise HarnessError("live_command_output_oversized")
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)

    def stop(self, child: ChildProcess, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        group = child.pid
        try:
            if child.poll() is None:
                group = os.getpgid(child.pid)
            if self._process_group_alive(group):
                os.killpg(group, signal.SIGTERM)
                try:
                    child.wait(timeout=max(timeout / 2.0, 0.01))
                except subprocess.TimeoutExpired:
                    pass
        except OSError:
            return child.poll() is not None
        if not self._process_group_alive(group):
            return child.poll() is not None
        try:
            os.killpg(group, signal.SIGKILL)
            if child.poll() is None:
                child.wait(timeout=max(deadline - time.monotonic(), 0.01))
        except (OSError, subprocess.TimeoutExpired):
            pass
        while time.monotonic() < deadline:
            if not self._process_group_alive(group):
                return child.poll() is not None
            time.sleep(0.01)
        return child.poll() is not None and not self._process_group_alive(group)

    @staticmethod
    def _process_group_alive(group: int) -> bool:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def display_available(self, number: int) -> bool:
        return not Path(f"/tmp/.X{number}-lock").exists() and not Path(
            f"/tmp/.X11-unix/X{number}"
        ).exists()

    def identify(
        self, child: ChildProcess, executable: Path
    ) -> ProcessIdentity | None:
        if child.poll() is not None:
            return None
        record = self._process_record(child.pid)
        if record is None or not self._executable_matches(child.pid, executable):
            return None
        _parent, start_time = record
        return ProcessIdentity(child.pid, start_time)

    def descendants(
        self, child: ChildProcess, executable: Path
    ) -> Sequence[ProcessIdentity]:
        if child.poll() is not None:
            return ()
        records: dict[int, tuple[int, int]] = {}
        try:
            entries = tuple(Path("/proc").iterdir())
        except OSError:
            return ()
        if len(entries) > MAX_PROCESS_SCAN_ENTRIES:
            return ()
        for entry in entries:
            if not entry.name.isascii() or not entry.name.isdecimal():
                continue
            pid = int(entry.name)
            record = self._process_record(pid)
            if record is not None:
                records[pid] = record

        matches: list[ProcessIdentity] = []
        for pid, (_parent, start_time) in records.items():
            if pid == child.pid or not self._is_descendant(pid, child.pid, records):
                continue
            if self._executable_matches(pid, executable):
                matches.append(ProcessIdentity(pid, start_time))
        return tuple(sorted(matches, key=lambda identity: identity.pid))

    @staticmethod
    def _process_record(pid: int) -> tuple[int, int] | None:
        try:
            payload = Path(f"/proc/{pid}/stat").read_bytes()
        except OSError:
            return None
        closing = payload.rfind(b")")
        if closing < 1 or closing + 2 >= len(payload):
            return None
        fields = payload[closing + 2 :].split()
        if len(fields) <= 19:
            return None
        try:
            parent = int(fields[1])
            start_time = int(fields[19])
        except ValueError:
            return None
        if parent < 0 or start_time <= 0:
            return None
        return parent, start_time

    @staticmethod
    def _is_descendant(
        pid: int, ancestor: int, records: Mapping[int, tuple[int, int]]
    ) -> bool:
        visited: set[int] = set()
        current = pid
        for _depth in range(MAX_PROCESS_ANCESTRY_DEPTH):
            record = records.get(current)
            if record is None:
                return False
            parent = record[0]
            if parent == ancestor:
                return True
            if parent <= 1 or parent in visited:
                return False
            visited.add(parent)
            current = parent
        return False

    @staticmethod
    def _executable_matches(pid: int, executable: Path) -> bool:
        try:
            expected = executable.stat()
            observed = Path(f"/proc/{pid}/exe").stat()
        except OSError:
            return False
        return (expected.st_dev, expected.st_ino) == (observed.st_dev, observed.st_ino)

    def monotonic(self) -> float:
        return time.monotonic()

    def pause(self, duration: float) -> None:
        time.sleep(duration)


@dataclass(frozen=True)
class XrandrOutput:
    """One strictly parsed RandR output line."""

    name: str
    connected: bool
    primary: bool
    width: int | None
    height: int | None
    x: int | None
    y: int | None

    @property
    def active(self) -> bool:
        return self.connected and self.width is not None


@dataclass(frozen=True)
class WindowGeometry:
    """Bounded mapped X11 window geometry from ``xwininfo``."""

    x: int
    y: int
    width: int
    height: int


_OUTPUT_LINE = re.compile(
    rb"^(?P<name>[A-Za-z0-9_.:-]{1,128}) "
    rb"(?P<state>connected|disconnected)"
    rb"(?P<primary> primary)?"
    rb"(?: (?P<width>[1-9][0-9]{0,5})x(?P<height>[1-9][0-9]{0,5})"
    rb"\+(?P<x>[0-9]{1,6})\+(?P<y>[0-9]{1,6}))?"
    rb"(?: |$)"
)


def parse_xrandr_outputs(payload: bytes) -> tuple[XrandrOutput, ...]:
    """Parse bounded ``xrandr --query`` output without accepting ambiguity."""

    if not payload or len(payload) > MAX_COMMAND_OUTPUT_BYTES or b"\x00" in payload:
        raise HarnessError("xrandr_output_invalid")
    outputs: list[XrandrOutput] = []
    names: set[str] = set()
    for line in payload.splitlines():
        match = _OUTPUT_LINE.match(line)
        if match is None:
            continue
        try:
            name = match.group("name").decode("ascii")
        except UnicodeDecodeError as error:
            raise HarnessError("xrandr_output_invalid") from error
        if name in names:
            raise HarnessError("xrandr_output_duplicate")
        names.add(name)
        values = {
            key: int(match.group(key)) if match.group(key) is not None else None
            for key in ("width", "height", "x", "y")
        }
        connected = match.group("state") == b"connected"
        if not connected and any(value is not None for value in values.values()):
            raise HarnessError("xrandr_output_invalid")
        outputs.append(
            XrandrOutput(
                name=name,
                connected=connected,
                primary=match.group("primary") is not None,
                width=values["width"],
                height=values["height"],
                x=values["x"],
                y=values["y"],
            )
        )
    if not outputs:
        raise HarnessError("xrandr_output_invalid")
    return tuple(outputs)


def parse_window_ids(payload: bytes, property_name: bytes) -> tuple[int, ...]:
    """Parse one bounded X11 WINDOW-list property without retaining names."""

    if (
        not payload
        or len(payload) > MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or re.fullmatch(rb"_[A-Z0-9_]{1,80}", property_name) is None
    ):
        raise HarnessError("x11_window_list_invalid")
    prefix = property_name + b"(WINDOW): window id # "
    if not payload.startswith(prefix) or not payload.endswith(b"\n"):
        raise HarnessError("x11_window_list_invalid")
    values = payload[len(prefix) : -1].split(b", ")
    if not values or len(values) > MAX_SURFACE_WINDOWS:
        raise HarnessError("x11_window_list_invalid")
    result: list[int] = []
    for value in values:
        if re.fullmatch(rb"0x[0-9a-f]{1,8}", value) is None:
            raise HarnessError("x11_window_list_invalid")
        parsed = int(value, 16)
        if parsed == 0 or parsed in result:
            raise HarnessError("x11_window_list_invalid")
        result.append(parsed)
    return tuple(result)


def parse_cardinals(payload: bytes, property_name: bytes, count: int) -> tuple[int, ...]:
    """Parse one exact bounded CARDINAL property."""

    if (
        not payload
        or len(payload) > MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or count < 1
        or count > 16
        or re.fullmatch(rb"_[A-Z0-9_]{1,80}", property_name) is None
    ):
        raise HarnessError("x11_cardinal_property_invalid")
    prefix = property_name + b"(CARDINAL) = "
    if not payload.startswith(prefix) or not payload.endswith(b"\n"):
        raise HarnessError("x11_cardinal_property_invalid")
    values = payload[len(prefix) : -1].split(b", ")
    if len(values) != count:
        raise HarnessError("x11_cardinal_property_invalid")
    result: list[int] = []
    for value in values:
        if re.fullmatch(rb"[0-9]{1,10}", value) is None:
            raise HarnessError("x11_cardinal_property_invalid")
        parsed = int(value)
        if parsed > 0xFFFF_FFFF:
            raise HarnessError("x11_cardinal_property_invalid")
        result.append(parsed)
    return tuple(result)


def parse_window_geometry(payload: bytes) -> WindowGeometry:
    """Extract geometry and mapping state without retaining a window title."""

    if not payload or len(payload) > MAX_COMMAND_OUTPUT_BYTES or b"\x00" in payload:
        raise HarnessError("x11_window_geometry_invalid")
    patterns = {
        "x": rb"^  Absolute upper-left X:\s+(-?[0-9]{1,7})$",
        "y": rb"^  Absolute upper-left Y:\s+(-?[0-9]{1,7})$",
        "width": rb"^  Width:\s+([1-9][0-9]{0,6})$",
        "height": rb"^  Height:\s+([1-9][0-9]{0,6})$",
        "border": rb"^  Border width:\s+([0-9]{1,6})$",
    }
    values: dict[str, int] = {}
    lines = payload.splitlines()
    for key, pattern in patterns.items():
        matches = [re.fullmatch(pattern, line) for line in lines]
        captures = [match.group(1) for match in matches if match is not None]
        if len(captures) != 1:
            raise HarnessError("x11_window_geometry_invalid")
        values[key] = int(captures[0])
    if (
        values["border"] != 0
        or lines.count(b"  Map State: IsViewable") != 1
        or lines.count(b"  Override Redirect State: no") != 1
    ):
        raise HarnessError("x11_window_geometry_invalid")
    return WindowGeometry(values["x"], values["y"], values["width"], values["height"])


def _session_config() -> str:
    """Return the complete private PD2 demonstration configuration."""

    return """schema_version = 2
profile = "lustre"

[appearance]
accent = "#4D7FFF"
transparency_enabled = true
blur_quality = "balanced"
reduced_motion = false
high_contrast = false
text_scale = 1.0
cursor_theme = "default"
cursor_size_px = 24
icon_theme = "hicolor"

[panel]
edge = "bottom"
size_px = 48
autohide = "never"
grouping = "when_full"
clock_format = "locale"
output_policy = "all"
outputs = []

[launcher]
layout = "expanded"
search_providers = ["applications", "settings"]
recent_items = "local_only"

[notifications]
enabled = true
history = "session"
default_timeout_ms = 5000
do_not_disturb = false

[desktop]
wallpaper_source = ""
background_color = "#182033"
wallpaper_mode = "fill"
icons_enabled = false

[integration]
export_gtk = true
export_qt = true
export_xsettings = true
export_portal = true

[accessibility]
focus_emphasis = "standard"
animation_scale = 1.0
minimum_target_size_px = 40

[keyboard]
menu_key_opens_launcher = true
focus_wraps = false

[developer]
diagnostics_enabled = false
mock_capability_overrides = []
"""


@dataclass(frozen=True)
class LiveXorgConfig:
    """Exact executable inputs for a private Xorg-dummy demonstration run."""

    xorg: Path
    xrandr: Path
    openbox: Path
    xprop: Path
    xwininfo: Path
    dbus_run_session: Path
    dbus_daemon: Path
    installed_executables: Mapping[str, Path]
    first_display: int = 120
    last_display: int = 159
    readiness_timeout: float = 8.0
    stop_timeout: float = 3.0

    def __post_init__(self) -> None:
        if not (1 <= self.first_display <= self.last_display <= 999):
            raise HarnessError("live_display_range_invalid")
        if self.readiness_timeout <= 0 or self.stop_timeout <= 0:
            raise HarnessError("live_timeout_invalid")


def _xorg_config() -> str:
    """Return the private deterministic two-output dummy-server configuration."""

    return """Section \"ServerFlags\"
    Option \"AutoAddDevices\" \"false\"
    Option \"AutoEnableDevices\" \"false\"
EndSection

Section \"Device\"
    Identifier \"PrismdrakeDummyDevice\"
    Driver \"dummy\"
    VideoRam 256000
EndSection

Section \"Monitor\"
    Identifier \"PrismdrakeDummyMonitor\"
    HorizSync 5.0-1000.0
    VertRefresh 5.0-200.0
    Modeline \"1600x900\" 118.25 1600 1696 1856 2112 900 903 908 934 -HSync +VSync
EndSection

Section \"Screen\"
    Identifier \"PrismdrakeDummyScreen\"
    Device \"PrismdrakeDummyDevice\"
    Monitor \"PrismdrakeDummyMonitor\"
    DefaultDepth 24
    SubSection \"Display\"
        Depth 24
        Modes \"1600x900\"
        Virtual 3200 1800
    EndSubSection
EndSection

Section \"ServerLayout\"
    Identifier \"PrismdrakeDummyLayout\"
    Screen \"PrismdrakeDummyScreen\"
EndSection
"""


class LiveXorgHarnessPort:
    """Fail-closed live adapter for the X11-owned subset of the PD2 contract."""

    _SUPPORTED_STEPS = {
        ("installed_workflow", "start_isolated_two_output_x11"),
        ("installed_workflow", "start_installed_prismdrake_session"),
        ("installed_workflow", "verify_component_readiness"),
        ("installed_workflow", "verify_desktop_wallpaper_both_outputs"),
        ("installed_workflow", "verify_panel_policy_both_outputs"),
        ("failure_replay", "two_same_scale_outputs_at_startup"),
        ("failure_replay", "add_second_output_after_startup"),
        ("failure_replay", "remove_primary_output"),
        ("failure_replay", "select_new_primary_output"),
        ("failure_replay", "change_output_geometry"),
        ("failure_replay", "replace_or_restart_window_manager"),
        ("failure_replay", "lose_x_server"),
    }

    def __init__(
        self, config: LiveXorgConfig, operations: Operations | None = None
    ) -> None:
        self._config = config
        self._operations = operations or SystemOperations()
        self._runtime: Path | None = None
        self._display: str | None = None
        self._xorg: ChildProcess | None = None
        self._openbox: ChildProcess | None = None
        self._session: ChildProcess | None = None
        self._desktop_surfaces: dict[int, WindowGeometry] | None = None
        self._surface_shell_identity: ProcessIdentity | None = None

    def installed_executable_hashes(self) -> Mapping[str, str]:
        if tuple(self._config.installed_executables.keys()) != EXECUTABLE_NAMES:
            raise HarnessError("installed_executable_set_invalid")
        return {
            name: self._hash_installed_executable(
                self._config.installed_executables[name], name
            )
            for name in EXECUTABLE_NAMES
        }

    def run_step(self, phase: str, identifier: str) -> Observation:
        if (phase, identifier) not in self._SUPPORTED_STEPS:
            raise HarnessError("live_step_not_implemented")
        if phase == "failure_replay":
            # Each replay scenario begins from a fresh isolated X11 epoch.  The
            # contract invokes scenarios consecutively and cleans only once at
            # the phase boundary, so carrying one scenario's topology into the
            # next would make the observation ambiguous.
            self.cleanup("failure_scenario_reset")
        if identifier == "start_isolated_two_output_x11":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
        elif identifier == "start_installed_prismdrake_session":
            self._start_session()
        elif identifier == "verify_component_readiness":
            self._wait_for_session_readiness()
            self.capture_process_ownership()
        elif identifier == "verify_desktop_wallpaper_both_outputs":
            self._require_desktop_wallpaper_surfaces()
        elif identifier == "verify_panel_policy_both_outputs":
            self._require_panel_surfaces()
        elif identifier == "two_same_scale_outputs_at_startup":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
        elif identifier == "add_second_output_after_startup":
            self._start_environment(two_outputs=False)
            self._require_topology(two_outputs=False)
            self._enable_secondary()
            self._require_topology(two_outputs=True)
        elif identifier == "remove_primary_output":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            self._xrandr("--output", PRIMARY_OUTPUT, "--off")
            self._require_only_secondary()
        elif identifier == "select_new_primary_output":
            self._start_environment(two_outputs=True)
            self._xrandr("--output", SECONDARY_OUTPUT, "--primary")
            self._require_topology(two_outputs=True, primary=SECONDARY_OUTPUT)
        elif identifier == "change_output_geometry":
            self._start_environment(two_outputs=True)
            self._xrandr("--output", SECONDARY_OUTPUT, "--pos", "0x900")
            self._require_topology(two_outputs=True, secondary_position=(0, 900))
        elif identifier == "replace_or_restart_window_manager":
            self._start_environment(two_outputs=True)
            if self._openbox is None or not self._operations.stop(
                self._openbox, self._config.stop_timeout
            ):
                raise HarnessError("window_manager_stop_failed")
            previous = self._openbox
            self._openbox = None
            self._start_openbox()
            if self._openbox is previous:
                raise HarnessError("window_manager_replacement_invalid")
        elif identifier == "lose_x_server":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            if self._xorg is None or not self._operations.stop(
                self._xorg, self._config.stop_timeout
            ):
                raise HarnessError("xorg_stop_failed")
            self._xorg = None
            result = self._run_x11((str(self._config.xrandr), "--query"))
            if result.returncode == 0:
                raise HarnessError("x_server_loss_unobserved")
        return Observation(True, True)

    def capture_process_ownership(self) -> Mapping[str, object]:
        if self._xorg is None or self._openbox is None or self._session is None:
            raise HarnessError("live_process_ownership_unavailable")
        session_executable = self._config.installed_executables.get(
            "prismdrake-session"
        )
        if session_executable is None:
            raise HarnessError("live_process_ownership_invalid")
        xorg = self._operations.identify(self._xorg, self._config.xorg)
        openbox = self._operations.identify(self._openbox, self._config.openbox)
        buses = self._operations.descendants(self._session, self._config.dbus_daemon)
        sessions = self._operations.descendants(self._session, session_executable)
        if xorg is None or openbox is None or len(buses) != 1 or len(sessions) != 1:
            raise HarnessError("live_process_ownership_invalid")
        identities = (xorg, openbox, buses[0], sessions[0])
        if len(set(identities)) != len(identities):
            raise HarnessError("live_process_ownership_invalid")
        return dict(
            zip(
                (
                    "xorg_dummy_server",
                    "openbox_window_manager",
                    "isolated_session_bus",
                    "prismdrake_session",
                ),
                identities,
                strict=True,
            )
        )

    def cleanup(self, phase: str) -> Observation:
        del phase
        clean = True
        for attribute in ("_session", "_openbox", "_xorg"):
            child = getattr(self, attribute)
            if child is not None:
                clean = self._operations.stop(child, self._config.stop_timeout) and clean
                setattr(self, attribute, None)
        self._display = None
        self._desktop_surfaces = None
        self._surface_shell_identity = None
        if self._runtime is not None:
            try:
                shutil.rmtree(self._runtime)
            except OSError:
                clean = False
            self._runtime = None
        if not clean:
            raise HarnessError("live_cleanup_failed")
        return Observation(True, True)

    @staticmethod
    def _hash_installed_executable(path: Path, expected_name: str) -> str:
        try:
            metadata = path.lstat()
        except OSError as error:
            raise HarnessError("installed_executable_unavailable") from error
        if (
            not path.is_absolute()
            or path.name != expected_name
            or stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_size <= 0
            or metadata.st_size > MAX_EXECUTABLE_BYTES
            or metadata.st_mode & 0o111 == 0
        ):
            raise HarnessError("installed_executable_invalid")
        digest = hashlib.sha256()
        try:
            with path.open("rb") as executable:
                while block := executable.read(1024 * 1024):
                    digest.update(block)
        except OSError as error:
            raise HarnessError("installed_executable_read_failed") from error
        return digest.hexdigest()

    def _start_environment(self, *, two_outputs: bool) -> None:
        if self._xorg is not None or self._openbox is not None:
            raise HarnessError("live_environment_already_started")
        self._runtime = Path(tempfile.mkdtemp(prefix="prismdrake-pd2-"))
        self._runtime.chmod(0o700)
        for directory in ("home", "config", "cache", "data", "state"):
            path = self._runtime / directory
            path.mkdir(mode=0o700)
        config_path = self._runtime / "xorg.conf"
        config_path.write_text(_xorg_config(), encoding="ascii")
        config_path.chmod(0o600)

        for number in range(self._config.first_display, self._config.last_display + 1):
            if not self._operations.display_available(number):
                continue
            display = f":{number}"
            argv = (
                str(self._config.xorg),
                display,
                "-config",
                str(config_path),
                "-nolisten",
                "tcp",
                "-noreset",
                "-logfile",
                str(self._runtime / "Xorg.log"),
            )
            child = self._operations.spawn(argv, self._base_environment())
            self._display = display
            self._xorg = child
            if self._wait_for_xorg():
                # Do not accept a query raced through an unrelated server while
                # this exact Xorg child was still failing its own startup.
                self._operations.pause(0.05)
                if child.poll() is None:
                    break
            self._operations.stop(child, self._config.stop_timeout)
            self._xorg = None
            self._display = None
        if self._xorg is None:
            raise HarnessError("xorg_readiness_failed")
        self._configure_primary()
        if two_outputs:
            self._enable_secondary()
        else:
            self._xrandr("--output", SECONDARY_OUTPUT, "--off")
        self._start_openbox()

    def _base_environment(self) -> dict[str, str]:
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}
        if self._runtime is not None:
            env.update(
                {
                    "HOME": str(self._runtime / "home"),
                    "XDG_CACHE_HOME": str(self._runtime / "cache"),
                    "XDG_CONFIG_HOME": str(self._runtime / "config"),
                    "XDG_DATA_DIRS": "/usr/local/share:/usr/share",
                    "XDG_DATA_HOME": str(self._runtime / "data"),
                    "XDG_RUNTIME_DIR": str(self._runtime),
                    "XDG_STATE_HOME": str(self._runtime / "state"),
                }
            )
        return env

    def _x11_environment(self) -> dict[str, str]:
        if self._display is None:
            raise HarnessError("xorg_not_started")
        env = self._base_environment()
        env["DISPLAY"] = self._display
        return env

    def _run_x11(self, argv: Sequence[str]) -> CommandResult:
        return self._operations.run(argv, self._x11_environment(), 3.0)

    def _start_session(self) -> None:
        if self._runtime is None or self._xorg is None or self._openbox is None:
            raise HarnessError("live_environment_not_started")
        if self._session is not None:
            raise HarnessError("live_session_already_started")
        if tuple(self._config.installed_executables.keys()) != EXECUTABLE_NAMES:
            raise HarnessError("installed_executable_set_invalid")
        executables = self._config.installed_executables
        configuration_directory = self._runtime / "config" / "prismdrake"
        try:
            configuration_directory.mkdir(mode=0o700)
            configuration = configuration_directory / "config.toml"
            configuration.write_text(_session_config(), encoding="ascii")
            configuration.chmod(0o600)
        except OSError as error:
            raise HarnessError("private_session_configuration_failed") from error
        environment = self._x11_environment()
        environment.update(
            {
                "QSG_RHI_BACKEND": "software",
                "QT_QPA_PLATFORM": "xcb",
                "QT_QUICK_CONTROLS_STYLE": "Basic",
            }
        )
        argv = (
            str(self._config.dbus_run_session),
            f"--dbus-daemon={self._config.dbus_daemon}",
            "--",
            str(executables["prismdrake-session"]),
            "--settingsd",
            str(executables["prismdrake-settingsd"]),
            "--notifyd",
            str(executables["prismdrake-notifyd"]),
            "--shell",
            str(executables["prismdrake-shell"]),
        )
        self._session = self._operations.spawn(argv, environment)
        self._operations.pause(0.05)
        if self._session.poll() is not None:
            self._session = None
            raise HarnessError("prismdrake_session_start_failed")

    def _wait_for_session_readiness(self) -> None:
        if self._session is None or self._runtime is None:
            raise HarnessError("prismdrake_session_not_started")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._session.poll() is not None:
                raise HarnessError("prismdrake_session_exited_before_readiness")
            state = self._session_marker_state()
            if state == "ready":
                return
            if state == "safe_mode":
                raise HarnessError("prismdrake_session_safe_mode_unexpected")
            self._operations.pause(0.05)
        raise HarnessError("prismdrake_session_readiness_failed")

    def _session_marker_state(self) -> str:
        if self._runtime is None:
            raise HarnessError("prismdrake_session_not_started")
        root = self._runtime / "prismdrake"
        try:
            root_metadata = root.lstat()
            entries = tuple(root.iterdir())
        except FileNotFoundError:
            return "pending"
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(root_metadata.st_mode)
            or not stat.S_ISDIR(root_metadata.st_mode)
            or root_metadata.st_uid != os.geteuid()
            or stat.S_IMODE(root_metadata.st_mode) != 0o700
            or len(entries) != 1
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        instance = entries[0]
        if re.fullmatch(r"session-[1-9][0-9]{0,19}-[0-9]{1,3}", instance.name) is None:
            raise HarnessError("prismdrake_session_runtime_invalid")
        try:
            metadata = instance.lstat()
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o700
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        ready = self._valid_marker(instance / "ready")
        safe_mode = self._valid_marker(instance / "safe-mode")
        if safe_mode:
            return "safe_mode"
        return "ready" if ready else "pending"

    @staticmethod
    def _valid_marker(path: Path) -> bool:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return False
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o600
            or metadata.st_size != 0
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        return True

    def _require_desktop_wallpaper_surfaces(self) -> None:
        self._require_topology(two_outputs=True)
        shell = self._require_surface_shell()
        desktops = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DESKTOP", shell)
        expected = {
            WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
            WindowGeometry(OUTPUT_WIDTH, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
        }
        if len(desktops) != 2 or set(desktops.values()) != expected:
            raise HarnessError("desktop_wallpaper_surface_invalid")
        # The production readiness byte is emitted only after wallpaper,
        # settings, output, and desktop generations align. Requiring that
        # marker together with two mapped shell-owned DesktopWallpaper views
        # observes the solid fallback on each output without capturing pixels.
        self._desktop_surfaces = desktops
        self._surface_shell_identity = shell

    def _require_panel_surfaces(self) -> None:
        if self._desktop_surfaces is None or self._surface_shell_identity is None:
            raise HarnessError("desktop_surface_observation_missing")
        self._require_topology(two_outputs=True)
        shell = self._require_surface_shell()
        if shell != self._surface_shell_identity:
            raise HarnessError("surface_shell_identity_changed")
        desktops = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DESKTOP", shell)
        if desktops != self._desktop_surfaces:
            raise HarnessError("desktop_surface_epoch_changed")
        panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
        expected_geometries = {
            WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT),
            WindowGeometry(
                OUTPUT_WIDTH,
                OUTPUT_HEIGHT - PANEL_HEIGHT,
                OUTPUT_WIDTH,
                PANEL_HEIGHT,
            ),
        }
        if len(panels) != 2 or set(panels.values()) != expected_geometries:
            raise HarnessError("panel_surface_invalid")
        for window, geometry in panels.items():
            strut = self._window_cardinals(window, b"_NET_WM_STRUT", 4)
            partial = self._window_cardinals(window, b"_NET_WM_STRUT_PARTIAL", 12)
            if strut != (0, 0, 0, PANEL_HEIGHT) or partial != (
                0,
                0,
                0,
                PANEL_HEIGHT,
                0,
                0,
                0,
                0,
                0,
                0,
                geometry.x,
                geometry.x + geometry.width - 1,
            ):
                raise HarnessError("panel_strut_invalid")

    def _require_surface_shell(self) -> ProcessIdentity:
        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        if self._session_marker_state() != "ready":
            raise HarnessError("prismdrake_session_readiness_lost")
        executable = self._config.installed_executables.get("prismdrake-shell")
        if executable is None:
            raise HarnessError("surface_shell_identity_invalid")
        identities = self._operations.descendants(self._session, executable)
        if len(identities) != 1:
            raise HarnessError("surface_shell_identity_invalid")
        return identities[0]

    def _observe_surface_windows(
        self, window_type: bytes, shell: ProcessIdentity
    ) -> dict[int, WindowGeometry]:
        result = self._run_x11(
            (str(self._config.xprop), "-root", "_NET_CLIENT_LIST_STACKING")
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_list_query_failed")
        windows = parse_window_ids(result.stdout, b"_NET_CLIENT_LIST_STACKING")
        observed: dict[int, WindowGeometry] = {}
        expected_type = b"_NET_WM_WINDOW_TYPE(ATOM) = " + window_type + b"\n"
        valid_type = re.compile(
            rb"_NET_WM_WINDOW_TYPE\(ATOM\) = _NET_WM_WINDOW_TYPE_[A-Z_]{1,64}"
            rb"(?:, _NET_WM_WINDOW_TYPE_[A-Z_]{1,64})*\n"
        )
        for window in windows:
            payload = self._window_property(window, b"_NET_WM_WINDOW_TYPE")
            if payload != expected_type:
                if valid_type.fullmatch(payload) is None:
                    raise HarnessError("x11_window_type_invalid")
                continue
            owner = self._window_cardinals(window, b"_NET_WM_PID", 1)
            if owner != (shell.pid,):
                raise HarnessError("surface_window_owner_invalid")
            hints = self._window_property(window, b"WM_HINTS")
            focus_lines = [
                line
                for line in hints.splitlines()
                if re.fullmatch(
                    rb"[ \t]+Client accepts input or input focus: (?:True|False)",
                    line,
                )
            ]
            if len(focus_lines) != 1 or not focus_lines[0].endswith(b": False"):
                raise HarnessError("surface_window_focus_policy_invalid")
            geometry_result = self._run_x11(
                (str(self._config.xwininfo), "-id", f"0x{window:x}", "-stats")
            )
            if geometry_result.returncode != 0:
                raise HarnessError("x11_window_geometry_query_failed")
            observed[window] = parse_window_geometry(geometry_result.stdout)
        return observed

    def _window_property(self, window: int, property_name: bytes) -> bytes:
        try:
            decoded = property_name.decode("ascii")
        except UnicodeDecodeError as error:
            raise HarnessError("x11_property_name_invalid") from error
        result = self._run_x11(
            (str(self._config.xprop), "-id", f"0x{window:x}", decoded)
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_property_query_failed")
        return result.stdout

    def _window_cardinals(
        self, window: int, property_name: bytes, count: int
    ) -> tuple[int, ...]:
        return parse_cardinals(
            self._window_property(window, property_name), property_name, count
        )

    def _wait_for_xorg(self) -> bool:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._xorg is None or self._xorg.poll() is not None:
                return False
            result = self._run_x11((str(self._config.xrandr), "--query"))
            if result.returncode == 0:
                try:
                    parse_xrandr_outputs(result.stdout)
                    return True
                except HarnessError:
                    pass
            self._operations.pause(0.05)
        return False

    def _xrandr(self, *arguments: str) -> None:
        result = self._run_x11((str(self._config.xrandr), *arguments))
        if result.returncode != 0:
            raise HarnessError("xrandr_mutation_failed")

    def _configure_primary(self) -> None:
        self._xrandr(
            "--output", PRIMARY_OUTPUT, "--mode", OUTPUT_MODE,
            "--scale", "1x1", "--pos", "0x0", "--primary",
        )

    def _enable_secondary(self) -> None:
        self._xrandr(
            "--output", SECONDARY_OUTPUT, "--mode", OUTPUT_MODE,
            "--scale", "1x1", "--pos", "1600x0",
        )

    def _query_outputs(self) -> dict[str, XrandrOutput]:
        result = self._run_x11((str(self._config.xrandr), "--query"))
        if result.returncode != 0:
            raise HarnessError("xrandr_query_failed")
        return {output.name: output for output in parse_xrandr_outputs(result.stdout)}

    def _require_topology(
        self,
        *,
        two_outputs: bool,
        primary: str = PRIMARY_OUTPUT,
        secondary_position: tuple[int, int] = (1600, 0),
    ) -> None:
        outputs = self._query_outputs()
        first = outputs.get(PRIMARY_OUTPUT)
        second = outputs.get(SECONDARY_OUTPUT)
        if first is None or second is None or not first.active:
            raise HarnessError("xrandr_topology_invalid")
        if (first.width, first.height, first.x, first.y) != (
            OUTPUT_WIDTH, OUTPUT_HEIGHT, 0, 0
        ):
            raise HarnessError("xrandr_topology_invalid")
        if two_outputs:
            if not second.active or (second.width, second.height, second.x, second.y) != (
                OUTPUT_WIDTH, OUTPUT_HEIGHT, *secondary_position
            ):
                raise HarnessError("xrandr_topology_invalid")
        elif second.active:
            raise HarnessError("xrandr_topology_invalid")
        active_primary = [output.name for output in outputs.values() if output.primary]
        if active_primary != [primary]:
            raise HarnessError("xrandr_primary_invalid")

    def _require_only_secondary(self) -> None:
        outputs = self._query_outputs()
        first = outputs.get(PRIMARY_OUTPUT)
        second = outputs.get(SECONDARY_OUTPUT)
        if first is None or second is None or first.active or not second.active:
            raise HarnessError("xrandr_primary_removal_invalid")

    def _start_openbox(self) -> None:
        self._openbox = self._operations.spawn(
            (str(self._config.openbox), "--sm-disable"), self._x11_environment()
        )
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._openbox.poll() is not None:
                break
            result = self._run_x11(
                (str(self._config.xprop), "-root", "_NET_SUPPORTING_WM_CHECK")
            )
            if result.returncode == 0 and re.fullmatch(
                rb"_NET_SUPPORTING_WM_CHECK\(WINDOW\): window id # 0x[0-9a-f]+\n?",
                result.stdout,
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError("window_manager_readiness_failed")
