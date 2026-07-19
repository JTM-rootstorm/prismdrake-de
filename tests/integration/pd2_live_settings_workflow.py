#!/usr/bin/env python3
"""Strict installed settings workflow for PD2 demonstration steps 25-28."""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Protocol

from pd2_live_dbus import METHODS, ProbeResult
from pd2_xwd import (
    MAX_DIMENSION,
    MAX_SURFACES,
    XwdEvidenceError,
    fingerprint_xwd_samples,
    require_complete_surface_transition,
)


QUICK_SETTINGS_NAME = "Quick settings"
QUICK_SETTINGS_ROLE = "pane"
SNAPSHOT_SCHEMA_VERSION = 2
MAX_GENERATION = (1 << 64) - 1

RESULT_CATEGORIES = {
    "open_quick_settings": "quick_settings_accessible",
    "switch_lustre_to_forge": "profile_generation_published",
    "verify_complete_settings_generation": "settings_surfaces_repainted",
    "switch_back_preserving_accessibility": "accessibility_preferences_preserved",
}

CLOSED_ERROR_CATEGORIES = frozenset(
    {
        "settings_accessibility_invalid",
        "settings_accessibility_preferences_changed",
        "settings_accessibility_resolution_mismatch",
        "settings_change_probe_invalid",
        "settings_generation_invalid",
        "settings_generation_transition_invalid",
        "settings_initial_profile_invalid",
        "settings_observer_failed",
        "settings_profile_invalid",
        "settings_quick_settings_not_accessible",
        "settings_quick_settings_probe_invalid",
        "settings_snapshot_probe_invalid",
        "settings_snapshot_shape_invalid",
        "settings_step_not_reviewed",
        "settings_step_order_invalid",
        "settings_surface_capture_invalid",
        "settings_surface_not_settled",
        "settings_surface_samples_invalid",
        "settings_surface_set_changed",
        "settings_surface_set_invalid",
        "settings_surface_transition_incomplete",
    }
)


class SettingsWorkflowError(RuntimeError):
    """A fixed, redacted installed-settings workflow failure."""


class TypedDbusPort(Protocol):
    """Closed typed D-Bus operations supplied by the private live epoch."""

    def call(self, operation: str, *arguments: Any) -> ProbeResult: ...


class AccessibilitySurfacePort(Protocol):
    """Narrow AT-SPI observation for one reviewed shell surface."""

    def observe_surface(self, name: str, role: str) -> bool: ...


class SurfaceObservationPort(Protocol):
    """Bounded settled XWD observations for fixed reviewed surfaces.

    The port may open or focus surfaces sequentially.  Each capture pair must
    come from two consecutive observations after that surface is visible and
    settled; the workflow accepts it only when both reviewed sample hashes
    match. Dynamic pixels outside those coordinates are deliberately ignored.
    """

    def reviewed_surface_sequence(self) -> Sequence[str]: ...

    def capture_settled_xwds(self, surface: str) -> tuple[bytes, bytes]: ...


@dataclass(frozen=True, slots=True)
class SettingsWorkflowConfig:
    """Fixed controlled surfaces and profile-sensitive sample coordinates."""

    reviewed_surfaces: tuple[str, ...]
    sample_points: tuple[tuple[tuple[int, int], ...], ...]

    def __post_init__(self) -> None:
        _validate_surface_set(self.reviewed_surfaces)
        if len(self.sample_points) != len(self.reviewed_surfaces):
            _fail("settings_surface_samples_invalid")
        for points in self.sample_points:
            if (
                not isinstance(points, tuple)
                or not 1 <= len(points) <= 16
                or len(set(points)) != len(points)
            ):
                _fail("settings_surface_samples_invalid")
            for point in points:
                if (
                    not isinstance(point, tuple)
                    or len(point) != 2
                    or type(point[0]) is not int
                    or type(point[1]) is not int
                    or not 0 <= point[0] < MAX_DIMENSION
                    or not 0 <= point[1] < MAX_DIMENSION
                ):
                    _fail("settings_surface_samples_invalid")

    def points_for(self, surface: str) -> tuple[tuple[int, int], ...]:
        try:
            index = self.reviewed_surfaces.index(surface)
        except ValueError:
            _fail("settings_surface_samples_invalid")
        return self.sample_points[index]


@dataclass(frozen=True, slots=True)
class AccessibilityPreferences:
    """Profile-independent accessibility preferences in a validated snapshot."""

    focus_emphasis: str
    animation_scale: float
    minimum_target_size_px: int
    high_contrast: bool
    reduced_motion: bool
    transparency_disabled: bool
    text_scale: float


@dataclass(frozen=True, slots=True)
class EffectiveAccessibility:
    """Resolved accessibility state used to verify return to the first profile."""

    high_contrast: bool
    reduced_motion: bool
    transparency_disabled: bool
    text_scale: float
    animation_scale: float
    focus_width_px: float
    minimum_target_size_px: float


@dataclass(frozen=True, slots=True)
class SnapshotObservation:
    """Only the bounded settings fields needed by steps 25-28."""

    generation: int
    profile: str
    preferences: AccessibilityPreferences
    effective: EffectiveAccessibility


@dataclass(frozen=True, slots=True)
class SettingsWorkflowResult:
    """Categorical serialization that excludes profiles, generations, and pixels."""

    category: str
    status: str = "passed"

    def evidence(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


class _Stage(IntEnum):
    initial = 0
    quick_settings_open = 1
    forge_published = 2
    forge_surfaces_verified = 3
    complete = 4
    failed = 5


def _fail(category: str) -> None:
    raise SettingsWorkflowError(category) from None


def _clean_error(operation: Any, fallback: str) -> Any:
    try:
        return operation(), None
    except SettingsWorkflowError as error:
        category = str(error)
        if category not in CLOSED_ERROR_CATEGORIES:
            category = fallback
    except Exception:
        category = fallback
    return None, category


def _raise_clean(category: str) -> None:
    error = SettingsWorkflowError(category)
    error.__cause__ = None
    error.__context__ = None
    raise error from None


def _validate_surface_set(surfaces: Any) -> tuple[str, ...]:
    if (
        not isinstance(surfaces, tuple)
        or not surfaces
        or len(surfaces) > MAX_SURFACES
        or len(set(surfaces)) != len(surfaces)
    ):
        _fail("settings_surface_set_invalid")
    for surface in surfaces:
        if (
            not isinstance(surface, str)
            or not surface
            or len(surface) > 64
            or not surface.replace("_", "").isalnum()
        ):
            _fail("settings_surface_set_invalid")
    return surfaces


def _exact_bool(value: Any, category: str) -> bool:
    if not isinstance(value, bool):
        _fail(category)
    return value


def _bounded_number(value: Any, minimum: float, maximum: float, category: str) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(float(value))
        or not minimum <= float(value) <= maximum
    ):
        _fail(category)
    return float(value)


def _bounded_integer(value: Any, minimum: int, maximum: int, category: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not minimum <= value <= maximum
    ):
        _fail(category)
    return value


def _mapping(value: Any, category: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        _fail(category)
    return value


def _accessibility(snapshot: Mapping[str, Any]) -> tuple[
    AccessibilityPreferences, EffectiveAccessibility
]:
    settings = _mapping(snapshot.get("settings"), "settings_snapshot_shape_invalid")
    accessibility = _mapping(
        settings.get("accessibility"), "settings_snapshot_shape_invalid"
    )
    appearance = _mapping(settings.get("appearance"), "settings_snapshot_shape_invalid")
    theme = _mapping(snapshot.get("theme"), "settings_snapshot_shape_invalid")
    effective = _mapping(
        theme.get("effective_accessibility"), "settings_snapshot_shape_invalid"
    )

    focus_emphasis = accessibility.get("focus_emphasis")
    if focus_emphasis not in {"standard", "strong"}:
        _fail("settings_accessibility_invalid")
    preferences = AccessibilityPreferences(
        focus_emphasis,
        _bounded_number(
            accessibility.get("animation_scale"),
            0.0,
            2.0,
            "settings_accessibility_invalid",
        ),
        _bounded_integer(
            accessibility.get("minimum_target_size_px"),
            24,
            96,
            "settings_accessibility_invalid",
        ),
        _exact_bool(appearance.get("high_contrast"), "settings_accessibility_invalid"),
        _exact_bool(appearance.get("reduced_motion"), "settings_accessibility_invalid"),
        not _exact_bool(
            appearance.get("transparency_enabled"), "settings_accessibility_invalid"
        ),
        _bounded_number(
            appearance.get("text_scale"),
            0.75,
            3.0,
            "settings_accessibility_invalid",
        ),
    )
    resolved = EffectiveAccessibility(
        _exact_bool(effective.get("high_contrast"), "settings_accessibility_invalid"),
        _exact_bool(effective.get("reduced_motion"), "settings_accessibility_invalid"),
        _exact_bool(
            effective.get("transparency_disabled"), "settings_accessibility_invalid"
        ),
        _bounded_number(
            effective.get("text_scale"),
            0.75,
            3.0,
            "settings_accessibility_invalid",
        ),
        _bounded_number(
            effective.get("animation_scale"),
            0.0,
            2.0,
            "settings_accessibility_invalid",
        ),
        _bounded_number(
            effective.get("focus_width_px"),
            0.0,
            65_535.0,
            "settings_accessibility_invalid",
        ),
        _bounded_number(
            effective.get("minimum_target_size_px"),
            24.0,
            65_535.0,
            "settings_accessibility_invalid",
        ),
    )
    if (
        preferences.high_contrast != resolved.high_contrast
        or preferences.reduced_motion != resolved.reduced_motion
        or preferences.transparency_disabled != resolved.transparency_disabled
        or preferences.text_scale != resolved.text_scale
        or preferences.animation_scale != resolved.animation_scale
        or preferences.minimum_target_size_px != resolved.minimum_target_size_px
    ):
        _fail("settings_accessibility_resolution_mismatch")
    return preferences, resolved


def _snapshot(result: Any) -> SnapshotObservation:
    expected = METHODS["settings_get_current_snapshot"].category
    if (
        not isinstance(result, ProbeResult)
        or result.operation != "settings_get_current_snapshot"
        or result.category != expected
        or result.status != "passed"
        or not isinstance(result.payload, tuple)
        or len(result.payload) != 2
    ):
        _fail("settings_snapshot_probe_invalid")
    generation = _bounded_integer(
        result.payload[0], 1, MAX_GENERATION, "settings_generation_invalid"
    )
    document = _mapping(result.payload[1], "settings_snapshot_shape_invalid")
    if document.get("generation") != generation:
        _fail("settings_generation_invalid")
    profile = document.get("profile_id")
    if profile not in {"lustre", "forge"}:
        _fail("settings_profile_invalid")
    preferences, effective = _accessibility(document)
    return SnapshotObservation(generation, profile, preferences, effective)


def _generation_reply(result: Any) -> int:
    expected = METHODS["settings_request_profile_change"].category
    if (
        not isinstance(result, ProbeResult)
        or result.operation != "settings_request_profile_change"
        or result.category != expected
        or result.status != "passed"
        or not isinstance(result.payload, tuple)
        or len(result.payload) != 1
    ):
        _fail("settings_change_probe_invalid")
    return _bounded_integer(
        result.payload[0], 1, MAX_GENERATION, "settings_generation_invalid"
    )


class InstalledSettingsWorkflow:
    """Ordered, fail-closed workflow for installed demonstration steps 25-28."""

    def __init__(
        self,
        config: SettingsWorkflowConfig,
        dbus: TypedDbusPort,
        accessibility: AccessibilitySurfacePort,
        surfaces: SurfaceObservationPort,
    ) -> None:
        self._config = config
        self._dbus = dbus
        self._accessibility = accessibility
        self._surfaces = surfaces
        self._stage = _Stage.initial
        self._initial: SnapshotObservation | None = None
        self._forge: SnapshotObservation | None = None
        self._before: dict[str, bytes] | None = None

    def run_step(self, identifier: str) -> SettingsWorkflowResult:
        expected_identifiers = tuple(RESULT_CATEGORIES)
        if identifier not in RESULT_CATEGORIES:
            _raise_clean("settings_step_not_reviewed")
        expected_index = int(self._stage)
        if expected_index >= len(expected_identifiers) or identifier != expected_identifiers[
            expected_index
        ]:
            _raise_clean("settings_step_order_invalid")
        result, failure = _clean_error(
            lambda: self._run_reviewed_step(identifier), "settings_observer_failed"
        )
        if failure is not None:
            self._stage = _Stage.failed
            _raise_clean(failure)
        if not isinstance(result, SettingsWorkflowResult):
            _raise_clean("settings_observer_failed")
        self._stage = _Stage(self._stage + 1)
        return result

    def _run_reviewed_step(self, identifier: str) -> SettingsWorkflowResult:
        if identifier == "open_quick_settings":
            self._open_quick_settings()
        elif identifier == "switch_lustre_to_forge":
            self._switch_to_forge()
        elif identifier == "verify_complete_settings_generation":
            self._verify_forge_surfaces()
        elif identifier == "switch_back_preserving_accessibility":
            self._switch_back()
        else:
            _fail("settings_step_not_reviewed")
        return SettingsWorkflowResult(RESULT_CATEGORIES[identifier])

    def _call(self, operation: str, *arguments: Any) -> ProbeResult:
        return self._dbus.call(operation, *arguments)

    def _open_quick_settings(self) -> None:
        result = self._call("shell_open_quick_settings", "")
        expected = METHODS["shell_open_quick_settings"].category
        if (
            not isinstance(result, ProbeResult)
            or result.operation != "shell_open_quick_settings"
            or result.category != expected
            or result.status != "passed"
            or result.payload != ()
        ):
            _fail("settings_quick_settings_probe_invalid")
        if self._accessibility.observe_surface(
            QUICK_SETTINGS_NAME, QUICK_SETTINGS_ROLE
        ) is not True:
            _fail("settings_quick_settings_not_accessible")

    def _capture(self) -> dict[str, bytes]:
        reviewed = self._surfaces.reviewed_surface_sequence()
        if not isinstance(reviewed, Sequence) or isinstance(reviewed, (str, bytes)):
            _fail("settings_surface_set_invalid")
        observed = tuple(reviewed)
        if observed != self._config.reviewed_surfaces:
            _fail("settings_surface_set_changed")
        fingerprints: dict[str, bytes] = {}
        for surface in self._config.reviewed_surfaces:
            try:
                pair = self._surfaces.capture_settled_xwds(surface)
                if not isinstance(pair, tuple) or len(pair) != 2:
                    _fail("settings_surface_capture_invalid")
                points = self._config.points_for(surface)
                first = fingerprint_xwd_samples(pair[0], points)
                second = fingerprint_xwd_samples(pair[1], points)
            except XwdEvidenceError:
                _fail("settings_surface_capture_invalid")
            if first != second:
                _fail("settings_surface_not_settled")
            fingerprints[surface] = second
        return fingerprints

    def _switch_to_forge(self) -> None:
        initial = _snapshot(
            self._call("settings_get_current_snapshot", SNAPSHOT_SCHEMA_VERSION)
        )
        if initial.profile != "lustre" or initial.generation == MAX_GENERATION:
            _fail("settings_initial_profile_invalid")
        before = self._capture()
        requested = _generation_reply(
            self._call("settings_request_profile_change", "forge")
        )
        if requested != initial.generation + 1:
            _fail("settings_generation_transition_invalid")
        forge = _snapshot(
            self._call("settings_get_current_snapshot", SNAPSHOT_SCHEMA_VERSION)
        )
        if forge.profile != "forge" or forge.generation != requested:
            _fail("settings_generation_transition_invalid")
        if forge.preferences != initial.preferences:
            _fail("settings_accessibility_preferences_changed")
        self._initial = initial
        self._forge = forge
        self._before = before

    def _verify_forge_surfaces(self) -> None:
        if self._before is None or self._forge is None:
            _fail("settings_step_order_invalid")
        after = self._capture()
        try:
            summary = require_complete_surface_transition(
                self._before, after, self._config.reviewed_surfaces
            )
        except XwdEvidenceError:
            _fail("settings_surface_transition_incomplete")
        if (
            summary.surface_count != len(self._config.reviewed_surfaces)
            or summary.changed_surface_count != summary.surface_count
            or summary.complete_transition_observed is not True
        ):
            _fail("settings_surface_transition_incomplete")

    def _switch_back(self) -> None:
        if self._initial is None or self._forge is None:
            _fail("settings_step_order_invalid")
        if self._forge.generation == MAX_GENERATION:
            _fail("settings_generation_transition_invalid")
        requested = _generation_reply(
            self._call("settings_request_profile_change", "lustre")
        )
        if requested != self._forge.generation + 1:
            _fail("settings_generation_transition_invalid")
        final = _snapshot(
            self._call("settings_get_current_snapshot", SNAPSHOT_SCHEMA_VERSION)
        )
        if final.profile != "lustre" or final.generation != requested:
            _fail("settings_generation_transition_invalid")
        if (
            self._initial.preferences != self._forge.preferences
            or self._initial.preferences != final.preferences
            or self._initial.effective != final.effective
        ):
            _fail("settings_accessibility_preferences_changed")
