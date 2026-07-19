#!/usr/bin/env python3
"""Strict typed D-Bus probes for the installed PD2 live demonstration.

The probe connects only to the caller-attested private session bus.  Public
operations are a closed mirror of the current Experimental XML contracts; no
arbitrary destination, object, interface, member, property, or signature is
accepted.  Returned evidence is categorical and never serializes reply data.
"""

from __future__ import annotations

import importlib.util
import json
import math
import multiprocessing
import os
import stat
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol


MAX_ADDRESS_BYTES = 4096
MAX_ENVIRONMENT_ENTRIES = 128
MAX_ENVIRONMENT_VALUE_BYTES = 8192
MAX_OUTPUT_NAME_BYTES = 255
MAX_ACTION_ID_BYTES = 256
MAX_ACTION_ID_CODEPOINTS = 64
MAX_SETTINGS_BYTES = 1024 * 1024
MAX_NOTIFICATION_STATE_BYTES = 8 * 1024 * 1024
MAX_REPLY_ENVELOPE_BYTES = 65_536
MAX_DIAGNOSTICS = 64
MAX_JSON_DEPTH = 64
MAX_JSON_NODES = 131_072
MAX_SCHEMA_BYTES = 1024 * 1024
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
HELPER_CONNECT_CLOSE_SECONDS = 4.0
HELPER_STOP_SECONDS = 0.5

CLOSED_ERROR_CATEGORIES = frozenset(
    {
        "dbus_argument_invalid",
        "dbus_argument_oversized",
        "dbus_arguments_invalid",
        "dbus_boolean_invalid",
        "dbus_byte_array_bound",
        "dbus_byte_array_invalid",
        "dbus_call_failed",
        "dbus_diagnostic_bound",
        "dbus_diagnostic_invalid",
        "dbus_environment_bound",
        "dbus_environment_invalid",
        "dbus_evidence_invalid",
        "dbus_helper_failed",
        "dbus_helper_timeout",
        "dbus_json_depth_bound",
        "dbus_json_duplicate_key",
        "dbus_json_generation_invalid",
        "dbus_json_invalid",
        "dbus_json_node_bound",
        "dbus_json_noncanonical",
        "dbus_json_schema_invalid",
        "dbus_operation_not_reviewed",
        "dbus_private_address_invalid",
        "dbus_private_address_mismatch",
        "dbus_private_connection_close_failed",
        "dbus_private_connection_failed",
        "dbus_private_socket_identity_invalid",
        "dbus_probe_closed",
        "dbus_profile_invalid",
        "dbus_profile_reply_invalid",
        "dbus_property_not_reviewed",
        "dbus_reply_oversized",
        "dbus_reply_size_invalid",
        "dbus_reply_type_invalid",
        "dbus_schema_unavailable",
        "dbus_schema_validation_failed",
        "dbus_schema_version_invalid",
        "dbus_session_state_invalid",
        "dbus_unsigned_invalid",
    }
)

SESSION_STATES = frozenset(
    {
        "idle",
        "confirmation_requested",
        "confirmed",
        "stopping_shell",
        "stopping_notifyd",
        "stopping_settingsd",
        "complete",
        "cancelled",
        "failed",
    }
)


class LiveDbusProbeError(RuntimeError):
    """A fail-closed probe failure represented by one redacted category."""


@dataclass(frozen=True)
class MethodSpec:
    """One fixed method from the committed Experimental interface XML."""

    service: str
    path: str
    interface: str
    member: str
    input_signature: str
    output_signature: str
    timeout_milliseconds: int
    category: str


@dataclass(frozen=True)
class PrivateBusEndpoint:
    """Exact private-bus address and socket identity attested by the adapter."""

    address: str
    socket_device: int
    socket_inode: int


@dataclass(frozen=True)
class ProbeResult:
    """Typed live result whose evidence form never contains reply data."""

    operation: str
    category: str
    payload: tuple[Any, ...] = field(repr=False)
    status: str = "passed"

    def evidence(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


class Transport(Protocol):
    """Minimal exact-call boundary used by real Gio and hermetic tests."""

    def call(
        self, spec: MethodSpec, arguments: tuple[Any, ...]
    ) -> tuple[Any, ...]: ...

    def close(self) -> None: ...


TransportFactory = Callable[[str], Transport]
SocketInspector = Callable[[Path], tuple[int, int] | None]


def _spec(
    service: str,
    path: str,
    interface: str,
    member: str,
    input_signature: str,
    output_signature: str,
    *,
    mutation: bool,
    category: str,
) -> MethodSpec:
    return MethodSpec(
        service,
        path,
        interface,
        member,
        input_signature,
        output_signature,
        6000 if mutation else 2000,
        category,
    )


_SETTINGS_SERVICE = "org.prismdrake.Settings1"
_SETTINGS_PATH = "/org/prismdrake/Settings1"
_SHELL_SERVICE = "org.prismdrake.Shell1"
_SHELL_PATH = "/org/prismdrake/Shell1"
_NOTIFICATION_SERVICE = "org.prismdrake.NotificationCenter1"
_NOTIFICATION_PATH = "/org/prismdrake/NotificationCenter1"
_SESSION_SERVICE = "org.prismdrake.Session1"
_SESSION_PATH = "/org/prismdrake/Session1"


METHODS: Mapping[str, MethodSpec] = {
    "settings_get_current_profile": _spec(
        _SETTINGS_SERVICE,
        _SETTINGS_PATH,
        "org.prismdrake.Settings1",
        "GetCurrentProfile",
        "",
        "st",
        mutation=False,
        category="settings_profile_observed",
    ),
    "settings_request_profile_change": _spec(
        _SETTINGS_SERVICE,
        _SETTINGS_PATH,
        "org.prismdrake.Settings1",
        "RequestProfileChange",
        "s",
        "t",
        mutation=True,
        category="settings_profile_change_requested",
    ),
    "settings_reload": _spec(
        _SETTINGS_SERVICE,
        _SETTINGS_PATH,
        "org.prismdrake.Settings1",
        "Reload",
        "",
        "t",
        mutation=True,
        category="settings_reload_requested",
    ),
    "settings_validate_candidate": _spec(
        _SETTINGS_SERVICE,
        _SETTINGS_PATH,
        "org.prismdrake.Settings1",
        "ValidateCandidate",
        "ay",
        "ba(sssss)",
        mutation=True,
        category="settings_candidate_validated",
    ),
    "settings_get_current_snapshot": _spec(
        _SETTINGS_SERVICE,
        _SETTINGS_PATH,
        "org.prismdrake.SettingsSnapshot1",
        "GetCurrentSnapshot",
        "u",
        "tay",
        mutation=False,
        category="settings_snapshot_observed",
    ),
    "shell_toggle_launcher": _spec(
        _SHELL_SERVICE,
        _SHELL_PATH,
        "org.prismdrake.Shell1",
        "ToggleLauncher",
        "s",
        "",
        mutation=False,
        category="shell_launcher_toggled",
    ),
    "shell_open_quick_settings": _spec(
        _SHELL_SERVICE,
        _SHELL_PATH,
        "org.prismdrake.Shell1",
        "OpenQuickSettings",
        "s",
        "",
        mutation=False,
        category="shell_quick_settings_opened",
    ),
    "shell_open_notification_center": _spec(
        _SHELL_SERVICE,
        _SHELL_PATH,
        "org.prismdrake.Shell1",
        "OpenNotificationCenter",
        "s",
        "",
        mutation=False,
        category="shell_notification_center_opened",
    ),
    "shell_focus_panel": _spec(
        _SHELL_SERVICE,
        _SHELL_PATH,
        "org.prismdrake.Shell1",
        "FocusPanel",
        "s",
        "",
        mutation=False,
        category="shell_panel_focused",
    ),
    "notification_get_current_state": _spec(
        _NOTIFICATION_SERVICE,
        _NOTIFICATION_PATH,
        "org.prismdrake.NotificationCenter1",
        "GetCurrentState",
        "u",
        "tay",
        mutation=False,
        category="notification_state_observed",
    ),
    "notification_set_do_not_disturb": _spec(
        _NOTIFICATION_SERVICE,
        _NOTIFICATION_PATH,
        "org.prismdrake.NotificationCenter1",
        "SetDoNotDisturb",
        "b",
        "t",
        mutation=True,
        category="notification_dnd_requested",
    ),
    "notification_clear_history": _spec(
        _NOTIFICATION_SERVICE,
        _NOTIFICATION_PATH,
        "org.prismdrake.NotificationCenter1",
        "ClearHistory",
        "",
        "t",
        mutation=True,
        category="notification_history_clear_requested",
    ),
    "notification_dismiss": _spec(
        _NOTIFICATION_SERVICE,
        _NOTIFICATION_PATH,
        "org.prismdrake.NotificationCenter1",
        "Dismiss",
        "tt",
        "t",
        mutation=True,
        category="notification_dismiss_requested",
    ),
    "notification_invoke_action": _spec(
        _NOTIFICATION_SERVICE,
        _NOTIFICATION_PATH,
        "org.prismdrake.NotificationCenter1",
        "InvokeAction",
        "tts",
        "t",
        mutation=True,
        category="notification_action_requested",
    ),
    "session_get_state": _spec(
        _SESSION_SERVICE,
        _SESSION_PATH,
        "org.prismdrake.Session1",
        "GetSessionState",
        "",
        "ts",
        mutation=False,
        category="session_state_observed",
    ),
    "session_request_logout": _spec(
        _SESSION_SERVICE,
        _SESSION_PATH,
        "org.prismdrake.Session1",
        "RequestLogout",
        "",
        "t",
        mutation=True,
        category="session_logout_requested",
    ),
    "session_confirm_logout": _spec(
        _SESSION_SERVICE,
        _SESSION_PATH,
        "org.prismdrake.Session1",
        "ConfirmLogout",
        "t",
        "",
        mutation=True,
        category="session_logout_confirmed",
    ),
    "session_cancel_logout": _spec(
        _SESSION_SERVICE,
        _SESSION_PATH,
        "org.prismdrake.Session1",
        "CancelLogout",
        "t",
        "",
        mutation=True,
        category="session_logout_cancelled",
    ),
}

# The current committed Experimental interfaces define no D-Bus properties.
# Keeping this explicit makes a future property addition an intentional probe
# review instead of an arbitrary org.freedesktop.DBus.Properties escape hatch.
PROPERTIES: Mapping[str, tuple[str, str, str]] = {}


def _fail(category: str) -> None:
    raise LiveDbusProbeError(category) from None


def _bounded_utf8(value: Any, byte_limit: int, *, empty: bool = True) -> str:
    if not isinstance(value, str) or "\x00" in value or (not empty and not value):
        _fail("dbus_argument_invalid")
    try:
        encoded = value.encode("utf-8")
    except UnicodeError:
        _fail("dbus_argument_invalid")
    if len(encoded) > byte_limit:
        _fail("dbus_argument_oversized")
    return value


def _unsigned(value: Any, maximum: int, *, nonzero: bool = False) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < (1 if nonzero else 0)
        or value > maximum
    ):
        _fail("dbus_unsigned_invalid")
    return value


def _octets(value: Any, maximum: int, *, empty: bool = False) -> bytes:
    if isinstance(value, bytes):
        payload = value
    elif isinstance(value, (bytearray, list, tuple)):
        try:
            payload = bytes(value)
        except (TypeError, ValueError):
            _fail("dbus_byte_array_invalid")
    else:
        _fail("dbus_byte_array_invalid")
    if (not empty and not payload) or len(payload) > maximum:
        _fail("dbus_byte_array_bound")
    return payload


def _reject_json_constant(value: str) -> Any:
    del value
    raise ValueError("non-finite JSON number")


def _finite_json_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError("non-finite JSON number")
    return parsed


def _validate_json_shape(document: Any) -> None:
    pending = [(document, 1)]
    nodes = 0
    while pending:
        value, depth = pending.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES:
            _fail("dbus_json_node_bound")
        if depth > MAX_JSON_DEPTH:
            _fail("dbus_json_depth_bound")
        if isinstance(value, dict):
            pending.extend((item, depth + 1) for item in value.values())
        elif isinstance(value, list):
            pending.extend((item, depth + 1) for item in value)


_SCHEMA_VALIDATOR: Callable[[Any, dict[str, Any], dict[str, Any], str], list[str]] | None = None
_SCHEMA_CACHE: dict[str, dict[str, Any]] = {}


def _project_schema_validator() -> Callable[
    [Any, dict[str, Any], dict[str, Any], str], list[str]
]:
    global _SCHEMA_VALIDATOR  # pylint: disable=global-statement
    if _SCHEMA_VALIDATOR is not None:
        return _SCHEMA_VALIDATOR
    source_root = Path(__file__).resolve().parents[2]
    module_path = source_root / "tools/validate.py"
    try:
        module_spec = importlib.util.spec_from_file_location(
            "_prismdrake_pd2_contract_validator", module_path
        )
        if module_spec is None or module_spec.loader is None:
            _fail("dbus_schema_unavailable")
        module = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(module)
        validator = getattr(module, "validate_schema", None)
    except LiveDbusProbeError:
        raise
    except Exception:
        _fail("dbus_schema_unavailable")
    if not callable(validator):
        _fail("dbus_schema_unavailable")
    _SCHEMA_VALIDATOR = validator
    return validator


def _committed_schema(name: str) -> dict[str, Any]:
    cached = _SCHEMA_CACHE.get(name)
    if cached is not None:
        return cached
    path = Path(__file__).resolve().parents[2] / "schemas" / name
    try:
        payload = path.read_bytes()
    except OSError:
        _fail("dbus_schema_unavailable")
    if not payload or len(payload) > MAX_SCHEMA_BYTES:
        _fail("dbus_schema_unavailable")
    try:
        schema = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=lambda pairs: dict(pairs),
            parse_constant=_reject_json_constant,
            parse_float=_finite_json_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError):
        _fail("dbus_schema_unavailable")
    if not isinstance(schema, dict):
        _fail("dbus_schema_unavailable")
    _SCHEMA_CACHE[name] = schema
    return schema


def _json_document(
    value: Any, maximum: int, generation: int, schema_name: str
) -> dict[str, Any]:
    payload = _octets(value, maximum)

    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, item in pairs:
            if key in result:
                _fail("dbus_json_duplicate_key")
            result[key] = item
        return result

    try:
        document = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=_reject_json_constant,
            parse_float=_finite_json_float,
        )
    except LiveDbusProbeError:
        raise
    except (UnicodeError, ValueError, json.JSONDecodeError, RecursionError):
        _fail("dbus_json_invalid")
    _validate_json_shape(document)
    embedded_generation = document.get("generation") if isinstance(document, dict) else None
    if (
        isinstance(embedded_generation, bool)
        or not isinstance(embedded_generation, int)
        or embedded_generation != generation
    ):
        _fail("dbus_json_generation_invalid")
    schema = _committed_schema(schema_name)
    try:
        schema_errors = _project_schema_validator()(
            document, schema, schema, "live_dbus_reply"
        )
    except LiveDbusProbeError:
        raise
    except Exception:
        _fail("dbus_schema_validation_failed")
    if schema_errors:
        _fail("dbus_json_schema_invalid")
    try:
        canonical = json.dumps(
            document,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeError):
        _fail("dbus_json_invalid")
    if canonical != payload:
        _fail("dbus_json_noncanonical")
    return document


def _validate_arguments(operation: str, arguments: tuple[Any, ...]) -> tuple[Any, ...]:
    if not isinstance(arguments, tuple):
        _fail("dbus_arguments_invalid")
    if operation in {
        "settings_get_current_profile",
        "settings_reload",
        "notification_clear_history",
        "session_get_state",
        "session_request_logout",
    }:
        if arguments:
            _fail("dbus_arguments_invalid")
        return arguments
    if operation == "settings_request_profile_change":
        if (
            len(arguments) != 1
            or not isinstance(arguments[0], str)
            or arguments[0] not in {"lustre", "forge"}
        ):
            _fail("dbus_profile_invalid")
        return arguments
    if operation == "settings_validate_candidate":
        if len(arguments) != 1:
            _fail("dbus_arguments_invalid")
        return (_octets(arguments[0], MAX_SETTINGS_BYTES, empty=True),)
    if operation == "settings_get_current_snapshot":
        if len(arguments) != 1 or _unsigned(arguments[0], UINT32_MAX) != 2:
            _fail("dbus_schema_version_invalid")
        return arguments
    if operation.startswith("shell_"):
        if len(arguments) != 1:
            _fail("dbus_arguments_invalid")
        return (_bounded_utf8(arguments[0], MAX_OUTPUT_NAME_BYTES),)
    if operation == "notification_get_current_state":
        if len(arguments) != 1 or _unsigned(arguments[0], UINT32_MAX) != 1:
            _fail("dbus_schema_version_invalid")
        return arguments
    if operation == "notification_set_do_not_disturb":
        if len(arguments) != 1 or not isinstance(arguments[0], bool):
            _fail("dbus_boolean_invalid")
        return arguments
    if operation == "notification_dismiss":
        if len(arguments) != 2:
            _fail("dbus_arguments_invalid")
        return tuple(_unsigned(value, UINT64_MAX, nonzero=True) for value in arguments)
    if operation == "notification_invoke_action":
        if len(arguments) != 3:
            _fail("dbus_arguments_invalid")
        action = _bounded_utf8(arguments[2], MAX_ACTION_ID_BYTES, empty=False)
        if len(action) > MAX_ACTION_ID_CODEPOINTS:
            _fail("dbus_argument_oversized")
        return (
            _unsigned(arguments[0], UINT64_MAX, nonzero=True),
            _unsigned(arguments[1], UINT64_MAX, nonzero=True),
            action,
        )
    if operation in {"session_confirm_logout", "session_cancel_logout"}:
        if len(arguments) != 1:
            _fail("dbus_arguments_invalid")
        return (_unsigned(arguments[0], UINT64_MAX, nonzero=True),)
    _fail("dbus_operation_not_reviewed")


def _validate_reply(operation: str, reply: Any) -> tuple[Any, ...]:
    if not isinstance(reply, tuple):
        _fail("dbus_reply_type_invalid")
    if operation == "settings_get_current_profile":
        if (
            len(reply) != 2
            or not isinstance(reply[0], str)
            or reply[0] not in {"lustre", "forge"}
        ):
            _fail("dbus_profile_reply_invalid")
        return (reply[0], _unsigned(reply[1], UINT64_MAX, nonzero=True))
    if operation in {
        "settings_request_profile_change",
        "settings_reload",
        "notification_set_do_not_disturb",
        "notification_clear_history",
        "notification_dismiss",
        "notification_invoke_action",
    }:
        if len(reply) != 1:
            _fail("dbus_reply_type_invalid")
        return (_unsigned(reply[0], UINT64_MAX, nonzero=True),)
    if operation == "settings_validate_candidate":
        if len(reply) != 2 or not isinstance(reply[0], bool):
            _fail("dbus_reply_type_invalid")
        diagnostics = reply[1]
        if not isinstance(diagnostics, (list, tuple)) or len(diagnostics) > MAX_DIAGNOSTICS:
            _fail("dbus_diagnostic_bound")
        checked: list[tuple[str, str, str, str, str]] = []
        maximum_codepoints = (64, 256, 64, 64, 64)
        for diagnostic in diagnostics:
            if not isinstance(diagnostic, (list, tuple)) or len(diagnostic) != 5:
                _fail("dbus_diagnostic_invalid")
            fields: list[str] = []
            for value, codepoint_limit in zip(
                diagnostic, maximum_codepoints, strict=True
            ):
                field = _bounded_utf8(value, codepoint_limit * 4)
                if len(field) > codepoint_limit:
                    _fail("dbus_diagnostic_bound")
                fields.append(field)
            checked.append(tuple(fields))
        return (reply[0], tuple(checked))
    if operation in {
        "settings_get_current_snapshot",
        "notification_get_current_state",
    }:
        if len(reply) != 2:
            _fail("dbus_reply_type_invalid")
        generation = _unsigned(reply[0], UINT64_MAX, nonzero=True)
        limit = (
            MAX_SETTINGS_BYTES
            if operation == "settings_get_current_snapshot"
            else MAX_NOTIFICATION_STATE_BYTES
        )
        schema_name = (
            "prismdrake-runtime-snapshot.schema.json"
            if operation == "settings_get_current_snapshot"
            else "prismdrake-notification-state.schema.json"
        )
        return (
            generation,
            _json_document(reply[1], limit, generation, schema_name),
        )
    if operation.startswith("shell_") or operation in {
        "session_confirm_logout",
        "session_cancel_logout",
    }:
        if reply:
            _fail("dbus_reply_type_invalid")
        return ()
    if operation == "session_get_state":
        if len(reply) != 2:
            _fail("dbus_reply_type_invalid")
        transaction = _unsigned(reply[0], UINT64_MAX)
        state = reply[1]
        if (
            not isinstance(state, str)
            or state not in SESSION_STATES
            or (state == "idle") != (transaction == 0)
        ):
            _fail("dbus_session_state_invalid")
        return (transaction, state)
    if operation == "session_request_logout":
        if len(reply) != 1:
            _fail("dbus_reply_type_invalid")
        return (_unsigned(reply[0], UINT64_MAX, nonzero=True),)
    _fail("dbus_operation_not_reviewed")


def _validate_environment(
    environment: Mapping[str, str], endpoint: PrivateBusEndpoint
) -> tuple[str, Path, Path]:
    if not isinstance(environment, Mapping) or len(environment) > MAX_ENVIRONMENT_ENTRIES:
        _fail("dbus_environment_invalid")
    checked: dict[str, str] = {}
    for key, value in environment.items():
        if (
            not isinstance(key, str)
            or not key
            or "=" in key
            or "\x00" in key
            or not isinstance(value, str)
            or "\x00" in value
        ):
            _fail("dbus_environment_invalid")
        if (
            len(key.encode("utf-8")) > 256
            or len(value.encode("utf-8")) > MAX_ENVIRONMENT_VALUE_BYTES
        ):
            _fail("dbus_environment_bound")
        checked[key] = value
    supplied = checked.get("DBUS_SESSION_BUS_ADDRESS")
    address = endpoint.address
    if not isinstance(address, str) or supplied != address:
        _fail("dbus_private_address_mismatch")
    if len(address.encode("utf-8")) > MAX_ADDRESS_BYTES or not address.startswith("unix:path="):
        _fail("dbus_private_address_invalid")
    socket_text = address.removeprefix("unix:path=")
    if (
        not socket_text
        or "," in socket_text
        or ";" in socket_text
        or "%" in socket_text
        or "\x00" in socket_text
    ):
        _fail("dbus_private_address_invalid")
    socket_path = Path(socket_text)
    runtime_text = checked.get("XDG_RUNTIME_DIR")
    if not socket_path.is_absolute() or not runtime_text:
        _fail("dbus_private_address_invalid")
    runtime_path = Path(runtime_text)
    if not runtime_path.is_absolute():
        _fail("dbus_private_address_invalid")
    if ".." in socket_path.parts or ".." in runtime_path.parts:
        _fail("dbus_private_address_invalid")
    if socket_path.parent != runtime_path:
        _fail("dbus_private_address_invalid")
    if (
        isinstance(endpoint.socket_device, bool)
        or not isinstance(endpoint.socket_device, int)
        or endpoint.socket_device < 0
        or isinstance(endpoint.socket_inode, bool)
        or not isinstance(endpoint.socket_inode, int)
        or endpoint.socket_inode <= 0
    ):
        _fail("dbus_private_socket_identity_invalid")
    return address, socket_path, runtime_path


def inspect_private_bus_socket(path: Path) -> tuple[int, int] | None:
    """Return a direct Unix-socket identity without following symlinks."""

    try:
        metadata = path.lstat()
    except OSError:
        return None
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISSOCK(metadata.st_mode):
        return None
    return metadata.st_dev, metadata.st_ino


def _validate_marshaled_reply_size(spec: MethodSpec, size: Any) -> None:
    if isinstance(size, bool) or not isinstance(size, int) or size < 0:
        _fail("dbus_reply_size_invalid")
    if (
        spec.interface == "org.prismdrake.NotificationCenter1"
        and spec.member == "GetCurrentState"
    ):
        payload_limit = MAX_NOTIFICATION_STATE_BYTES
    elif (
        spec.interface == "org.prismdrake.SettingsSnapshot1"
        and spec.member == "GetCurrentSnapshot"
    ):
        payload_limit = MAX_SETTINGS_BYTES
    elif (
        spec.interface == "org.prismdrake.Settings1"
        and spec.member == "ValidateCandidate"
    ):
        payload_limit = 65_536
    else:
        payload_limit = 4096
    if size > payload_limit + MAX_REPLY_ENVELOPE_BYTES:
        _fail("dbus_reply_oversized")


def _verify_private_endpoint(
    endpoint: PrivateBusEndpoint,
    socket_path: Path,
    runtime_path: Path,
    socket_inspector: SocketInspector,
) -> None:
    try:
        runtime_metadata = runtime_path.lstat()
        identity = socket_inspector(socket_path)
    except Exception:
        _fail("dbus_private_socket_identity_invalid")
    if (
        stat.S_ISLNK(runtime_metadata.st_mode)
        or not stat.S_ISDIR(runtime_metadata.st_mode)
        or runtime_metadata.st_uid != os.geteuid()
        or stat.S_IMODE(runtime_metadata.st_mode) != 0o700
        or identity != (endpoint.socket_device, endpoint.socket_inode)
    ):
        _fail("dbus_private_socket_identity_invalid")


class _DirectGioTransport:
    """Child-process-only Gio transport with typed variants and no auto-start."""

    def __init__(self, address: str) -> None:
        try:
            import gi

            gi.require_version("Gio", "2.0")
            from gi.repository import Gio, GLib

            flags = (
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
                | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
            )
            connection = Gio.DBusConnection.new_for_address_sync(
                address, flags, None, None
            )
            connection.set_exit_on_close(False)
        except Exception:
            _fail("dbus_private_connection_failed")
        self._gio = Gio
        self._glib = GLib
        self._connection = connection

    def call(self, spec: MethodSpec, arguments: tuple[Any, ...]) -> tuple[Any, ...]:
        try:
            parameters = self._glib.Variant(f"({spec.input_signature})", arguments)
            reply_type = self._glib.VariantType.new(f"({spec.output_signature})")
            reply = self._connection.call_sync(
                spec.service,
                spec.path,
                spec.interface,
                spec.member,
                parameters,
                reply_type,
                self._gio.DBusCallFlags.NO_AUTO_START,
                spec.timeout_milliseconds,
                None,
            )
            _validate_marshaled_reply_size(spec, reply.get_size())
            unpacked = reply.unpack()
        except LiveDbusProbeError:
            raise
        except Exception:
            _fail("dbus_call_failed")
        if not isinstance(unpacked, tuple):
            _fail("dbus_reply_type_invalid")
        return unpacked

    def close(self) -> None:
        try:
            self._connection.close_sync(None)
        except Exception:
            _fail("dbus_private_connection_close_failed")


def _gio_helper(
    sender: Any, address: str, spec: MethodSpec, arguments: tuple[Any, ...]
) -> None:
    category: str | None = None
    reply: tuple[Any, ...] | None = None
    transport: _DirectGioTransport | None = None
    try:
        transport = _DirectGioTransport(address)
        reply = transport.call(spec, arguments)
        transport.close()
        transport = None
    except LiveDbusProbeError as error:
        category = str(error)
    except Exception:
        category = "dbus_helper_failed"
    if transport is not None:
        try:
            transport.close()
        except Exception:
            category = category or "dbus_private_connection_close_failed"
    try:
        sender.send(("error", category) if category else ("ok", reply))
    except Exception:
        pass
    finally:
        sender.close()


class _BoundedGioTransport:
    """Runs Gio connect, call, and close inside one killable helper process."""

    def __init__(
        self,
        address: str,
        *,
        helper: Callable[[Any, str, MethodSpec, tuple[Any, ...]], None] = _gio_helper,
        connect_close_seconds: float = HELPER_CONNECT_CLOSE_SECONDS,
    ) -> None:
        self._address = address
        self._helper = helper
        self._connect_close_seconds = connect_close_seconds

    @staticmethod
    def _stop_helper(process: multiprocessing.Process) -> None:
        if process.is_alive():
            process.terminate()
            process.join(HELPER_STOP_SECONDS)
        if process.is_alive():
            process.kill()
            process.join(HELPER_STOP_SECONDS)

    def call(self, spec: MethodSpec, arguments: tuple[Any, ...]) -> tuple[Any, ...]:
        receiver, sender = multiprocessing.get_context("spawn").Pipe(duplex=False)
        process = multiprocessing.get_context("spawn").Process(
            target=self._helper,
            args=(sender, self._address, spec, arguments),
            daemon=True,
        )
        started = False
        try:
            process.start()
            started = True
            sender.close()
            timeout = (
                spec.timeout_milliseconds / 1000.0
                + self._connect_close_seconds
            )
            if not receiver.poll(timeout):
                self._stop_helper(process)
                _fail("dbus_helper_timeout")
            try:
                message = receiver.recv()
            except (EOFError, OSError):
                self._stop_helper(process)
                _fail("dbus_helper_failed")
            process.join(HELPER_STOP_SECONDS)
            if process.is_alive():
                self._stop_helper(process)
                _fail("dbus_helper_timeout")
        finally:
            receiver.close()
            sender.close()
            if started and process.is_alive():
                self._stop_helper(process)
        if (
            not isinstance(message, tuple)
            or len(message) != 2
            or message[0] not in {"ok", "error"}
        ):
            _fail("dbus_helper_failed")
        if message[0] == "error":
            category = message[1]
            if not isinstance(category, str) or category not in CLOSED_ERROR_CATEGORIES:
                _fail("dbus_helper_failed")
            _fail(category)
        if not isinstance(message[1], tuple):
            _fail("dbus_reply_type_invalid")
        return message[1]

    def close(self) -> None:
        # Each call owns and closes its exact child connection before replying.
        return


def _error_category(error: Exception, fallback: str) -> str:
    if isinstance(error, LiveDbusProbeError):
        category = str(error)
        if category in CLOSED_ERROR_CATEGORIES:
            return category
    return fallback


def _raise_public(category: str) -> None:
    error = LiveDbusProbeError(category)
    error.__cause__ = None
    error.__context__ = None
    raise error from None


class LiveDbusProbe:
    """Closed, typed probe bound to one caller-attested private bus epoch.

    Gio connects by pathname and cannot atomically select a Unix-socket inode.
    The adapter therefore owns the mode-0700 runtime and this probe rejects any
    identity change before or after a call.  A detected same-UID path race
    invalidates evidence; this harness does not claim the rejected call was
    never delivered.
    """

    def __init__(
        self,
        environment: Mapping[str, str],
        *,
        endpoint: PrivateBusEndpoint,
        transport_factory: TransportFactory | None = None,
        socket_inspector: SocketInspector = inspect_private_bus_socket,
    ) -> None:
        failure: str | None = None
        try:
            address, socket_path, runtime_path = _validate_environment(
                environment, endpoint
            )
            _verify_private_endpoint(
                endpoint, socket_path, runtime_path, socket_inspector
            )
            factory = transport_factory or _BoundedGioTransport
            transport = factory(address)
        except Exception as error:
            failure = _error_category(error, "dbus_private_connection_failed")
        if failure is not None:
            _raise_public(failure)
        self._endpoint = endpoint
        self._socket_path = socket_path
        self._runtime_path = runtime_path
        self._socket_inspector = socket_inspector
        self._transport = transport
        self._closed = False

    def call(self, operation: str, *arguments: Any) -> ProbeResult:
        failure: str | None = None
        result: ProbeResult | None = None
        try:
            if self._closed:
                _fail("dbus_probe_closed")
            spec = METHODS.get(operation)
            if spec is None:
                _fail("dbus_operation_not_reviewed")
            checked_arguments = _validate_arguments(operation, tuple(arguments))
            _verify_private_endpoint(
                self._endpoint,
                self._socket_path,
                self._runtime_path,
                self._socket_inspector,
            )
            reply = self._transport.call(spec, checked_arguments)
            _verify_private_endpoint(
                self._endpoint,
                self._socket_path,
                self._runtime_path,
                self._socket_inspector,
            )
            payload = _validate_reply(operation, reply)
            result = ProbeResult(operation, spec.category, payload)
        except Exception as error:
            failure = _error_category(error, "dbus_call_failed")
        if failure is not None:
            _raise_public(failure)
        if result is None:
            _raise_public("dbus_call_failed")
        return result

    def get_property(self, operation: str) -> ProbeResult:
        del operation
        _fail("dbus_property_not_reviewed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        failure: str | None = None
        try:
            self._transport.close()
        except Exception as error:
            failure = _error_category(
                error, "dbus_private_connection_close_failed"
            )
        if failure is not None:
            _raise_public(failure)

    def __enter__(self) -> "LiveDbusProbe":
        return self

    def __exit__(self, exception_type: Any, exception: Any, traceback: Any) -> None:
        del exception_type, exception, traceback
        self.close()


def contract_manifest() -> frozenset[
    tuple[str, str, str, str, str, str, int, str]
]:
    """Return every fixed routing, signature, timeout, and category field."""

    return frozenset(
        (
            spec.service,
            spec.path,
            spec.interface,
            spec.member,
            spec.input_signature,
            spec.output_signature,
            spec.timeout_milliseconds,
            spec.category,
        )
        for spec in METHODS.values()
    )


def validate_evidence(result: ProbeResult) -> None:
    """Validate serialization shape, not provenance or execution authenticity."""

    spec = METHODS.get(result.operation) if isinstance(result, ProbeResult) else None
    if (
        spec is None
        or result.category != spec.category
        or result.status != "passed"
        or result.evidence() != {"category": spec.category, "status": "passed"}
    ):
        _fail("dbus_evidence_invalid")
