#!/usr/bin/env python3
"""Map source-bound installed GI AT-SPI observations into the PD2 matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
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
    ("transient_notification", "enter_activation"): requirement(
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

CTEST_CHECKS: dict[str, tuple[str, frozenset[str]]] = {
    "panel": (
        "PanelSurfaceQmlTest",
        frozenset({
            "arrow_navigation", "enter_activation", "space_activation", "minimum_target_size",
            "high_contrast", "reduced_motion", "disabled_transparency", "text_scale",
            "rtl_smoke", "long_string_layout",
        }),
    ),
    "pinned_task": (
        "PanelSurfaceQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "enter_activation",
            "space_activation", "escape_recovery", "menu_key", "shift_f10",
            "accessible_name", "accessible_role", "accessible_description",
            "state_focused", "state_active", "state_minimized", "state_disabled",
            "state_urgent", "state_checked", "minimum_target_size", "high_contrast",
            "reduced_motion", "disabled_transparency", "text_scale", "rtl_smoke",
            "long_string_layout",
        }),
    ),
    "running_task": (
        "PanelSurfaceQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "enter_activation",
            "space_activation", "accessible_description", "state_checked",
            "state_active", "state_minimized", "state_disabled", "state_urgent",
            "minimum_target_size", "high_contrast", "reduced_motion",
            "disabled_transparency", "text_scale", "rtl_smoke", "long_string_layout",
        }),
    ),
    "grouped_task": (
        "PanelSurfaceQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "enter_activation", "space_activation",
            "escape_recovery", "menu_key", "shift_f10", "accessible_description",
            "state_focused", "state_active", "state_minimized", "state_disabled",
            "state_urgent", "state_checked", "minimum_target_size", "high_contrast",
            "reduced_motion", "disabled_transparency", "text_scale", "rtl_smoke",
            "long_string_layout",
        }),
    ),
    "group_popup": (
        "PanelSurfaceQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "enter_activation", "space_activation",
            "escape_recovery", "menu_key", "shift_f10",
            "accessible_name", "accessible_role", "accessible_description",
            "state_focused", "state_active", "state_minimized", "state_urgent",
            "minimum_target_size", "high_contrast", "reduced_motion",
            "disabled_transparency", "text_scale", "rtl_smoke", "long_string_layout",
            "state_disabled", "state_selected",
        }),
    ),
    "launcher": (
        "LauncherSurfaceQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "enter_activation",
            "space_activation", "escape_recovery", "accessible_description",
            "minimum_target_size", "state_unavailable", "high_contrast",
            "reduced_motion", "disabled_transparency", "text_scale", "rtl_smoke",
            "long_string_layout",
        }),
    ),
    "launcher_results": (
        "LauncherSurfaceQmlTest",
        frozenset({
            "tab_backtab", "space_activation", "menu_key", "accessible_description",
            "state_selected", "state_checked", "state_focused", "state_disabled",
            "minimum_target_size", "high_contrast", "reduced_motion",
            "disabled_transparency", "text_scale",
            "rtl_smoke", "long_string_layout",
        }),
    ),
    "transient_notification": (
        "NotificationCardQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "space_activation",
            "escape_recovery", "accessible_description", "state_focused",
            "state_urgent", "state_disabled", "minimum_target_size", "high_contrast",
            "reduced_motion", "disabled_transparency", "rtl_smoke",
            "text_scale", "long_string_layout",
        }),
    ),
    "notification_center": (
        "NotificationCenterQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "space_activation", "escape_recovery",
            "accessible_description", "state_disabled", "minimum_target_size",
            "high_contrast",
            "reduced_motion", "disabled_transparency", "state_unavailable",
            "text_scale", "rtl_smoke", "long_string_layout",
        }),
    ),
    "quick_settings": (
        "QuickSettingsSurfaceQmlTest",
        frozenset({
            "arrow_navigation", "enter_activation", "escape_recovery", "minimum_target_size",
            "state_disabled", "state_unavailable", "high_contrast", "reduced_motion",
            "disabled_transparency",
            "text_scale", "rtl_smoke", "long_string_layout",
        }),
    ),
    "logout_confirmation": (
        "QuickSettingsSurfaceQmlTest",
        frozenset({
            "tab_backtab", "arrow_navigation", "space_activation", "escape_recovery",
            "accessible_description", "state_disabled", "minimum_target_size", "text_scale",
            "high_contrast", "reduced_motion", "disabled_transparency", "rtl_smoke",
            "long_string_layout",
        }),
    ),
}

CTEST_OBSERVED = {
    (surface, check): test_name
    for surface, (test_name, checks) in CTEST_CHECKS.items()
    for check in checks
    if (surface, check) not in OBSERVED
}

# Cells added after the initial matrix audit name the exact QML case that
# exercises the behavior.  The package evidence binds these passing suites to
# the installed source revision; retaining the case identifiers here prevents
# a broad suite pass from obscuring what was actually observed.
CTEST_CASE_EVIDENCE: dict[tuple[str, str], tuple[str, ...]] = {
    ("panel", "arrow_navigation"): (
        "PanelSurfaceRealAdapters::test_taskArrowTraversalMirrorsForRightToLeft",
    ),
    ("pinned_task", "state_active"): (
        "PanelSurfaceRealAdapters::test_pinnedOnlyTaskKeyboardAccessibilityAndPinStateAreExact",
    ),
    ("pinned_task", "state_disabled"): (
        "PanelSurfaceRealAdapters::test_pinnedOnlyTaskKeyboardAccessibilityAndPinStateAreExact",
    ),
    ("pinned_task", "state_minimized"): (
        "PanelSurfaceRealAdapters::test_pinnedOnlyTaskKeyboardAccessibilityAndPinStateAreExact",
    ),
    ("pinned_task", "state_urgent"): (
        "PanelSurfaceRealAdapters::test_pinnedOnlyTaskKeyboardAccessibilityAndPinStateAreExact",
    ),
    ("running_task", "state_active"): (
        "PanelSurfaceRealAdapters::test_targetsAccessibilityAndNonColorTaskStates",
    ),
    ("running_task", "state_disabled"): (
        "PanelSurfaceRealAdapters::test_minimizedTargetDisablesMinimizeAndStartsOnClose",
    ),
    ("running_task", "state_minimized"): (
        "PanelSurfaceRealAdapters::test_minimizedTargetDisablesMinimizeAndStartsOnClose",
    ),
    ("running_task", "state_urgent"): (
        "PanelSurfaceRealAdapters::test_targetsAccessibilityAndNonColorTaskStates",
    ),
    ("grouped_task", "state_focused"): (
        "PanelSurfaceRealAdapters::test_groupPopupKeyboardActionsAndAccessibilityAreDeterministic",
    ),
    ("grouped_task", "state_urgent"): (
        "PanelSurfaceRealAdapters::test_groupedTasksExposeProductionActionsAndOutputScopedAffordances",
    ),
    ("group_popup", "state_focused"): (
        "PanelSurfaceRealAdapters::test_groupPopupKeyboardActionsAndAccessibilityAreDeterministic",
    ),
    ("launcher", "state_unavailable"): (
        "LauncherSurfaceRealAdapters::test_explicitLoadingEmptyNoResultsErrorAndInvalidRetention",
    ),
    ("launcher_results", "state_focused"): (
        "LauncherSurfaceRealAdapters::test_boundedSearchKeyboardActivationAndFocusExit",
    ),
    ("transient_notification", "state_disabled"): (
        "NotificationCard::test_keyboardOrderSkipsDisabledActionAndActivatesExactAffordance",
    ),
    ("transient_notification", "state_urgent"): (
        "NotificationCard::test_literalTextAndAccessibleNotificationMetadata",
    ),
    ("transient_notification", "text_scale"): (
        "NotificationCard::test_pseudoLocalizedLongTextAndScaledTargetsRemainUsable",
    ),
    ("notification_center", "state_unavailable"): (
        "NotificationCenterSurface::test_unavailableAndLiteralAccessibleContent",
    ),
    ("notification_center", "text_scale"): (
        "NotificationCenterSurface::test_rtlFallbackContrastAndScaledTargetsUsePublishedThemeInputs",
    ),
    ("quick_settings", "state_disabled"): (
        "QuickSettingsSurface::test_pendingAndUnavailableAdaptersRenderDisabled",
    ),
    ("quick_settings", "state_unavailable"): (
        "QuickSettingsSurface::test_oneAdapterFailureDoesNotDisableOthers",
    ),
}
CTEST_CASE_EVIDENCE.update({
    ("pinned_task", check): (
        "PanelSurfaceRealAdapters::test_pinnedSurfaceConsumesAccessibleGenerationTokens",
    )
    for check in (
        "disabled_transparency", "high_contrast", "reduced_motion", "rtl_smoke",
        "text_scale",
    )
})
CTEST_CASE_EVIDENCE[("running_task", "text_scale")] = (
    "PanelSurfaceRealAdapters::test_accessibilityAndOpaqueFallbackComeFromOneRealGeneration",
)
CTEST_CASE_EVIDENCE[("panel", "text_scale")] = (
    "PanelSurfaceRealAdapters::test_accessibilityAndOpaqueFallbackComeFromOneRealGeneration",
)
CTEST_CASE_EVIDENCE.update({
    ("grouped_task", check): (
        "PanelSurfaceRealAdapters::test_groupedTaskAndPopupExposeCompleteKeyboardStateTransitions",
    )
    for check in (
        "arrow_navigation", "menu_key", "shift_f10", "space_activation", "state_active",
        "state_checked", "state_disabled", "state_minimized", "tab_backtab",
    )
})
CTEST_CASE_EVIDENCE.update({
    ("grouped_task", check): (
        "PanelSurfaceRealAdapters::test_groupedSurfacesConsumeAccessibleGenerationTokens",
    )
    for check in (
        "disabled_transparency", "high_contrast", "reduced_motion", "rtl_smoke",
        "text_scale",
    )
})
CTEST_CASE_EVIDENCE.update({
    ("group_popup", check): (
        "PanelSurfaceRealAdapters::test_groupedTaskAndPopupExposeCompleteKeyboardStateTransitions",
    )
    for check in (
        "shift_f10", "space_activation", "state_active", "state_minimized",
        "state_urgent", "tab_backtab",
    )
})
CTEST_CASE_EVIDENCE.update({
    ("group_popup", check): (
        "PanelSurfaceRealAdapters::test_groupedSurfacesConsumeAccessibleGenerationTokens",
    )
    for check in (
        "disabled_transparency", "high_contrast", "reduced_motion", "text_scale",
    )
})
CTEST_CASE_EVIDENCE.update({
    ("launcher", check): (
        "LauncherSurfaceRealAdapters::test_accessibleFallbackTargetsAndPlainUntrustedText",
    )
    for check in ("high_contrast", "long_string_layout", "text_scale")
})
CTEST_CASE_EVIDENCE[("launcher", "space_activation")] = (
    "LauncherSurfaceRealAdapters::test_explicitRefreshAndOpaqueIconPresentation",
)
CTEST_CASE_EVIDENCE[("launcher_results", "menu_key")] = (
    "LauncherSurfaceRealAdapters::test_applicationActionsAreKeyboardReachable",
)
CTEST_CASE_EVIDENCE[("launcher_results", "space_activation")] = (
    "LauncherSurfaceRealAdapters::test_boundedSearchKeyboardActivationAndFocusExit",
)
CTEST_CASE_EVIDENCE.update({
    ("launcher_results", check): (
        "LauncherSurfaceRealAdapters::test_accessibleFallbackTargetsAndPlainUntrustedText",
    )
    for check in ("high_contrast", "text_scale")
})
CTEST_CASE_EVIDENCE[("notification_center", "space_activation")] = (
    "NotificationCenterSurface::test_keyboardTraversalAndFocusReturn",
)
CTEST_CASE_EVIDENCE[("notification_center", "state_disabled")] = (
    "NotificationCenterSurface::test_unavailableAndLiteralAccessibleContent",
)
CTEST_CASE_EVIDENCE.update({
    ("logout_confirmation", check): (
        "LogoutConfirmationSurface::test_accessibleModalFocusAndActions",
    )
    for check in ("disabled_transparency",)
})
CTEST_CASE_EVIDENCE.update({
    ("panel", check): (
        "PanelSurfaceRealAdapters::test_accessibleLauncherPressEmitsExactlyOneRequest",
    )
    for check in ("enter_activation", "space_activation")
})
CTEST_CASE_EVIDENCE[("panel", "long_string_layout")] = (
    "PanelSurfaceRealAdapters::test_longRunningTaskRemainsBoundedAndScrollable",
)
CTEST_CASE_EVIDENCE[("pinned_task", "long_string_layout")] = (
    "PanelSurfaceRealAdapters::test_longPinnedTaskRemainsBoundedAndScrollable",
)
CTEST_CASE_EVIDENCE[("running_task", "long_string_layout")] = (
    "PanelSurfaceRealAdapters::test_longRunningTaskRemainsBoundedAndScrollable",
)
CTEST_CASE_EVIDENCE[("grouped_task", "long_string_layout")] = (
    "PanelSurfaceRealAdapters::test_longGroupedTaskAndMembersRemainBoundedAndScrollable",
)
CTEST_CASE_EVIDENCE[("group_popup", "long_string_layout")] = (
    "PanelSurfaceRealAdapters::test_longGroupedTaskAndMembersRemainBoundedAndScrollable",
)
CTEST_CASE_EVIDENCE.update({
    ("group_popup", check): (
        "PanelSurfaceRealAdapters::test_groupedTaskAndPopupExposeCompleteKeyboardStateTransitions",
    )
    for check in ("state_disabled", "state_selected")
})
CTEST_CASE_EVIDENCE[("launcher_results", "state_disabled")] = (
    "LauncherSurfaceRealAdapters::test_boundedSearchKeyboardActivationAndFocusExit",
)
CTEST_CASE_EVIDENCE[("notification_center", "long_string_layout")] = (
    "NotificationCenterSurface::test_longNotificationContentWrapsInsideTheCenter",
)
CTEST_CASE_EVIDENCE[("quick_settings", "enter_activation")] = (
    "QuickSettingsSurface::test_keyboardTraversalAndEscapeRecovery",
)
CTEST_CASE_EVIDENCE.update({
    (surface, check): (
        "QuickSettingsResolvedTheme::"
        "test_accessibleGenerationCausallyControlsBothRenderedSurfaces",
    )
    for surface in ("quick_settings", "logout_confirmation")
    for check in ("high_contrast", "reduced_motion")
})
if not set(CTEST_CASE_EVIDENCE) <= set(CTEST_OBSERVED):
    raise RuntimeError("accessibility_ctest_case_map_invalid")

NOT_APPLICABLE_CHECKS = {
    "panel": frozenset({
        "escape_recovery", "menu_key", "shift_f10", "state_selected",
        "state_active", "state_urgent", "state_minimized", "state_disabled",
        "state_unavailable", "state_checked", "state_expanded",
    }),
    "pinned_task": frozenset({
        "state_selected", "state_unavailable",
    }),
    "running_task": frozenset({"state_selected", "state_unavailable"}),
    "grouped_task": frozenset({"state_selected", "state_unavailable"}),
    "group_popup": frozenset({
        "state_checked", "state_unavailable", "state_expanded",
    }),
    "launcher": frozenset({
        "menu_key", "shift_f10", "state_selected", "state_active",
        "state_urgent", "state_minimized", "state_disabled", "state_checked",
        "state_expanded",
    }),
    "launcher_results": frozenset({
        "state_active", "state_urgent", "state_minimized", "state_unavailable",
    }),
    "desktop_surface_exclusion": frozenset(MATRIX_CHECKS),
    "transient_notification": frozenset({
        "menu_key", "shift_f10", "state_selected", "state_active",
        "state_minimized", "state_unavailable", "state_checked", "state_expanded",
    }),
    "notification_center": frozenset({
        "menu_key", "shift_f10", "state_selected", "state_active",
        "state_urgent", "state_minimized", "state_expanded",
    }),
    "quick_settings": frozenset({
        "menu_key", "shift_f10", "state_selected", "state_active",
        "state_urgent", "state_minimized", "state_expanded",
    }),
    "logout_confirmation": frozenset({
        "menu_key", "shift_f10", "state_selected", "state_active",
        "state_urgent", "state_minimized", "state_unavailable", "state_checked",
        "state_expanded",
    }),
    "output_migration": frozenset({
        "accessible_description", "arrow_navigation", "disabled_transparency",
        "enter_activation", "escape_recovery", "high_contrast",
        "long_string_layout", "menu_key", "minimum_target_size",
        "reduced_motion", "rtl_smoke", "shift_f10", "space_activation",
        "state_active", "state_checked", "state_disabled", "state_expanded",
        "state_minimized", "state_selected", "state_unavailable", "state_urgent",
        "text_scale",
    }),
    "service_restart_focus_recovery": frozenset({
        "accessible_description", "arrow_navigation", "disabled_transparency",
        "enter_activation", "escape_recovery", "high_contrast",
        "long_string_layout", "menu_key", "minimum_target_size",
        "reduced_motion", "rtl_smoke", "shift_f10", "space_activation",
        "state_active", "state_checked", "state_disabled", "state_expanded",
        "state_minimized", "state_selected", "state_unavailable", "state_urgent",
        "text_scale",
    }),
}

NOT_APPLICABLE = {
    (surface, check): (
        f"The {surface.replace('_', ' ')} evidence object does not define "
        f"{check.replace('_', ' ')} semantics"
    )
    for surface, checks in NOT_APPLICABLE_CHECKS.items()
    for check in checks
}

_MATRIX_KEYS = {
    (surface, check) for surface in SURFACES for check in MATRIX_CHECKS
}
UNIMPLEMENTED_CHECKS = {
    "panel": frozenset(),
    "pinned_task": frozenset({
        "state_expanded",
    }),
    "running_task": frozenset({
        "state_expanded",
    }),
    "grouped_task": frozenset({
        "state_expanded",
    }),
    "group_popup": frozenset(),
    "launcher": frozenset(),
    "launcher_results": frozenset({
        "state_expanded",
    }),
    "desktop_surface_exclusion": frozenset(),
    "transient_notification": frozenset(),
    "notification_center": frozenset(),
    "quick_settings": frozenset(),
    "logout_confirmation": frozenset(),
    "output_migration": frozenset(),
    "service_restart_focus_recovery": frozenset(),
}
UNIMPLEMENTED = {
    (surface, check)
    for surface, checks in UNIMPLEMENTED_CHECKS.items()
    for check in checks
}
UNIMPLEMENTED_REASONS = {
    key: (
        "Qt 6.11 QQuickAccessibleAttached cannot publish the expanded accessibility "
        "state for this real menu or popup; closing this cell requires a reviewed "
        "C++ accessibility backend path"
    )
    for key in UNIMPLEMENTED
    if key[1] == "state_expanded"
}
if set(UNIMPLEMENTED_REASONS) != UNIMPLEMENTED:
    raise RuntimeError("accessibility_unimplemented_reason_map_invalid")
if (
    set(OBSERVED) & set(CTEST_OBSERVED)
    or set(OBSERVED) & set(NOT_APPLICABLE)
    or set(CTEST_OBSERVED) & set(NOT_APPLICABLE)
    or set(OBSERVED) & set(UNIMPLEMENTED)
    or set(CTEST_OBSERVED) & set(UNIMPLEMENTED)
    or set(NOT_APPLICABLE) & set(UNIMPLEMENTED)
    or set(OBSERVED) | set(CTEST_OBSERVED) | set(NOT_APPLICABLE)
       | set(UNIMPLEMENTED) != _MATRIX_KEYS
):
    raise RuntimeError("accessibility_coverage_map_invalid")


class AccessibilityObserverError(RuntimeError):
    """One closed accessibility-observer failure."""


class ClosedArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        del message
        raise AccessibilityObserverError("arguments_invalid")


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise AccessibilityObserverError(identifier)


def duplicate_safe_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def reject_constant(value: str) -> NoReturn:
    del value
    raise ValueError("non-finite JSON number")


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


def passed(
    method: str = "Validated source bound installed GI AT SPI lifecycle evidence",
) -> dict[str, str]:
    return {
        "status": "passed",
        "method": method,
    }


def read_regular_file(path: Path, maximum: int, mode: int | None = None) -> bytes:
    """Read one bounded regular file without following its final component."""

    require(path.is_absolute() and not path.is_symlink(), "evidence_file_invalid")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise AccessibilityObserverError("evidence_file_unavailable") from error
    try:
        before = os.fstat(descriptor)
        require(
            stat.S_ISREG(before.st_mode)
            and 0 < before.st_size <= maximum
            and (mode is None or stat.S_IMODE(before.st_mode) == mode),
            "evidence_file_invalid",
        )
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(64 * 1024, maximum + 1 - total))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            require(total <= maximum, "evidence_file_invalid")
        after = os.fstat(descriptor)
        require(
            total == before.st_size
            and after.st_size == before.st_size
            and after.st_mtime_ns == before.st_mtime_ns,
            "evidence_file_changed",
        )
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def require_desktop_exclusion_contract(document: Mapping[str, Any]) -> None:
    """Bind the all-N/A desktop row to the deliberate AT-SPI exclusion."""

    qml_path = ROOT / "src" / "shell" / "desktop" / "DesktopSurface.qml"
    workflow_path = ROOT / "tests" / "integration" / "pd2_tier_c_workflow.py"
    adapter_path = ROOT / "tests" / "integration" / "pd2_live_xorg.py"
    try:
        qml_source = read_regular_file(qml_path, 16_384).decode("utf-8")
        workflow_source = read_regular_file(workflow_path, 524_288).decode("utf-8")
        adapter_source = read_regular_file(adapter_path, 524_288).decode("utf-8")
    except UnicodeError as error:
        raise AccessibilityObserverError("desktop_exclusion_contract_invalid") from error
    require(
        qml_source.count("Accessible.ignored: true") == 1
        and qml_source.count("Accessible.role: Accessible.Pane") == 1
        and 'Accessible.name: qsTr("Desktop wallpaper")' in qml_source
        and workflow_source.count('"tier_c_desktop_accessibility_excluded"') >= 2
        and adapter_source.count('TierCPortResult("tier_c_desktop_accessibility_excluded")')
        == 1
        and "desktop" in _result_ids(document)["tier_c_replay"],
        "desktop_exclusion_contract_invalid",
    )


def installed_artifact_sha256(document: Mapping[str, Any]) -> str:
    """Derive the lifecycle collector's canonical five-executable identity."""

    records = document.get("installed_executables")
    require(isinstance(records, Sequence), "installed_demo_evidence_invalid")
    normalized: list[dict[str, str]] = []
    for record in records:
        require(
            isinstance(record, Mapping)
            and set(record) == {"name", "sha256"}
            and isinstance(record["name"], str)
            and isinstance(record["sha256"], str)
            and re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) is not None,
            "installed_demo_evidence_invalid",
        )
        normalized.append({
            "path": f"/usr/bin/{record['name']}",
            "sha256": record["sha256"],
        })
    require(
        [record["name"] for record in records] == [
            "prismdrake-session", "prismdrake-settingsd", "prismdrake-notifyd",
            "prismdrake-shell", "prismdrake-shellctl",
        ],
        "installed_demo_evidence_invalid",
    )
    payload = json.dumps(
        normalized, allow_nan=False, ensure_ascii=True,
        separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def package_test_evidence() -> Mapping[str, Any]:
    """Validate the exact-inventory/JUnit package-test stage once per matrix."""

    configured = os.environ.get("PRISMDRAKE_PD2_PACKAGE_TEST_EVIDENCE", "")
    path = Path(configured)
    require(
        bool(configured) and path.is_absolute(),
        "package_test_evidence_unavailable",
    )
    try:
        raw = read_regular_file(path, 524_288, 0o600)
        document = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=duplicate_safe_object,
            parse_constant=reject_constant,
        )
    except (OSError, UnicodeError, ValueError) as error:
        raise AccessibilityObserverError("package_test_evidence_invalid") from error
    require(
        isinstance(document, dict) and set(document) == {
            "schema_version", "stage_kind", "package_test",
        } and document["schema_version"] == 1
        and document["stage_kind"] == "pd2_package_test",
        "package_test_evidence_invalid",
    )
    package = document["package_test"]
    required_fields = {
        "source_revision", "ebuild_sha256", "artifact_sha256", "features_test",
        "result", "selected_test_count", "executed_test_count",
        "passed_test_count", "skipped_test_count", "skipped_tests",
        "disabled_test_count", "not_run_test_count", "test_manifest_sha256",
        "inventory_contract_sha256", "excluded_tests",
    }
    require(isinstance(package, dict) and set(package) == required_fields,
            "package_test_evidence_invalid")
    inventory_path = ROOT / "tests" / "gentoo" / "pd2-ctest-inventory-v1.json"
    ebuild_path = (
        ROOT / "packaging" / "gentoo" / "repository" / "x11-misc"
        / "prismdrake" / "prismdrake-9999.ebuild"
    )
    try:
        inventory_raw = inventory_path.read_bytes()
        inventory = json.loads(
            inventory_raw.decode("utf-8"),
            object_pairs_hook=duplicate_safe_object,
            parse_constant=reject_constant,
        )
    except (OSError, UnicodeError, ValueError) as error:
        raise AccessibilityObserverError("ctest_inventory_contract_invalid") from error
    excluded = inventory.get("excluded_tests") if isinstance(inventory, dict) else None
    exact_count = inventory.get("exact_test_count") if isinstance(inventory, dict) else None
    selected_count = (
        exact_count - len(excluded)
        if isinstance(exact_count, int) and isinstance(excluded, list)
        else -1
    )
    sha_pattern = re.compile(r"[0-9a-f]{64}")
    require(
        package["source_revision"] == binding()["source_revision"]
        and package["ebuild_sha256"]
        == hashlib.sha256(read_regular_file(ebuild_path, 524_288)).hexdigest()
        and package["features_test"] is True and package["result"] == "passed"
        and package["selected_test_count"] == selected_count
        and package["executed_test_count"] == selected_count
        and package["passed_test_count"] == selected_count - 1
        and package["skipped_test_count"] == 1
        and package["skipped_tests"] == [{
            "test_id": "AtomicFileTest.RejectsDestinationOwnedByAnotherUser",
            "reason": "requires_root_ownership_transition",
        }]
        and package["disabled_test_count"] == 0
        and package["not_run_test_count"] == 0
        and package["excluded_tests"] == excluded
        and package["inventory_contract_sha256"]
        == hashlib.sha256(inventory_raw).hexdigest()
        and all(
            isinstance(package[field], str)
            and sha_pattern.fullmatch(package[field]) is not None
            for field in (
                "ebuild_sha256", "artifact_sha256", "test_manifest_sha256",
                "inventory_contract_sha256",
            )
        ),
        "package_test_evidence_invalid",
    )
    return package


def accessibility_result(
    surface: str, check: str, package_test: Mapping[str, Any] | None = None,
    installed_document: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    require(surface in SURFACES and check in MATRIX_CHECKS, "accessibility_probe_invalid")
    if installed_document is None:
        require_gi_atspi()
        document = installed_evidence()
    else:
        document = installed_document
    key = (surface, check)
    required = OBSERVED.get(key)
    if required is not None:
        observed = _result_ids(document)
        require(
            all(identifier in observed[phase] for phase, identifier in required),
            "installed_demo_observation_missing",
        )
        return passed()
    ctest_name = CTEST_OBSERVED.get(key)
    if ctest_name is not None:
        require(package_test is not None, "package_test_evidence_unavailable")
        require(
            ctest_name in {value[0] for value in CTEST_CHECKS.values()},
            "accessibility_ctest_contract_invalid",
        )
        cases = CTEST_CASE_EVIDENCE.get(key, ())
        case_note = f" ({', '.join(cases)})" if cases else ""
        return passed(
            f"Exact source inventory and passing JUnit include {ctest_name}{case_note}"
        )
    if key in NOT_APPLICABLE:
        return reviewed_skip("not_applicable", NOT_APPLICABLE[key])
    require(key in UNIMPLEMENTED, "accessibility_coverage_map_incomplete")
    return reviewed_skip(
        "live_adapter_unimplemented",
        UNIMPLEMENTED_REASONS[key],
    )


def matrix_result() -> dict[str, Any]:
    """Resolve the complete matrix with one package-test validation."""

    require_gi_atspi()
    document = installed_evidence()
    package = package_test_evidence()
    require_desktop_exclusion_contract(document)
    require(
        package["artifact_sha256"] == installed_artifact_sha256(document),
        "package_test_artifact_mismatch",
    )
    return {
        "matrix": [
            {
                "surface": surface,
                "checks": {
                    check: accessibility_result(surface, check, package, document)
                    for check in MATRIX_CHECKS
                },
            }
            for surface in SURFACES
        ]
    }


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
    parser.add_argument("--matrix", action="store_true")
    options = parser.parse_args()
    require(
        sum((
            options.global_check is not None,
            options.surface is not None and options.check is not None,
            options.matrix,
        )) == 1,
        "arguments_invalid",
    )
    return options


def main() -> int:
    try:
        options = parse_arguments()
        if options.matrix:
            result = matrix_result()
        elif options.global_check is not None:
            result = global_result(options.global_check)
        else:
            result = accessibility_result(options.surface, options.check)
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
