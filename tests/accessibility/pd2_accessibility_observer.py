#!/usr/bin/env python3
"""Map source-bound installed GI AT-SPI observations into the PD2 matrix."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, NoReturn


ROOT = Path(__file__).resolve().parents[2]
INTEGRATION = ROOT / "tests" / "integration"
REVISION = re.compile(r"[0-9a-f]{40}")
ENVIRONMENT_ID = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
METHOD_ID = "pd2-live-v1"
SURFACES = (
    "panel",
    "pinned_task",
    "running_task",
    "grouped_task",
    "group_popup",
    "launcher",
    "launcher_results",
    "desktop_surface_exclusion",
    "transient_notification",
    "notification_center",
    "quick_settings",
    "logout_confirmation",
    "output_migration",
    "service_restart_focus_recovery",
)
MATRIX_CHECKS = (
    "tab_backtab",
    "arrow_navigation",
    "enter_activation",
    "space_activation",
    "escape_recovery",
    "menu_key",
    "shift_f10",
    "accessible_name",
    "accessible_role",
    "accessible_description",
    "state_focused",
    "state_selected",
    "state_active",
    "state_urgent",
    "state_minimized",
    "state_disabled",
    "state_unavailable",
    "state_checked",
    "state_expanded",
    "minimum_target_size",
    "high_contrast",
    "reduced_motion",
    "disabled_transparency",
    "text_scale",
    "rtl_smoke",
    "long_string_layout",
)

EvidenceRequirement = tuple[tuple[str, str], ...]


def requirement(*items: tuple[str, str]) -> EvidenceRequirement:
    return items


# Every pass below is backed by an exact installed workflow or Tier C result.
# Missing rows remain visible reviewed skips; broad Tier C claims are not
# expanded into states or settings variants that the live workflow did not
# actually observe.
OBSERVED: dict[tuple[str, str], EvidenceRequirement] = {
    ("panel", "tab_backtab"): requirement(
        ("installed_workflow", "verify_accessible_descriptions_states_and_backtab")
    ),
    ("panel", "accessible_name"): requirement(("tier_c_replay", "panel")),
    ("panel", "accessible_role"): requirement(("tier_c_replay", "panel")),
    ("panel", "accessible_description"): requirement(
        ("installed_workflow", "verify_accessible_descriptions_states_and_backtab")
    ),
    ("panel", "state_focused"): requirement(("tier_c_replay", "panel")),
    ("running_task", "escape_recovery"): requirement(
        ("tier_c_replay", "task_actions")
    ),
    ("running_task", "menu_key"): requirement(("tier_c_replay", "task_actions")),
    ("running_task", "shift_f10"): requirement(
        ("installed_workflow", "close_both_windows_through_panel")
    ),
    ("running_task", "accessible_name"): requirement(
        ("installed_workflow", "close_both_windows_through_panel")
    ),
    ("running_task", "accessible_role"): requirement(
        ("installed_workflow", "close_both_windows_through_panel")
    ),
    ("running_task", "state_focused"): requirement(
        ("installed_workflow", "close_both_windows_through_panel")
    ),
    ("grouped_task", "accessible_name"): requirement(
        ("installed_workflow", "verify_task_grouping")
    ),
    ("grouped_task", "accessible_role"): requirement(
        ("installed_workflow", "verify_task_grouping")
    ),
    ("launcher", "accessible_name"): requirement(("tier_c_replay", "launcher")),
    ("launcher", "accessible_role"): requirement(("tier_c_replay", "launcher")),
    ("launcher", "state_focused"): requirement(("tier_c_replay", "launcher")),
    ("launcher_results", "arrow_navigation"): requirement(
        ("installed_workflow", "pin_controlled_application")
    ),
    ("launcher_results", "escape_recovery"): requirement(
        ("installed_workflow", "pin_controlled_application")
    ),
    ("launcher_results", "enter_activation"): requirement(
        ("tier_c_replay", "launcher")
    ),
    ("launcher_results", "shift_f10"): requirement(
        ("installed_workflow", "pin_controlled_application")
    ),
    ("launcher_results", "accessible_name"): requirement(
        ("installed_workflow", "search_controlled_application")
    ),
    ("launcher_results", "accessible_role"): requirement(
        ("installed_workflow", "search_controlled_application")
    ),
    ("transient_notification", "accessible_name"): requirement(
        ("tier_c_replay", "notification")
    ),
    ("transient_notification", "accessible_role"): requirement(
        ("tier_c_replay", "notification")
    ),
    ("notification_center", "enter_activation"): requirement(
        ("tier_c_replay", "notification_center")
    ),
    ("notification_center", "accessible_name"): requirement(
        ("tier_c_replay", "notification_center")
    ),
    ("notification_center", "accessible_role"): requirement(
        ("tier_c_replay", "notification_center")
    ),
    ("notification_center", "state_focused"): requirement(
        ("tier_c_replay", "notification_center")
    ),
    ("notification_center", "state_checked"): requirement(
        ("installed_workflow", "toggle_do_not_disturb")
    ),
    ("quick_settings", "space_activation"): requirement(
        ("tier_c_replay", "quick_settings")
    ),
    ("quick_settings", "tab_backtab"): requirement(
        ("installed_workflow", "verify_accessible_descriptions_states_and_backtab")
    ),
    ("quick_settings", "accessible_name"): requirement(
        ("tier_c_replay", "quick_settings")
    ),
    ("quick_settings", "accessible_role"): requirement(
        ("tier_c_replay", "quick_settings")
    ),
    ("quick_settings", "accessible_description"): requirement(
        ("installed_workflow", "verify_accessible_descriptions_states_and_backtab")
    ),
    ("quick_settings", "state_focused"): requirement(
        ("tier_c_replay", "quick_settings")
    ),
    ("quick_settings", "state_checked"): requirement(
        ("tier_c_replay", "quick_settings")
    ),
    ("logout_confirmation", "enter_activation"): requirement(
        ("tier_c_replay", "logout")
    ),
    ("logout_confirmation", "accessible_name"): requirement(
        ("tier_c_replay", "logout")
    ),
    ("logout_confirmation", "accessible_role"): requirement(
        ("tier_c_replay", "logout")
    ),
    ("logout_confirmation", "state_focused"): requirement(
        ("tier_c_replay", "logout")
    ),
    ("output_migration", "tab_backtab"): requirement(
        ("installed_workflow", "verify_output_migration_accessibility")
    ),
    ("output_migration", "accessible_name"): requirement(
        ("installed_workflow", "verify_output_migration_accessibility")
    ),
    ("output_migration", "accessible_role"): requirement(
        ("installed_workflow", "verify_output_migration_accessibility")
    ),
    ("output_migration", "state_focused"): requirement(
        ("installed_workflow", "verify_output_migration_accessibility")
    ),
    ("service_restart_focus_recovery", "tab_backtab"): requirement(
        ("installed_workflow", "verify_settingsd_focus_recovery"),
        ("installed_workflow", "verify_notifyd_focus_recovery"),
        ("installed_workflow", "verify_shell_focus_recovery"),
    ),
    ("service_restart_focus_recovery", "accessible_name"): requirement(
        ("installed_workflow", "verify_settingsd_focus_recovery"),
        ("installed_workflow", "verify_notifyd_focus_recovery"),
        ("installed_workflow", "verify_shell_focus_recovery"),
    ),
    ("service_restart_focus_recovery", "accessible_role"): requirement(
        ("installed_workflow", "verify_settingsd_focus_recovery"),
        ("installed_workflow", "verify_notifyd_focus_recovery"),
        ("installed_workflow", "verify_shell_focus_recovery"),
    ),
    ("service_restart_focus_recovery", "state_focused"): requirement(
        ("installed_workflow", "verify_settingsd_focus_recovery"),
        ("installed_workflow", "verify_notifyd_focus_recovery"),
        ("installed_workflow", "verify_shell_focus_recovery"),
    ),
}

NOT_APPLICABLE = {
    ("panel", "state_selected"),
    ("panel", "state_urgent"),
    ("panel", "state_minimized"),
    ("panel", "state_checked"),
    ("panel", "state_expanded"),
    ("desktop_surface_exclusion", "accessible_name"),
    ("desktop_surface_exclusion", "accessible_role"),
    ("desktop_surface_exclusion", "accessible_description"),
    ("desktop_surface_exclusion", "state_selected"),
    ("desktop_surface_exclusion", "state_active"),
    ("desktop_surface_exclusion", "state_urgent"),
    ("desktop_surface_exclusion", "state_minimized"),
    ("desktop_surface_exclusion", "state_disabled"),
    ("desktop_surface_exclusion", "state_unavailable"),
    ("desktop_surface_exclusion", "state_checked"),
    ("desktop_surface_exclusion", "state_expanded"),
    ("transient_notification", "state_selected"),
    ("transient_notification", "state_minimized"),
    ("transient_notification", "state_checked"),
    ("transient_notification", "state_expanded"),
    ("logout_confirmation", "state_selected"),
    ("logout_confirmation", "state_urgent"),
    ("logout_confirmation", "state_minimized"),
    ("logout_confirmation", "state_checked"),
}


class AccessibilityObserverError(RuntimeError):
    """One closed accessibility-observer failure."""


class ClosedArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        del message
        raise AccessibilityObserverError("arguments_invalid")


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise AccessibilityObserverError(identifier)


def binding() -> dict[str, str]:
    revision = os.environ.get("PRISMDRAKE_PD2_SOURCE_REVISION", "")
    environment_id = os.environ.get("PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID", "")
    method_id = os.environ.get("PRISMDRAKE_PD2_METHOD_ID", "")
    require(REVISION.fullmatch(revision) is not None, "binding_environment_invalid")
    require(
        ENVIRONMENT_ID.fullmatch(environment_id) is not None,
        "binding_environment_invalid",
    )
    require(method_id == METHOD_ID, "binding_environment_invalid")
    return {
        "source_revision": revision,
        "reference_environment_id": environment_id,
        "method_id": method_id,
    }


def require_gi_atspi() -> Any:
    try:
        import gi  # pylint: disable=import-outside-toplevel

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi  # pylint: disable=import-outside-toplevel
    except (ImportError, ValueError) as error:
        raise AccessibilityObserverError("gi_atspi_dependency_unavailable") from error
    require(callable(getattr(Atspi, "get_desktop", None)), "gi_atspi_dependency_invalid")
    return Atspi


def installed_evidence() -> Mapping[str, Any]:
    configured = os.environ.get("PRISMDRAKE_PD2_INSTALLED_DEMO_EVIDENCE", "")
    path = Path(configured)
    require(
        bool(configured)
        and path.is_absolute()
        and path.is_file()
        and not path.is_symlink(),
        "installed_demo_evidence_unavailable",
    )
    integration = os.fspath(INTEGRATION)
    if integration not in sys.path:
        sys.path.insert(0, integration)
    import pd2_demo  # pylint: disable=import-outside-toplevel

    try:
        document = pd2_demo.load_evidence(path)
        pd2_demo.validate_evidence(document)
    except pd2_demo.HarnessError as error:
        raise AccessibilityObserverError("installed_demo_evidence_invalid") from error
    require(
        document.get("source_revision") == binding()["source_revision"],
        "installed_demo_revision_mismatch",
    )
    return document


def _result_ids(document: Mapping[str, Any]) -> dict[str, frozenset[str]]:
    result: dict[str, frozenset[str]] = {}
    for phase, field in (
        ("installed_workflow", "workflow"),
        ("failure_replay", "failure_replay"),
        ("tier_c_replay", "tier_c_replay"),
    ):
        records = document[field]
        require(isinstance(records, Sequence), "installed_demo_evidence_invalid")
        result[phase] = frozenset(record["id"] for record in records)
    return result


def reviewed_skip(code: str, reason: str) -> dict[str, Any]:
    return {
        "status": "skipped",
        "reviewed": True,
        "reason_code": code,
        "reason": reason,
    }


def passed() -> dict[str, str]:
    return {
        "status": "passed",
        "method": "Validated source bound installed GI AT SPI lifecycle evidence",
    }


def accessibility_result(surface: str, check: str) -> dict[str, Any]:
    require(surface in SURFACES and check in MATRIX_CHECKS, "accessibility_probe_invalid")
    require_gi_atspi()
    document = installed_evidence()
    key = (surface, check)
    if surface == "service_restart_focus_recovery" and key not in OBSERVED:
        return reviewed_skip(
            "live_adapter_unimplemented",
            "Installed lifecycle evidence does not observe post restart keyboard focus",
        )
    if key in NOT_APPLICABLE:
        return reviewed_skip(
            "not_applicable",
            "The named semantic object does not expose this state or property",
        )
    required = OBSERVED.get(key)
    if required is None:
        return reviewed_skip(
            "live_adapter_unimplemented",
            "Installed lifecycle evidence does not observe this exact matrix check",
        )
    observed = _result_ids(document)
    require(
        all(identifier in observed[phase] for phase, identifier in required),
        "installed_demo_observation_missing",
    )
    return passed()


def global_result(global_check: str) -> dict[str, Any]:
    require(global_check in {"live_atspi", "screen_reader_workflow"},
            "accessibility_probe_invalid")
    if global_check == "screen_reader_workflow":
        tools = (Path("/usr/bin/orca"), Path("/usr/bin/spd-say"), Path("/usr/bin/espeak-ng"))
        require(not any(path.is_file() for path in tools),
                "screen_reader_dependency_state_unexpected")
        return reviewed_skip(
            "dependency_unavailable",
            "The reference environment does not provide reviewed screen reader tooling",
        )
    require_gi_atspi()
    document = installed_evidence()
    observed = _result_ids(document)
    require(
        all(identifier in observed["tier_c_replay"] for identifier in (
            "desktop",
            "panel",
            "launcher",
            "task_actions",
            "notification",
            "notification_center",
            "quick_settings",
            "logout",
        )),
        "installed_demo_observation_missing",
    )
    return passed()


def parse_arguments() -> argparse.Namespace:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument("--role", choices=("accessibility",), default="accessibility")
    parser.add_argument("--surface")
    parser.add_argument("--check")
    parser.add_argument("--global", dest="global_check")
    options = parser.parse_args()
    require(
        (options.global_check is not None)
        != (options.surface is not None and options.check is not None),
        "arguments_invalid",
    )
    return options


def main() -> int:
    try:
        options = parse_arguments()
        result = (
            global_result(options.global_check)
            if options.global_check is not None
            else accessibility_result(options.surface, options.check)
        )
        print(
            json.dumps(
                {"binding": binding(), "result": result},
                ensure_ascii=True,
                separators=(",", ":"),
                sort_keys=True,
            )
        )
        return 0
    except AccessibilityObserverError as error:
        print(f"pd2_accessibility_observer: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
