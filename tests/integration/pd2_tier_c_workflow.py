#!/usr/bin/env python3
"""Ordered, privacy-safe Tier C fallback replay for the installed PD2 harness."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, Protocol

from pd2_live_atspi import (
    CategoricalResult as AtspiResult,
    ControlSelector,
    validate_categorical_result as validate_atspi_result,
)
from pd2_x11trace import X11TraceSummary


TIER_C_STEPS = (
    "desktop",
    "panel",
    "launcher",
    "task_actions",
    "notification",
    "notification_center",
    "quick_settings",
    "logout",
)
TIER_C_ASSERTIONS = (
    "opaque_materials",
    "no_shell_side_capture",
    "no_blur_attempt",
    "complete_keyboard_operation",
    "readable_text_and_visible_focus",
)
FINGERPRINT_BYTES = 32
REVIEWED_SAMPLE_COUNT = 4
XWD_SAMPLE_FINGERPRINT_KIND = "xwd_sha256_reviewed_samples_v1"
CONTROLLED_APPLICATION_NAME = "Prismdrake Controlled Application"

_KEYS = frozenset(
    {"SUPER_D", "TAB", "SHIFT_TAB", "ENTER", "ESCAPE", "SPACE", "MENU", "DOWN"}
)
_RESULT_CATEGORIES = frozenset(
    {
        "tier_c_clients_cleaned",
        "tier_c_keyboard_complete",
        "tier_c_material_observed",
        "tier_c_step_complete",
        "tier_c_step_prepared",
    }
)
_ERROR_CATEGORIES = frozenset(
    {
        "tier_c_atspi_invalid",
        "tier_c_cleanup_failed",
        "tier_c_cleanup_order_invalid",
        "tier_c_contract_invalid",
        "tier_c_keyboard_invalid",
        "tier_c_material_invalid",
        "tier_c_order_invalid",
        "tier_c_port_result_invalid",
        "tier_c_step_failed",
        "tier_c_trace_incomplete",
        "tier_c_trace_invalid",
    }
)


class TierCWorkflowError(RuntimeError):
    """A fixed, redacted Tier C workflow failure."""


@dataclass(frozen=True, slots=True)
class TierCStepContract:
    """One exact surface, keyboard, AT-SPI, and reviewed-sample contract."""

    identifier: str
    keys: tuple[str, ...]
    surface_selector: ControlSelector
    focus_selector: ControlSelector
    readable_selector: ControlSelector
    sample_points: tuple[tuple[int, int], ...]


@dataclass(frozen=True, slots=True)
class PortResult:
    """Narrow internal categorical result returned by the live adapter."""

    category: str
    status: str = "passed"


@dataclass(frozen=True, slots=True)
class KeyboardObservation:
    """Exact accepted key count, hidden from public evidence."""

    category: str
    accepted_key_count: int = field(repr=False)
    status: str = "passed"


@dataclass(frozen=True, slots=True)
class MaterialObservation:
    """Settled material and reference fingerprints, never raw pixels."""

    category: str
    sample_count: int
    fingerprint_kind: str
    first_fingerprint: bytes = field(repr=False)
    settled_fingerprint: bytes = field(repr=False)
    transparent_reference_fingerprint: bytes = field(repr=False)
    status: str = "passed"


@dataclass(frozen=True, slots=True)
class ClientCleanupObservation:
    """Exact Tier C client cleanup state, without process identities."""

    category: str
    clients_exited: bool
    status: str = "passed"


@dataclass(frozen=True, slots=True)
class TierCStepResult:
    """Serialization-safe result for one exact step."""

    category: str = "tier_c_step_observed"
    status: str = "passed"

    def as_dict(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


@dataclass(frozen=True, slots=True)
class TierCCleanupResult:
    """Serialization-safe finalization result after trace validation."""

    category: str = "tier_c_replay_finalized"
    status: str = "passed"

    def as_dict(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


class TierCReplayPort(Protocol):
    """Non-vacuous live boundary invoked separately for every assertion."""

    def prepare_step(self, contract: TierCStepContract) -> PortResult: ...

    def send_reviewed_keys(
        self, identifier: str, keys: tuple[str, ...]
    ) -> KeyboardObservation: ...

    def observe_atspi(self, selector: ControlSelector) -> AtspiResult: ...

    def capture_reviewed_material(
        self, identifier: str, sample_points: tuple[tuple[int, int], ...]
    ) -> MaterialObservation: ...

    def complete_step(self, identifier: str) -> PortResult: ...

    def cleanup_clients(self) -> ClientCleanupObservation: ...


class TraceSupervisorPort(Protocol):
    @property
    def finished(self) -> bool: ...

    def poll(self) -> None: ...

    def finish(self, *, clients_exited: bool) -> X11TraceSummary: ...


_POINTS = ((8, 8), (16, 8), (8, 16), (16, 16))
STEP_CONTRACTS: Mapping[str, TierCStepContract] = MappingProxyType({
    "desktop": TierCStepContract(
        "desktop",
        ("SUPER_D", "TAB"),
        ControlSelector("Open applications", "push_button"),
        ControlSelector("Open applications", "push_button", require_focused=True),
        ControlSelector("Open applications", "push_button"),
        _POINTS,
    ),
    "panel": TierCStepContract(
        "panel",
        ("TAB",),
        ControlSelector("Open applications", "push_button"),
        ControlSelector("Open applications", "push_button", require_focused=True),
        ControlSelector("Open notification center", "push_button"),
        _POINTS,
    ),
    "launcher": TierCStepContract(
        "launcher",
        ("ENTER",),
        ControlSelector("Applications", "pane"),
        ControlSelector("Search applications", "editable_text", require_focused=True),
        ControlSelector("Applications", "pane"),
        _POINTS,
    ),
    "task_actions": TierCStepContract(
        "task_actions",
        ("ESCAPE", "TAB", "MENU", "ESCAPE"),
        ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
        ControlSelector(
            CONTROLLED_APPLICATION_NAME, "push_button", require_focused=True
        ),
        ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
        _POINTS,
    ),
    "notification": TierCStepContract(
        "notification",
        ("TAB", "ENTER"),
        ControlSelector("Transient notifications", "notification"),
        ControlSelector(
            "Open notification center", "push_button", require_focused=True
        ),
        ControlSelector("Transient notifications", "notification"),
        _POINTS,
    ),
    "notification_center": TierCStepContract(
        "notification_center",
        ("ENTER", "TAB"),
        ControlSelector("Notification center", "pane"),
        ControlSelector("Do not disturb", "check_box", require_focused=True),
        ControlSelector("Notification center", "pane"),
        _POINTS,
    ),
    "quick_settings": TierCStepContract(
        "quick_settings",
        ("ESCAPE", "SHIFT_TAB", "ENTER", "TAB"),
        ControlSelector("Quick settings", "pane"),
        ControlSelector("Request logout", "push_button", require_focused=True),
        ControlSelector("Quick settings", "pane"),
        _POINTS,
    ),
    "logout": TierCStepContract(
        "logout",
        ("ENTER", "TAB"),
        ControlSelector("Log out of Prismdrake", "dialog"),
        ControlSelector("Cancel logout", "push_button", require_focused=True),
        ControlSelector("Log out of Prismdrake", "dialog"),
        _POINTS,
    ),
})


def _fail(category: str) -> None:
    if category not in _ERROR_CATEGORIES:
        category = "tier_c_step_failed"
    error = TierCWorkflowError(category)
    error.__cause__ = None
    error.__context__ = None
    raise error from None


def _validate_contracts() -> None:
    if tuple(STEP_CONTRACTS) != TIER_C_STEPS:
        _fail("tier_c_contract_invalid")
    for identifier, contract in STEP_CONTRACTS.items():
        if (
            not isinstance(contract, TierCStepContract)
            or contract.identifier != identifier
            or not contract.keys
            or len(contract.keys) > 8
            or any(key not in _KEYS for key in contract.keys)
            or len(contract.sample_points) != REVIEWED_SAMPLE_COUNT
            or len(set(contract.sample_points)) != REVIEWED_SAMPLE_COUNT
            or any(
                type(x) is not int
                or type(y) is not int
                or not 0 <= x <= 4096
                or not 0 <= y <= 4096
                for x, y in contract.sample_points
            )
            or not contract.focus_selector.require_focused
        ):
            _fail("tier_c_contract_invalid")


def _validate_port_result(result: Any, category: str) -> None:
    if (
        not isinstance(result, PortResult)
        or category not in _RESULT_CATEGORIES
        or result.category != category
        or result.status != "passed"
    ):
        _fail("tier_c_port_result_invalid")


def _validate_keyboard(result: Any, key_count: int) -> None:
    if (
        not isinstance(result, KeyboardObservation)
        or result.category != "tier_c_keyboard_complete"
        or result.status != "passed"
        or isinstance(result.accepted_key_count, bool)
        or result.accepted_key_count != key_count
    ):
        _fail("tier_c_keyboard_invalid")


def _valid_fingerprint(value: Any) -> bool:
    return isinstance(value, bytes) and len(value) == FINGERPRINT_BYTES


def _validate_material(result: Any) -> None:
    if (
        not isinstance(result, MaterialObservation)
        or result.category != "tier_c_material_observed"
        or result.status != "passed"
        or result.sample_count != REVIEWED_SAMPLE_COUNT
        or result.fingerprint_kind != XWD_SAMPLE_FINGERPRINT_KIND
        or not _valid_fingerprint(result.first_fingerprint)
        or not _valid_fingerprint(result.settled_fingerprint)
        or not _valid_fingerprint(result.transparent_reference_fingerprint)
        or result.first_fingerprint != result.settled_fingerprint
        or result.first_fingerprint == result.transparent_reference_fingerprint
    ):
        _fail("tier_c_material_invalid")


def _validate_atspi(result: Any) -> None:
    try:
        validate_atspi_result(result)
    except Exception:
        _fail("tier_c_atspi_invalid")
    if not isinstance(result, AtspiResult) or result.category != "atspi_control_ready":
        _fail("tier_c_atspi_invalid")


def _validate_trace(summary: Any) -> None:
    integer_fields = (
        "request_count",
        "reply_boundary_count",
        "forbidden_request_count",
        "core_get_image_count",
        "mit_shm_get_image_count",
        "composite_name_window_pixmap_count",
        "composite_redirect_window_count",
        "composite_redirect_subwindows_count",
        "backdrop_property_mutation_count",
    )
    if (
        not isinstance(summary, X11TraceSummary)
        or any(type(getattr(summary, name)) is not int for name in integer_fields)
        or summary.trace_complete is not True
        or summary.request_count <= 0
        or summary.reply_boundary_count <= 0
        or summary.forbidden_request_count != 0
        or summary.core_get_image_count != 0
        or summary.mit_shm_get_image_count != 0
        or summary.composite_name_window_pixmap_count != 0
        or summary.composite_redirect_window_count != 0
        or summary.composite_redirect_subwindows_count != 0
        or summary.backdrop_property_mutation_count != 0
        or summary.shell_side_capture_observed is not False
        or summary.blur_attempt_observed is not False
    ):
        _fail("tier_c_trace_invalid")


class TierCReplayWorkflow:
    """Execute all eight surfaces in exact order, then finalize in cleanup."""

    def __init__(self, port: TierCReplayPort, trace: TraceSupervisorPort) -> None:
        _validate_contracts()
        self._port = port
        self._trace = trace
        self._next_step = 0
        self._failed = False
        self._cleaned = False
        self._assertions: dict[str, int] = {
            assertion: 0 for assertion in TIER_C_ASSERTIONS
        }

    def run_step(self, identifier: str) -> TierCStepResult:
        if self._cleaned or self._next_step >= len(TIER_C_STEPS):
            _fail("tier_c_order_invalid")
        expected = TIER_C_STEPS[self._next_step]
        if identifier != expected:
            _fail("tier_c_order_invalid")
        contract = STEP_CONTRACTS[identifier]
        try:
            self._trace.poll()
            _validate_port_result(
                self._port.prepare_step(contract), "tier_c_step_prepared"
            )
            _validate_keyboard(
                self._port.send_reviewed_keys(identifier, contract.keys),
                len(contract.keys),
            )
            _validate_atspi(self._port.observe_atspi(contract.surface_selector))
            _validate_atspi(self._port.observe_atspi(contract.focus_selector))
            _validate_atspi(self._port.observe_atspi(contract.readable_selector))
            _validate_material(
                self._port.capture_reviewed_material(
                    identifier, contract.sample_points
                )
            )
            _validate_port_result(
                self._port.complete_step(identifier), "tier_c_step_complete"
            )
            if self._next_step + 1 < len(TIER_C_STEPS):
                self._trace.poll()
        except TierCWorkflowError:
            self._failed = True
            raise
        except Exception:
            self._failed = True
            _fail("tier_c_step_failed")

        self._assertions["opaque_materials"] += 1
        self._assertions["complete_keyboard_operation"] += 1
        self._assertions["readable_text_and_visible_focus"] += 1
        self._assertions["no_shell_side_capture"] += 1
        self._assertions["no_blur_attempt"] += 1
        self._next_step += 1
        return TierCStepResult()

    def cleanup(self) -> TierCCleanupResult:
        """Stop clients, naturally finish x11trace, and validate global claims."""

        if self._cleaned:
            _fail("tier_c_cleanup_order_invalid")
        self._cleaned = True
        cleanup: ClientCleanupObservation | None = None
        try:
            cleanup = self._port.cleanup_clients()
        except Exception:
            try:
                self._trace.finish(clients_exited=False)
            except Exception:
                pass
            _fail("tier_c_cleanup_failed")
        if (
            not isinstance(cleanup, ClientCleanupObservation)
            or cleanup.category != "tier_c_clients_cleaned"
            or cleanup.status != "passed"
            or cleanup.clients_exited is not True
        ):
            try:
                self._trace.finish(clients_exited=False)
            except Exception:
                pass
            _fail("tier_c_cleanup_failed")

        complete = self._next_step == len(TIER_C_STEPS) and not self._failed
        try:
            summary = self._trace.finish(clients_exited=cleanup.clients_exited)
        except Exception:
            if complete or self._trace.finished is not True:
                _fail("tier_c_trace_incomplete")
            return TierCCleanupResult()

        if self._trace.finished is not True:
            _fail("tier_c_cleanup_failed")
        if not complete:
            return TierCCleanupResult()
        _validate_trace(summary)
        if any(value != len(TIER_C_STEPS) for value in self._assertions.values()):
            _fail("tier_c_contract_invalid")
        return TierCCleanupResult()


def validate_tier_c_result(result: Any) -> None:
    if isinstance(result, TierCStepResult):
        expected = {"category": "tier_c_step_observed", "status": "passed"}
    elif isinstance(result, TierCCleanupResult):
        expected = {"category": "tier_c_replay_finalized", "status": "passed"}
    else:
        _fail("tier_c_port_result_invalid")
    if result.as_dict() != expected:
        _fail("tier_c_port_result_invalid")
