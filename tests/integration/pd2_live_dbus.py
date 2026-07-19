#!/usr/bin/env python3
"""Strict typed D-Bus probes for the installed PD2 live demonstration.

The probe connects only to the caller-attested private session bus.  Public
operations are a closed mirror of the current Experimental XML contracts; no
arbitrary destination, object, interface, member, property, or signature is
accepted.  Returned evidence is categorical and never serializes reply data.
"""

from __future__ import annotations

import json
import os
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
MAX_DIAGNOSTICS = 64
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1

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
    raise LiveDbusProbeError(category)


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


def _json_document(value: Any, maximum: int, generation: int) -> dict[str, Any]:
    payload = _octets(value, maximum)

    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, item in pairs:
            if key in result:
                _fail("dbus_json_duplicate_key")
            result[key] = item
        return result

    try:
        document = json.loads(payload.decode("utf-8"), object_pairs_hook=unique_object)
    except LiveDbusProbeError:
        raise
    except (UnicodeError, json.JSONDecodeError):
        _fail("dbus_json_invalid")
    embedded_generation = document.get("generation") if isinstance(document, dict) else None
    if (
        isinstance(embedded_generation, bool)
        or not isinstance(embedded_generation, int)
        or embedded_generation != generation
    ):
        _fail("dbus_json_generation_invalid")
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
        return (generation, _json_document(reply[1], limit, generation))
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
    environment: Mapping[str, str], address: str | None
) -> str:
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
    if address is None:
        address = supplied
    if not isinstance(address, str) or supplied != address:
        _fail("dbus_private_address_mismatch")
    if len(address.encode("utf-8")) > MAX_ADDRESS_BYTES or not address.startswith("unix:path="):
        _fail("dbus_private_address_invalid")
    socket_text = address.removeprefix("unix:path=")
    if not socket_text or "," in socket_text or ";" in socket_text or "\x00" in socket_text:
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
    try:
        if os.path.commonpath((str(runtime_path), str(socket_path))) != str(runtime_path):
            _fail("dbus_private_address_invalid")
    except ValueError:
        _fail("dbus_private_address_invalid")
    return address


class GioTransport:
    """Exact-address Gio transport with typed variants and no auto-start."""

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
        except Exception as error:
            raise LiveDbusProbeError("dbus_private_connection_failed") from error
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
            unpacked = reply.unpack()
        except Exception as error:
            raise LiveDbusProbeError("dbus_call_failed") from error
        if not isinstance(unpacked, tuple):
            _fail("dbus_reply_type_invalid")
        return unpacked

    def close(self) -> None:
        try:
            self._connection.close_sync(None)
        except Exception as error:
            raise LiveDbusProbeError("dbus_private_connection_close_failed") from error


class LiveDbusProbe:
    """Closed, typed probe bound to one caller-supplied private bus epoch."""

    def __init__(
        self,
        environment: Mapping[str, str],
        *,
        address: str | None = None,
        transport_factory: TransportFactory = GioTransport,
    ) -> None:
        private_address = _validate_environment(environment, address)
        try:
            self._transport = transport_factory(private_address)
        except LiveDbusProbeError:
            raise
        except Exception as error:
            raise LiveDbusProbeError("dbus_private_connection_failed") from error
        self._closed = False

    def call(self, operation: str, *arguments: Any) -> ProbeResult:
        if self._closed:
            _fail("dbus_probe_closed")
        spec = METHODS.get(operation)
        if spec is None:
            _fail("dbus_operation_not_reviewed")
        checked_arguments = _validate_arguments(operation, tuple(arguments))
        try:
            reply = self._transport.call(spec, checked_arguments)
        except LiveDbusProbeError:
            raise
        except Exception as error:
            raise LiveDbusProbeError("dbus_call_failed") from error
        payload = _validate_reply(operation, reply)
        return ProbeResult(operation, spec.category, payload)

    def get_property(self, operation: str) -> ProbeResult:
        del operation
        _fail("dbus_property_not_reviewed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._transport.close()

    def __enter__(self) -> "LiveDbusProbe":
        return self

    def __exit__(self, exception_type: Any, exception: Any, traceback: Any) -> None:
        del exception_type, exception, traceback
        self.close()


def contract_manifest() -> frozenset[tuple[str, str, str, str]]:
    """Return the exact interface/member/signature surface under review."""

    return frozenset(
        (
            spec.interface,
            spec.member,
            spec.input_signature,
            spec.output_signature,
        )
        for spec in METHODS.values()
    )


def validate_evidence(result: ProbeResult) -> None:
    """Reject anything except a fixed categorical result serialization."""

    spec = METHODS.get(result.operation) if isinstance(result, ProbeResult) else None
    if (
        spec is None
        or result.category != spec.category
        or result.status != "passed"
        or result.evidence() != {"category": spec.category, "status": "passed"}
    ):
        _fail("dbus_evidence_invalid")
