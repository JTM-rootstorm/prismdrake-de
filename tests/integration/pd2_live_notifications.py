#!/usr/bin/env python3
"""Persistent, bounded notification sender for the installed PD2 demonstration."""

from __future__ import annotations

import time
from collections import deque
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any, Protocol


ACTION_KEY = "pd2-action"
ACTION_LABEL = "Review"
APPLICATION_NAME = "Prismdrake test sender"
BODY = "Controlled notification body"
NORMAL_SUMMARY = "Controlled notification"
REPLACEMENT_SUMMARY = "Controlled replacement"
CRITICAL_SUMMARY = "Controlled critical notification"

MAX_ACTIVE_NOTIFICATIONS = 8
MAX_BUS_ADDRESS_BYTES = 4096
MAX_DISPATCH_ITERATIONS = 64
MAX_NOTIFICATION_OPERATIONS = 32
MAX_PENDING_SIGNALS = 32
MAX_UNIQUE_NAME_BYTES = 255
MIN_WAIT_SECONDS = 0.01
MAX_WAIT_SECONDS = 5.0
METHOD_TIMEOUT_MS = 1000
SIGNAL_BOUNDARY_TIMEOUT_SECONDS = 0.25
UINT32_MAX = (1 << 32) - 1

STANDARD_BUS_NAME = "org.freedesktop.Notifications"
STANDARD_INTERFACE = "org.freedesktop.Notifications"
STANDARD_PATH = "/org/freedesktop/Notifications"

RESULT_CATEGORIES = frozenset(
    {
        "notification_action_observed",
        "notification_closed_observed",
        "notification_created",
        "notification_critical_created",
        "notification_replaced",
    }
)


class LiveNotificationError(RuntimeError):
    """A notification probe operation failed with a redacted category."""


@dataclass(frozen=True)
class CategoricalResult:
    """A serialization-safe result that contains no notification content."""

    category: str
    status: str = "passed"

    def as_dict(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


@dataclass(frozen=True, repr=False)
class NotificationToken:
    """Opaque identity for a notification created by one probe."""

    _probe_identity: object
    _serial: int
    _live_id: int
    _content_generation: int
    _signal_generation: int


@dataclass(frozen=True, repr=False)
class NotificationSignal:
    """One validated standard signal retained only inside the live probe."""

    kind: str
    notification_id: int
    detail: str | int
    generation: int


class NotificationTransport(Protocol):
    """Persistent transport boundary used by the policy wrapper."""

    def notify(self, replaces_id: int, summary: str, urgency: int) -> int: ...

    def take_notify_reply_latency_ns(self) -> int: ...

    def take_signals(self) -> Sequence[NotificationSignal]: ...

    def advance_signal_generation(self, deadline: float) -> int: ...

    def dispatch(self, deadline: float) -> None: ...

    def close(self) -> None: ...


def _fail(category: str) -> None:
    raise LiveNotificationError(category)


def _uint32(value: Any, category: str, *, allow_zero: bool = False) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < (0 if allow_zero else 1)
        or value > UINT32_MAX
    ):
        _fail(category)
    return value


def _private_bus_address(environment: Mapping[str, str]) -> str:
    if not isinstance(environment, Mapping):
        _fail("notification_private_bus_environment_invalid")
    address = environment.get("DBUS_SESSION_BUS_ADDRESS")
    if (
        not isinstance(address, str)
        or not address.startswith("unix:path=/")
        or len(address.encode("utf-8")) > MAX_BUS_ADDRESS_BYTES
        or any(ord(character) < 0x20 for character in address)
        or "," in address
        or ";" in address
    ):
        _fail("notification_private_bus_address_invalid")
    return address


def validate_categorical_result(result: CategoricalResult) -> None:
    """Reject any result that could carry live or sender-controlled values."""

    if (
        not isinstance(result, CategoricalResult)
        or result.category not in RESULT_CATEGORIES
        or result.status != "passed"
        or set(result.as_dict()) != {"category", "status"}
    ):
        _fail("notification_evidence_invalid")


def _validate_signal(signal: NotificationSignal) -> None:
    _uint32(signal.notification_id, "notification_signal_invalid")
    _uint32(signal.generation, "notification_signal_invalid", allow_zero=True)
    if signal.kind == "action":
        if signal.detail != ACTION_KEY:
            _fail("notification_signal_invalid")
        return
    if signal.kind == "closed":
        reason = _uint32(signal.detail, "notification_signal_invalid")
        if reason > 4:
            _fail("notification_signal_invalid")
        return
    _fail("notification_signal_invalid")


class GioNotificationTransport:
    """One explicit private-bus Gio connection kept alive for signal routing."""

    def __init__(
        self,
        gio: Any,
        glib: Any,
        connection: Any,
        context: Any,
        *,
        clock_ns: Any = time.monotonic_ns,
    ) -> None:
        self._gio = gio
        self._glib = glib
        self._connection = connection
        self._context = context
        self._clock_ns = clock_ns
        self._last_notify_reply_latency_ns: int | None = None
        self._signals: deque[NotificationSignal] = deque()
        self._signal_error: str | None = None
        self._signal_generation = 0
        self._closed = False
        subscriptions: list[int] = []
        try:
            subscriptions.append(
                connection.signal_subscribe(
                    STANDARD_BUS_NAME,
                    STANDARD_INTERFACE,
                    "ActionInvoked",
                    STANDARD_PATH,
                    None,
                    gio.DBusSignalFlags.NONE,
                    self._on_signal,
                    None,
                )
            )
            subscriptions.append(
                connection.signal_subscribe(
                    STANDARD_BUS_NAME,
                    STANDARD_INTERFACE,
                    "NotificationClosed",
                    STANDARD_PATH,
                    None,
                    gio.DBusSignalFlags.NONE,
                    self._on_signal,
                    None,
                )
            )
        except Exception:
            for subscription in subscriptions:
                try:
                    connection.signal_unsubscribe(subscription)
                except Exception:
                    pass
            try:
                connection.close_sync(None)
            except Exception:
                pass
            _fail("notification_signal_subscription_failed")
        self._subscriptions = tuple(subscriptions)

    @classmethod
    def connect(
        cls,
        environment: Mapping[str, str],
        gio: Any,
        glib: Any,
        *,
        clock_ns: Any = time.monotonic_ns,
    ) -> "GioNotificationTransport":
        """Connect to only the private address supplied by the harness."""

        address = _private_bus_address(environment)
        connection = None
        context = None
        pushed = False
        transport = None
        try:
            context = glib.MainContext.new()
            if context is None:
                raise RuntimeError
            context.push_thread_default()
            pushed = True
            flags = (
                gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
                | gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
            )
            connection = gio.DBusConnection.new_for_address_sync(
                address, flags, None, None
            )
            unique_name = connection.get_unique_name()
        except Exception:
            if connection is not None:
                try:
                    connection.close_sync(None)
                except Exception:
                    pass
            if pushed:
                try:
                    context.pop_thread_default()
                except Exception:
                    pass
            _fail("notification_private_bus_connection_failed")
        if (
            not isinstance(unique_name, str)
            or not unique_name.startswith(":")
            or len(unique_name.encode("utf-8")) > MAX_UNIQUE_NAME_BYTES
        ):
            try:
                connection.close_sync(None)
            except Exception:
                pass
            try:
                context.pop_thread_default()
            except Exception:
                pass
            _fail("notification_private_bus_identity_invalid")
        try:
            transport = cls(gio, glib, connection, context, clock_ns=clock_ns)
        finally:
            try:
                context.pop_thread_default()
            except Exception:
                if transport is not None:
                    transport.close()
                else:
                    try:
                        connection.close_sync(None)
                    except Exception:
                        pass
                _fail("notification_private_bus_connection_failed")
        return transport

    def notify(self, replaces_id: int, summary: str, urgency: int) -> int:
        if self._closed:
            _fail("notification_transport_closed")
        _uint32(replaces_id, "notification_replacement_id_invalid", allow_zero=True)
        if summary not in {NORMAL_SUMMARY, REPLACEMENT_SUMMARY, CRITICAL_SUMMARY}:
            _fail("notification_fixture_content_invalid")
        if urgency not in {1, 2}:
            _fail("notification_urgency_invalid")
        try:
            parameters = self._glib.Variant(
                "(susssasa{sv}i)",
                (
                    APPLICATION_NAME,
                    replaces_id,
                    "",
                    summary,
                    BODY,
                    [ACTION_KEY, ACTION_LABEL],
                    {"urgency": self._glib.Variant("y", urgency)},
                    0,
                ),
            )
            self._last_notify_reply_latency_ns = None
            started_ns = self._clock_ns()
            reply = self._connection.call_sync(
                STANDARD_BUS_NAME,
                STANDARD_PATH,
                STANDARD_INTERFACE,
                "Notify",
                parameters,
                self._glib.VariantType.new("(u)"),
                self._gio.DBusCallFlags.NO_AUTO_START,
                METHOD_TIMEOUT_MS,
                None,
            )
            unpacked = reply.unpack()
        except Exception:
            _fail("notification_notify_failed")
        if not isinstance(unpacked, tuple) or len(unpacked) != 1:
            _fail("notification_notify_reply_invalid")
        notification_id = _uint32(unpacked[0], "notification_notify_reply_invalid")
        finished_ns = self._clock_ns()
        latency_ns = finished_ns - started_ns
        if (
            not isinstance(latency_ns, int)
            or not 0 <= latency_ns <= METHOD_TIMEOUT_MS * 1_000_000
        ):
            _fail("notification_notify_latency_invalid")
        self._last_notify_reply_latency_ns = latency_ns
        return notification_id

    def take_notify_reply_latency_ns(self) -> int:
        latency_ns = self._last_notify_reply_latency_ns
        self._last_notify_reply_latency_ns = None
        if latency_ns is None:
            _fail("notification_notify_latency_unavailable")
        return latency_ns

    def _on_signal(
        self,
        _connection: Any,
        _sender_name: str,
        _object_path: str,
        _interface_name: str,
        signal_name: str,
        parameters: Any,
        _user_data: Any,
    ) -> None:
        if self._closed:
            return
        try:
            values = parameters.unpack()
            if signal_name == "ActionInvoked":
                if (
                    not isinstance(values, tuple)
                    or len(values) != 2
                    or not isinstance(values[1], str)
                    or values[1] != ACTION_KEY
                ):
                    raise ValueError
                event = NotificationSignal(
                    "action",
                    _uint32(values[0], "notification_signal_invalid"),
                    ACTION_KEY,
                    self._signal_generation,
                )
            elif signal_name == "NotificationClosed":
                if not isinstance(values, tuple) or len(values) != 2:
                    raise ValueError
                reason = _uint32(values[1], "notification_signal_invalid")
                if reason > 4:
                    raise ValueError
                event = NotificationSignal(
                    "closed",
                    _uint32(values[0], "notification_signal_invalid"),
                    reason,
                    self._signal_generation,
                )
            else:
                raise ValueError
        except Exception:
            self._signal_error = "notification_signal_invalid"
            return
        if len(self._signals) >= MAX_PENDING_SIGNALS:
            self._signal_error = "notification_signal_bound"
            return
        self._signals.append(event)

    def take_signals(self) -> Sequence[NotificationSignal]:
        if self._signal_error is not None:
            _fail(self._signal_error)
        signals = tuple(self._signals)
        self._signals.clear()
        return signals

    def advance_signal_generation(self, deadline: float) -> int:
        self.dispatch(deadline)
        if self._signal_generation >= UINT32_MAX:
            _fail("notification_signal_generation_bound")
        self._signal_generation += 1
        return self._signal_generation

    def dispatch(self, deadline: float) -> None:
        if self._closed:
            _fail("notification_transport_closed")
        if isinstance(deadline, bool) or not isinstance(deadline, (int, float)):
            _fail("notification_signal_deadline_invalid")
        try:
            for _iteration in range(MAX_DISPATCH_ITERATIONS):
                if time.monotonic() >= deadline:
                    _fail("notification_signal_dispatch_timeout")
                if not self._context.pending():
                    return
                self._context.iteration(False)
            if self._context.pending():
                _fail("notification_signal_dispatch_bound")
        except LiveNotificationError:
            raise
        except Exception:
            _fail("notification_signal_dispatch_failed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        for subscription in self._subscriptions:
            try:
                self._connection.signal_unsubscribe(subscription)
            except Exception:
                pass
        try:
            self._connection.close_sync(None)
        except Exception:
            pass


class PersistentNotificationProbe:
    """Bounded notification workflow over one persistent sender connection."""

    def __init__(self, transport: NotificationTransport) -> None:
        self._transport = transport
        self._identity = object()
        self._tokens: dict[int, NotificationToken] = {}
        self._next_serial = 1
        self._operations = 0
        self._signal_generation = 0
        self._closed = False
        self._pending_signals: deque[NotificationSignal] = deque()

    @classmethod
    def connect_private(
        cls, environment: Mapping[str, str], gio: Any, glib: Any
    ) -> "PersistentNotificationProbe":
        return cls(GioNotificationTransport.connect(environment, gio, glib))

    def create_normal(self) -> tuple[NotificationToken, CategoricalResult]:
        return self._create(NORMAL_SUMMARY, 1, "notification_created")

    def take_notify_reply_latency_ns(self) -> int:
        self._ensure_open()
        try:
            latency_ns = self._transport.take_notify_reply_latency_ns()
        except Exception:
            _fail("notification_notify_latency_unavailable")
        if (
            isinstance(latency_ns, bool)
            or not isinstance(latency_ns, int)
            or not 0 <= latency_ns <= METHOD_TIMEOUT_MS * 1_000_000
        ):
            _fail("notification_notify_latency_invalid")
        return latency_ns

    def create_critical(self) -> tuple[NotificationToken, CategoricalResult]:
        return self._create(CRITICAL_SUMMARY, 2, "notification_critical_created")

    def _create(
        self, summary: str, urgency: int, category: str
    ) -> tuple[NotificationToken, CategoricalResult]:
        self._ensure_open()
        if len(self._tokens) >= MAX_ACTIVE_NOTIFICATIONS:
            _fail("notification_fixture_capacity")
        self._count_operation()
        live_id = self._transport.notify(0, summary, urgency)
        _uint32(live_id, "notification_notify_reply_invalid")
        if any(token._live_id == live_id for token in self._tokens.values()):
            _fail("notification_live_id_duplicate")
        signal_generation = self._advance_signal_boundary()
        token = NotificationToken(
            self._identity,
            self._next_serial,
            live_id,
            1,
            signal_generation,
        )
        self._tokens[token._serial] = token
        self._prune_pending_signals()
        self._next_serial += 1
        result = CategoricalResult(category)
        validate_categorical_result(result)
        return token, result

    def replace(
        self, token: NotificationToken
    ) -> tuple[NotificationToken, CategoricalResult]:
        current = self._current_token(token)
        self._count_operation()
        live_id = self._transport.notify(
            current._live_id, REPLACEMENT_SUMMARY, 1
        )
        _uint32(live_id, "notification_notify_reply_invalid")
        if live_id != current._live_id:
            _fail("notification_replacement_identity")
        signal_generation = self._advance_signal_boundary()
        replacement = NotificationToken(
            self._identity,
            current._serial,
            current._live_id,
            current._content_generation + 1,
            signal_generation,
        )
        self._tokens[current._serial] = replacement
        self._prune_pending_signals()
        result = CategoricalResult("notification_replaced")
        validate_categorical_result(result)
        return replacement, result

    def wait_for_action(
        self, token: NotificationToken, timeout_seconds: float
    ) -> CategoricalResult:
        current = self._current_token(token)
        self._wait_for_signal(
            "action",
            current._live_id,
            current._signal_generation,
            timeout_seconds,
        )
        result = CategoricalResult("notification_action_observed")
        validate_categorical_result(result)
        return result

    def wait_for_close(
        self, token: NotificationToken, timeout_seconds: float
    ) -> CategoricalResult:
        current = self._current_token(token)
        self._wait_for_signal(
            "closed",
            current._live_id,
            current._signal_generation,
            timeout_seconds,
        )
        del self._tokens[current._serial]
        result = CategoricalResult("notification_closed_observed")
        validate_categorical_result(result)
        return result

    def _wait_for_signal(
        self,
        kind: str,
        notification_id: int,
        signal_generation: int,
        timeout_seconds: float,
    ) -> NotificationSignal:
        if (
            isinstance(timeout_seconds, bool)
            or not isinstance(timeout_seconds, (int, float))
            or not MIN_WAIT_SECONDS <= timeout_seconds <= MAX_WAIT_SECONDS
        ):
            _fail("notification_wait_timeout_invalid")
        deadline = time.monotonic() + float(timeout_seconds)
        while True:
            self._transport.dispatch(deadline)
            self._collect_signals()
            for index, signal in enumerate(self._pending_signals):
                if (
                    signal.kind == kind
                    and signal.notification_id == notification_id
                    and signal.generation >= signal_generation
                ):
                    del self._pending_signals[index]
                    return signal
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _fail("notification_signal_timeout")
            time.sleep(min(MIN_WAIT_SECONDS, remaining))

    def _advance_signal_boundary(self) -> int:
        deadline = time.monotonic() + SIGNAL_BOUNDARY_TIMEOUT_SECONDS
        generation = self._transport.advance_signal_generation(deadline)
        _uint32(generation, "notification_signal_generation_invalid")
        if generation != self._signal_generation + 1:
            _fail("notification_signal_generation_invalid")
        self._signal_generation = generation
        self._collect_signals()
        return generation

    def _collect_signals(self) -> None:
        incoming = self._transport.take_signals()
        if len(incoming) > MAX_PENDING_SIGNALS:
            _fail("notification_signal_bound")
        for signal in incoming:
            if not isinstance(signal, NotificationSignal):
                _fail("notification_signal_invalid")
            _validate_signal(signal)
            if signal.generation > self._signal_generation:
                _fail("notification_signal_generation_invalid")
            if len(self._pending_signals) >= MAX_PENDING_SIGNALS:
                _fail("notification_signal_bound")
            self._pending_signals.append(signal)

    def _prune_pending_signals(self) -> None:
        current_identities = {
            token._live_id: token._signal_generation for token in self._tokens.values()
        }
        self._pending_signals = deque(
            signal
            for signal in self._pending_signals
            if signal.notification_id in current_identities
            and signal.generation >= current_identities[signal.notification_id]
        )

    def _current_token(self, token: NotificationToken) -> NotificationToken:
        self._ensure_open()
        if (
            not isinstance(token, NotificationToken)
            or token._probe_identity is not self._identity
            or self._tokens.get(token._serial) != token
        ):
            _fail("notification_token_stale")
        return token

    def _ensure_open(self) -> None:
        if self._closed:
            _fail("notification_probe_closed")

    def _count_operation(self) -> None:
        if self._operations >= MAX_NOTIFICATION_OPERATIONS:
            _fail("notification_operation_bound")
        self._operations += 1

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._tokens.clear()
        self._pending_signals.clear()
        self._transport.close()

    def __enter__(self) -> "PersistentNotificationProbe":
        self._ensure_open()
        return self

    def __exit__(self, _kind: Any, _value: Any, _traceback: Any) -> None:
        self.close()
