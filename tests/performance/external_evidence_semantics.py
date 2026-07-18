#!/usr/bin/env python3
"""Kind-specific semantic checks for PD1 external performance evidence."""

from __future__ import annotations

import re
from typing import Any


EXPECTED_CADENCE_SCENARIOS = [
    ("lustre", "lustre", False, False, True),
    ("forge", "forge", False, False, True),
    ("reduced_motion", "lustre", True, False, True),
    ("disabled_transparency", "lustre", False, True, True),
    ("missing_blur", "lustre", False, False, False),
]
EXPECTED_IDLE_COMPONENTS = ["session", "settingsd", "shell"]
SAFE_FONT_BASENAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}")
SHA256 = re.compile(r"[0-9a-f]{64}")
STARTUP_DEADLINE_NS = 10_000_000_000


def _startup_error(document: dict[str, Any]) -> str | None:
    method = document.get("method", {})
    result = document["result"]
    if method.get("deadline_ns") != STARTUP_DEADLINE_NS:
        return "startup_deadline_contract_invalid"
    if any(
        result[field] > STARTUP_DEADLINE_NS
        for field in ("duration_ns", "ready_marker_ns", "mapped_dock_ns")
    ):
        return "startup_deadline_contract_invalid"
    if result["duration_ns"] != max(result["ready_marker_ns"], result["mapped_dock_ns"]):
        return "startup_endpoint_semantics_invalid"
    if result["safe_mode"] or result["child_restart_observed"]:
        return "startup_recovery_state_invalid"
    if (
        result["mapped_dock_count"] != 1
        or result["foreign_dock_observed"]
        or result["duplicate_dock_observed"]
    ):
        return "startup_dock_ownership_invalid"
    return None


def _idle_error(document: dict[str, Any]) -> str | None:
    results = document["results"]
    if [item["component"] for item in results] != EXPECTED_IDLE_COMPONENTS:
        return "idle_component_order_invalid"
    method = document["method"]
    if (
        method["requested_interval_ns"] != 60_000_000_000
        or not method["contract_eligible"]
        or method["collection_scope"] != "system_wide"
        or method["process_identity"] != "pidfd_and_proc_executable_at_boundaries"
        or method["live_tree_ownership"] != "exact_session_direct_children"
        or method["startup_endpoint"]
        != "ready_marker_and_single_owned_mapped_dock"
    ):
        return "idle_method_invalid"
    if method["observed_interval_ns"] < method["requested_interval_ns"]:
        return "idle_interval_invalid"
    for item in results:
        if item["thread_count_start"] != item["thread_count_end"]:
            return "idle_thread_stability_invalid"
        if item["received_wakeup_count"] != (
            item["sched_wakeup_count"] + item["sched_wakeup_new_count"]
        ):
            return "idle_wakeup_sum_invalid"
    return None


def _cadence_error(document: dict[str, Any]) -> str | None:
    method = document["method"]
    if not (
        method["lang_environment"] == "C.UTF-8"
        and method["lc_all_environment"] == "C.UTF-8"
        and method["timezone_environment"] == "UTC"
        and method["qt_locale_name"] == "C"
        and method["runtime_utc_offset_seconds"] == 0
        and method["qpa_platform"] == method["qpa_platform_actual"] == "offscreen"
        and method["scenegraph_backend"]
        == method["graphics_backend_actual"]
        == "software"
        and method["qt_graphics_api_reported"]
        in {
            "unknown",
            "software",
            "openvg",
            "opengl",
            "direct3d11",
            "vulkan",
            "metal",
            "null",
            "direct3d12",
        }
        and method["font_requested_family"] == "sans-serif"
        and method["font_claimed_family"] == method["font_actual_family"]
        and method["font_resolution_source"]
        == "fontconfig_fc_match_at_configure_time"
        and SAFE_FONT_BASENAME.fullmatch(method["font_source_basename"])
        is not None
        and SHA256.fullmatch(method["font_source_sha256"]) is not None
    ):
        return "cadence_environment_invalid"
    observed = [
        (
            item["scenario"],
            item["profile"],
            item["reduced_motion"],
            item["transparency_disabled"],
            item["blur_available"],
        )
        for item in document["series"]
    ]
    if observed != EXPECTED_CADENCE_SCENARIOS:
        return "cadence_scenario_order_invalid"
    for item in document["series"]:
        statistics = item["statistics"]
        samples = statistics["samples_ns"]
        ordered = sorted(samples)
        p95_index = ((len(ordered) - 1) * 95 + 99) // 100
        if not (
            len(samples) == 240
            and statistics["sample_count"] == 240
            and statistics["minimum_ns"] == ordered[0]
            and statistics["median_ns"] == ordered[(len(ordered) - 1) // 2]
            and statistics["p95_ns"] == ordered[p95_index]
            and statistics["maximum_ns"] == ordered[-1]
            and statistics["above_25000000_ns"]
            == sum(sample > 25_000_000 for sample in samples)
        ):
            return "cadence_summary_invalid"
    return None


def semantic_error(document: dict[str, Any]) -> str | None:
    """Return one closed semantic failure identifier after schema validation."""
    kind = document["evidence_kind"]
    if kind == "startup_to_mapped_panel":
        return _startup_error(document)
    if kind == "idle_scheduler_wakeups":
        return _idle_error(document)
    if kind == "deterministic_visual_cadence":
        return _cadence_error(document)
    return "unsupported_evidence_kind"
