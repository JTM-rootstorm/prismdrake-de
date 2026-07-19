#!/usr/bin/env python3
"""Fail-closed controls for PD2 notification failure replay.

The helper covers the two notification-specific WP14 failures without placing
notification content, D-Bus unique names, process identifiers, or paths in
evidence.  It is intentionally separate from the Xorg adapter so the installed
harness can compose it with its existing process and private-bus attestations.
"""

from __future__ import annotations

import hashlib
import multiprocessing
import os
import signal
import stat
import subprocess
import tempfile
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from pd2_live_dbus import LiveDbusProbe, PrivateBusEndpoint
from pd2_live_notification_workflow import LiveDbusNotificationPort


STANDARD_BUS_NAME = "org.freedesktop.Notifications"
HISTORY_RELATIVE_PATH = Path("prismdrake/notifications/history-v1.json")
MALFORMED_HISTORY_BYTES = b'{"schema_version":1,"records":['
NOTIFYD_CONFLICT_EXIT_STATUS = 3
NOTIFYD_CONFLICT_DIAGNOSTIC = (
    b"prismdrake-notifyd: A notification service name is already owned.\n"
)
MAX_PROCESS_OUTPUT_BYTES = 4096
MAX_ENVIRONMENT_ENTRIES = 128
MAX_ENVIRONMENT_VALUE_BYTES = 8192
OWNER_COMMAND_SECONDS = 3.0
PROCESS_EXIT_SECONDS = 5.0
PROCESS_STOP_SECONDS = 0.5

_CATEGORIES = frozenset(
    {
        "notification_failure_environment_invalid",
        "notification_failure_executable_invalid",
        "notification_failure_history_exists",
        "notification_failure_history_invalid",
        "notification_failure_history_setup_failed",
        "notification_failure_history_state_invalid",
        "notification_failure_owner_failed",
        "notification_failure_owner_lost",
        "notification_failure_owner_protocol_invalid",
        "notification_failure_owner_timeout",
        "notification_failure_process_failed",
        "notification_failure_process_output_invalid",
        "notification_failure_process_timeout",
        "notification_failure_sibling_changed",
        "notification_failure_sibling_invalid",
        "notification_failure_state_root_invalid",
    }
)


class NotificationFailureError(RuntimeError):
    """One redacted notification failure-replay category."""


@dataclass(frozen=True)
class FailureEvidence:
    """Categorical result suitable for the installed evidence manifest."""

    category: str
    status: str = "passed"

    def as_dict(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


@dataclass(frozen=True)
class MalformedHistoryFixture:
    """Identity of the known malformed file, with content hidden from repr."""

    path: Path = field(repr=False)
    device: int = field(repr=False)
    inode: int = field(repr=False)
    digest: bytes = field(repr=False)

    def verify_retained(self) -> None:
        descriptor = -1
        try:
            flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            descriptor = os.open(self.path, flags)
            metadata = os.fstat(descriptor)
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or stat.S_IMODE(metadata.st_mode) != 0o600
                or (metadata.st_dev, metadata.st_ino) != (self.device, self.inode)
                or metadata.st_size != len(MALFORMED_HISTORY_BYTES)
            ):
                _fail("notification_failure_history_invalid")
            payload = os.read(descriptor, len(MALFORMED_HISTORY_BYTES) + 1)
            if hashlib.sha256(payload).digest() != self.digest:
                _fail("notification_failure_history_invalid")
        except NotificationFailureError:
            raise
        except Exception:
            _fail("notification_failure_history_invalid")
        finally:
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                except OSError:
                    pass


class OwnerLease(Protocol):
    """Opaque ownership lease; implementations never expose a unique name."""

    def verify(self) -> None: ...


BusAttestor = Callable[[Mapping[str, str], PrivateBusEndpoint], str]


class _AttestationTransport:
    def __init__(self, address: str) -> None:
        self.address = address

    def call(self, spec: Any, arguments: tuple[Any, ...]) -> tuple[Any, ...]:
        del spec, arguments
        _fail("notification_failure_owner_protocol_invalid")

    def close(self) -> None:
        return


def _fail(category: str) -> None:
    if category not in _CATEGORIES:
        category = "notification_failure_process_failed"
    error = NotificationFailureError(category)
    error.__cause__ = None
    error.__context__ = None
    raise error from None


def _private_directory(path: Path, category: str) -> os.stat_result:
    try:
        metadata = path.lstat()
    except Exception:
        _fail(category)
    if (
        not path.is_absolute()
        or ".." in path.parts
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or stat.S_IMODE(metadata.st_mode) != 0o700
    ):
        _fail(category)
    return metadata


def _ensure_private_child(parent: Path, name: str) -> Path:
    path = parent / name
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    except Exception:
        _fail("notification_failure_history_setup_failed")
    _private_directory(path, "notification_failure_history_setup_failed")
    return path


def create_malformed_history_fixture(state_root: Path) -> MalformedHistoryFixture:
    """Create one known malformed persistent-history file in a private XDG root.

    Existing history is never replaced.  The installed adapter is expected to
    provide a scenario-private ``XDG_STATE_HOME``.
    """

    if not isinstance(state_root, Path):
        _fail("notification_failure_state_root_invalid")
    _private_directory(state_root, "notification_failure_state_root_invalid")
    prismdrake = _ensure_private_child(state_root, "prismdrake")
    notifications = _ensure_private_child(prismdrake, "notifications")
    history = notifications / "history-v1.json"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = -1
    try:
        descriptor = os.open(history, flags, 0o600)
        completed = 0
        while completed < len(MALFORMED_HISTORY_BYTES):
            written = os.write(descriptor, MALFORMED_HISTORY_BYTES[completed:])
            if written <= 0:
                _fail("notification_failure_history_setup_failed")
            completed += written
        os.fsync(descriptor)
        metadata = os.fstat(descriptor)
    except FileExistsError:
        _fail("notification_failure_history_exists")
    except NotificationFailureError:
        raise
    except Exception:
        _fail("notification_failure_history_setup_failed")
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or stat.S_IMODE(metadata.st_mode) != 0o600
    ):
        _fail("notification_failure_history_setup_failed")
    fixture = MalformedHistoryFixture(
        history,
        metadata.st_dev,
        metadata.st_ino,
        hashlib.sha256(MALFORMED_HISTORY_BYTES).digest(),
    )
    fixture.verify_retained()
    return fixture


def verify_malformed_history_recovery(
    fixture: MalformedHistoryFixture, probe: LiveDbusProbe
) -> FailureEvidence:
    """Require retained malformed bytes and one empty persistent live state."""

    if not isinstance(fixture, MalformedHistoryFixture):
        _fail("notification_failure_history_invalid")
    fixture.verify_retained()
    try:
        state = LiveDbusNotificationPort(probe).current_state()
    except Exception:
        _fail("notification_failure_history_state_invalid")
    if state.history_mode != "persistent" or state.records:
        _fail("notification_failure_history_state_invalid")
    fixture.verify_retained()
    return FailureEvidence("malformed_persistent_history_recovered")


def _attest_private_bus(
    environment: Mapping[str, str], endpoint: PrivateBusEndpoint
) -> str:
    try:
        probe = LiveDbusProbe(
            environment,
            endpoint=endpoint,
            transport_factory=_AttestationTransport,
        )
        probe.close()
    except Exception:
        _fail("notification_failure_environment_invalid")
    return endpoint.address


def _gio_owner_worker(channel: Any, address: str) -> None:
    """Own the standard name in a killable child and expose categories only."""

    connection = None
    try:
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib

        flags = (
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        )
        connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        connection.set_exit_on_close(False)

        def call(member: str, signature: str, arguments: tuple[Any, ...], reply: str) -> Any:
            result = connection.call_sync(
                "org.freedesktop.DBus",
                "/org/freedesktop/DBus",
                "org.freedesktop.DBus",
                member,
                GLib.Variant(f"({signature})", arguments),
                GLib.VariantType.new(f"({reply})"),
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            )
            return result.unpack()

        acquired = call("RequestName", "su", (STANDARD_BUS_NAME, 0), "u")
        unique_name = connection.get_unique_name()
        if acquired != (1,) or not isinstance(unique_name, str) or not unique_name:
            raise RuntimeError("owner")
        channel.send(("ready",))
        while True:
            command = channel.recv()
            if command == "verify":
                owner = call("GetNameOwner", "s", (STANDARD_BUS_NAME,), "s")
                channel.send(("owned",) if owner == (unique_name,) else ("lost",))
            elif command == "close":
                released = call("ReleaseName", "s", (STANDARD_BUS_NAME,), "u")
                connection.close_sync(None)
                connection = None
                channel.send(("closed",) if released == (1,) else ("failed",))
                return
            else:
                channel.send(("failed",))
                return
    except Exception:
        try:
            channel.send(("failed",))
        except Exception:
            pass
    finally:
        if connection is not None:
            try:
                connection.close_sync(None)
            except Exception:
                pass
        channel.close()


class GioNotificationOwnerLease:
    """Bounded child-process lease for ``org.freedesktop.Notifications``."""

    def __init__(
        self,
        environment: Mapping[str, str],
        endpoint: PrivateBusEndpoint,
        *,
        worker: Callable[[Any, str], None] = _gio_owner_worker,
        attestor: BusAttestor = _attest_private_bus,
        command_seconds: float = OWNER_COMMAND_SECONDS,
    ) -> None:
        if (
            not isinstance(command_seconds, (int, float))
            or not 0.05 <= command_seconds <= 10.0
        ):
            _fail("notification_failure_owner_protocol_invalid")
        self._environment = dict(environment)
        self._endpoint = endpoint
        self._attestor = attestor
        self._command_seconds = float(command_seconds)
        try:
            address = attestor(self._environment, endpoint)
        except NotificationFailureError:
            raise
        except Exception:
            _fail("notification_failure_environment_invalid")
        parent, child = multiprocessing.get_context("spawn").Pipe(duplex=True)
        process = multiprocessing.get_context("spawn").Process(
            target=worker, args=(child, address), daemon=True
        )
        self._channel = parent
        self._process = process
        self._process_started = False
        self._closed = False
        try:
            process.start()
            self._process_started = True
            child.close()
            message = self._receive()
            if message != ("ready",):
                _fail("notification_failure_owner_failed")
            self.verify()
        except Exception as error:
            self._stop()
            if isinstance(error, NotificationFailureError):
                raise
            _fail("notification_failure_owner_failed")

    def _receive(self) -> tuple[str, ...]:
        if not self._channel.poll(self._command_seconds):
            _fail("notification_failure_owner_timeout")
        try:
            message = self._channel.recv()
        except Exception:
            _fail("notification_failure_owner_failed")
        if (
            not isinstance(message, tuple)
            or len(message) != 1
            or message[0] not in {"ready", "owned", "lost", "closed", "failed"}
        ):
            _fail("notification_failure_owner_protocol_invalid")
        return message

    def _stop(self) -> None:
        if self._process_started and self._process.is_alive():
            self._process.terminate()
            self._process.join(PROCESS_STOP_SECONDS)
        if self._process_started and self._process.is_alive():
            self._process.kill()
            self._process.join(PROCESS_STOP_SECONDS)
        try:
            self._channel.close()
        except Exception:
            pass

    def verify(self) -> None:
        if (
            self._closed
            or not self._process_started
            or not self._process.is_alive()
        ):
            _fail("notification_failure_owner_lost")
        try:
            self._attestor(self._environment, self._endpoint)
        except NotificationFailureError:
            raise
        except Exception:
            _fail("notification_failure_environment_invalid")
        try:
            self._channel.send("verify")
        except Exception:
            _fail("notification_failure_owner_lost")
        if self._receive() != ("owned",):
            _fail("notification_failure_owner_lost")
        try:
            self._attestor(self._environment, self._endpoint)
        except NotificationFailureError:
            raise
        except Exception:
            _fail("notification_failure_environment_invalid")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        failure = False
        try:
            if not self._process_started or not self._process.is_alive():
                failure = True
            else:
                self._channel.send("close")
                failure = self._receive() != ("closed",)
            self._process.join(PROCESS_STOP_SECONDS)
            failure = failure or self._process.is_alive()
        except Exception:
            failure = True
        finally:
            self._stop()
        if failure:
            _fail("notification_failure_owner_failed")

    def __enter__(self) -> "GioNotificationOwnerLease":
        return self

    def __exit__(self, exception_type: Any, exception: Any, traceback: Any) -> None:
        del exception_type, exception, traceback
        self.close()


def _checked_environment(environment: Mapping[str, str]) -> dict[str, str]:
    if (
        not isinstance(environment, Mapping)
        or len(environment) > MAX_ENVIRONMENT_ENTRIES
    ):
        _fail("notification_failure_environment_invalid")
    checked: dict[str, str] = {}
    for key, value in environment.items():
        if (
            not isinstance(key, str)
            or not key
            or "=" in key
            or "\x00" in key
            or not isinstance(value, str)
            or "\x00" in value
            or len(key.encode("utf-8")) > 256
            or len(value.encode("utf-8")) > MAX_ENVIRONMENT_VALUE_BYTES
        ):
            _fail("notification_failure_environment_invalid")
        checked[key] = value
    return checked


def _read_bounded_output(path: Path) -> bytes:
    try:
        metadata = path.lstat()
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o600
            or metadata.st_size > MAX_PROCESS_OUTPUT_BYTES
        ):
            _fail("notification_failure_process_output_invalid")
        return path.read_bytes()
    except NotificationFailureError:
        raise
    except Exception:
        _fail("notification_failure_process_output_invalid")


def _identity_map(value: Any) -> dict[str, tuple[int, int]]:
    expected = {"prismdrake-settingsd", "prismdrake-shell"}
    if not isinstance(value, Mapping) or set(value) != expected:
        _fail("notification_failure_sibling_invalid")
    checked: dict[str, tuple[int, int]] = {}
    for name, identity in value.items():
        if (
            not isinstance(identity, tuple)
            or len(identity) != 2
            or any(
                isinstance(item, bool) or not isinstance(item, int) or item <= 0
                for item in identity
            )
        ):
            _fail("notification_failure_sibling_invalid")
        checked[name] = identity
    return checked


def verify_conflicting_notification_owner(
    executable: Path,
    environment: Mapping[str, str],
    endpoint: PrivateBusEndpoint,
    owner: OwnerLease,
    sibling_observer: Callable[[], Mapping[str, tuple[int, int]]],
    *,
    timeout_seconds: float = PROCESS_EXIT_SECONDS,
    attestor: BusAttestor = _attest_private_bus,
) -> FailureEvidence:
    """Run notifyd once and prove a name conflict is bounded and isolated."""

    if (
        not isinstance(executable, Path)
        or not executable.is_absolute()
        or not executable.is_file()
        or not os.access(executable, os.X_OK)
    ):
        _fail("notification_failure_executable_invalid")
    if (
        not isinstance(timeout_seconds, (int, float))
        or not 0.05 <= timeout_seconds <= 15.0
    ):
        _fail("notification_failure_process_timeout")
    checked_environment = _checked_environment(environment)
    try:
        attestor(checked_environment, endpoint)
    except NotificationFailureError:
        raise
    except Exception:
        _fail("notification_failure_environment_invalid")
    try:
        owner.verify()
        siblings_before = _identity_map(sibling_observer())
    except NotificationFailureError:
        raise
    except Exception:
        _fail("notification_failure_sibling_invalid")

    runtime = Path(checked_environment.get("XDG_RUNTIME_DIR", ""))
    _private_directory(runtime, "notification_failure_environment_invalid")
    stdout_descriptor = stderr_descriptor = -1
    stdout_path: Path | None = None
    stderr_path: Path | None = None
    process: subprocess.Popen[bytes] | None = None
    try:
        stdout_descriptor, stdout_name = tempfile.mkstemp(
            prefix="pd2-notifyd-conflict-out-", dir=runtime
        )
        stderr_descriptor, stderr_name = tempfile.mkstemp(
            prefix="pd2-notifyd-conflict-err-", dir=runtime
        )
        os.fchmod(stdout_descriptor, 0o600)
        os.fchmod(stderr_descriptor, 0o600)
        stdout_path = Path(stdout_name)
        stderr_path = Path(stderr_name)
        with os.fdopen(stdout_descriptor, "wb", closefd=True) as stdout_stream, os.fdopen(
            stderr_descriptor, "wb", closefd=True
        ) as stderr_stream:
            stdout_descriptor = stderr_descriptor = -1
            process = subprocess.Popen(
                [str(executable), "--foreground"],
                stdin=subprocess.DEVNULL,
                stdout=stdout_stream,
                stderr=stderr_stream,
                cwd="/",
                env=checked_environment,
                start_new_session=True,
            )
            try:
                returncode = process.wait(timeout=float(timeout_seconds))
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=PROCESS_STOP_SECONDS)
                except Exception:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except Exception:
                        pass
                    process.wait(timeout=PROCESS_STOP_SECONDS)
                _fail("notification_failure_process_timeout")
        stdout = _read_bounded_output(stdout_path)
        stderr = _read_bounded_output(stderr_path)
        if (
            returncode != NOTIFYD_CONFLICT_EXIT_STATUS
            or stdout
            or stderr != NOTIFYD_CONFLICT_DIAGNOSTIC
        ):
            _fail("notification_failure_process_failed")
        owner.verify()
        try:
            siblings_after = _identity_map(sibling_observer())
        except NotificationFailureError:
            raise
        except Exception:
            _fail("notification_failure_sibling_invalid")
        if siblings_after != siblings_before:
            _fail("notification_failure_sibling_changed")
        try:
            attestor(checked_environment, endpoint)
        except NotificationFailureError:
            raise
        except Exception:
            _fail("notification_failure_environment_invalid")
        return FailureEvidence("notification_owner_conflict_isolated")
    except NotificationFailureError:
        raise
    except Exception:
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=PROCESS_STOP_SECONDS)
            except Exception:
                pass
        _fail("notification_failure_process_failed")
    finally:
        for descriptor in (stdout_descriptor, stderr_descriptor):
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
        for path in (stdout_path, stderr_path):
            if path is not None:
                try:
                    path.unlink()
                except OSError:
                    pass


def validate_failure_evidence(value: Any) -> None:
    if (
        not isinstance(value, FailureEvidence)
        or value.status != "passed"
        or value.category
        not in {
            "malformed_persistent_history_recovered",
            "notification_owner_conflict_isolated",
        }
        or value.as_dict() != {"category": value.category, "status": "passed"}
    ):
        _fail("notification_failure_process_failed")
