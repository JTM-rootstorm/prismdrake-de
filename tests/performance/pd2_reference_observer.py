#!/usr/bin/env python3
"""Observe the defensible WP15 endpoints available on the Gentoo reference VM.

The executable implements every collector role through one closed JSON
protocol.  It measures only endpoints with an existing exact runner, executes
the established i18n tests, validates installed-demo cleanup evidence, and
returns reviewed skips for probes that still need live-session orchestration.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, NoReturn, Sequence


ROOT = Path(__file__).resolve().parents[2]
MAX_INPUT_BYTES = 524_288
MAX_COMMAND_BYTES = 2_097_152
SAFE_ID = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
REVISION = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
SAFE_TEXT = re.compile(r"[A-Za-z0-9][A-Za-z0-9 .,:;_+()'=-]{0,255}")

PERFORMANCE_PROBES = {
    ("profile_switch", None): ("profile_switch_publication", None),
    ("catalog_refresh", None): ("desktop_entry_discovery", 256),
    ("search_response", "16"): ("application_search_response", 16),
    ("search_response", "64"): ("application_search_response", 64),
    ("search_response", "256"): ("application_search_response", 256),
}

I18N_TESTS = {
    "qt_translation_routing": r"^TranslationCatalogTest\.",
    "rtl_fixture": (
        r"^(TranslationCatalogTest\.LoadsRtlPseudoLocaleWithoutOwningLayoutPolicy|"
        r"PanelSurfaceQmlTest|LauncherSurfaceQmlTest|NotificationCenterQmlTest|"
        r"QuickSettingsSurfaceQmlTest)$"
    ),
    "long_string_fixture": (
        r"^(TranslationCatalogTest\.LoadsExpandedPseudoLocaleAndRestoresSourceStrings|"
        r"TranslationCatalogTest\.PreservesPlaceholdersInExpandedPseudoLocale|"
        r"PanelSurfaceQmlTest|LauncherSurfaceQmlTest|NotificationCenterQmlTest|"
        r"QuickSettingsSurfaceQmlTest)$"
    ),
    "locale_aware_clock": (
        r"^PanelClockTest\.FormatsLocaleTwelveAndTwentyFourHourModesDeterministically$"
    ),
    "localized_desktop_search": (
        r"^ApplicationSearchTest\.SearchesParserSelectedLocalizedValuesAndBaseCategories$"
    ),
}

# These combinations have no corresponding state on the named semantic object.
# The collector still requires the operator to list every returned skip.
NOT_APPLICABLE_ACCESSIBILITY = {
    ("panel", "state_selected"),
    ("panel", "state_urgent"),
    ("panel", "state_minimized"),
    ("panel", "state_checked"),
    ("panel", "state_expanded"),
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
    ("logout_confirmation", "state_expanded"),
}


class ObserverError(RuntimeError):
    """One closed observer failure."""


class DuplicateJsonKeyError(ValueError):
    """A JSON object repeated a key."""


class ClosedArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        del message
        raise ObserverError("arguments_invalid")


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise ObserverError(identifier)


def duplicate_safe_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError("duplicate key")
        result[key] = value
    return result


def reject_constant(value: str) -> Any:
    del value
    raise ValueError("non finite number")


def finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError("non finite number")
    return parsed


def load_json(path: Path, maximum: int = MAX_INPUT_BYTES) -> Any:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise ObserverError("evidence_read_failed") from error
    require(0 < len(raw) <= maximum, "evidence_size_invalid")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=duplicate_safe_object,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise ObserverError("evidence_json_invalid") from error


def required_environment(name: str, pattern: re.Pattern[str]) -> str:
    value = os.environ.get(name, "")
    require(pattern.fullmatch(value) is not None, "binding_environment_invalid")
    return value


def binding() -> dict[str, str]:
    return {
        "source_revision": required_environment(
            "PRISMDRAKE_PD2_SOURCE_REVISION", REVISION
        ),
        "reference_environment_id": required_environment(
            "PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID", SAFE_ID
        ),
        "method_id": required_environment(
            "PRISMDRAKE_PD2_METHOD_ID", re.compile(r"pd2-live-v1")
        ),
    }


def configured_path(name: str, defaults: Sequence[Path] = ()) -> Path:
    configured = os.environ.get(name)
    candidates = (Path(configured),) if configured else tuple(defaults)
    for candidate in candidates:
        if candidate.is_absolute() and candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise ObserverError("observer_dependency_unavailable")


def configured_regular_file(name: str) -> Path:
    value = os.environ.get(name, "")
    path = Path(value)
    require(
        bool(value) and path.is_absolute() and path.is_file() and not path.is_symlink(),
        "observer_evidence_unavailable",
    )
    return path


def run_command(arguments: Sequence[str], timeout: float = 120.0) -> subprocess.CompletedProcess[bytes]:
    try:
        completed = subprocess.run(
            list(arguments),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise ObserverError("observer_command_failed") from error
    require(
        len(completed.stdout) <= MAX_COMMAND_BYTES
        and len(completed.stderr) <= MAX_COMMAND_BYTES,
        "observer_command_output_invalid",
    )
    return completed


def command_text(arguments: Sequence[str]) -> str:
    completed = run_command(arguments, 15.0)
    require(
        completed.returncode == 0 and not completed.stderr and completed.stdout,
        "observer_command_failed",
    )
    try:
        return completed.stdout.decode("utf-8").strip()
    except UnicodeError as error:
        raise ObserverError("observer_command_output_invalid") from error


def safe_text(value: str, identifier: str) -> str:
    require(SAFE_TEXT.fullmatch(value) is not None, identifier)
    return value


def compiler(family: str, environment_name: str, defaults: Sequence[Path]) -> dict[str, str] | None:
    try:
        executable = configured_path(environment_name, defaults)
    except ObserverError:
        if os.environ.get(environment_name):
            raise
        return None
    version = command_text((str(executable), "-dumpfullversion", "-dumpversion")).splitlines()[0]
    return {"family": family, "version": safe_text(version, "compiler_version_invalid")}


def observe_window_manager() -> str:
    xprop = configured_path("PRISMDRAKE_PD2_XPROP", (Path("/usr/bin/xprop"),))
    root = command_text((str(xprop), "-root", "_NET_SUPPORTING_WM_CHECK"))
    match = re.search(r"0x[0-9a-fA-F]+", root)
    require(match is not None, "window_manager_observation_failed")
    name_line = command_text((str(xprop), "-id", match.group(0), "_NET_WM_NAME", "WM_NAME"))
    match = re.search(r'=\s*"([^"\\]{1,64})"\s*$', name_line)
    require(match is not None, "window_manager_observation_failed")
    normalized = re.sub(r"[^a-z0-9]+", "_", match.group(1).lower()).strip("_")
    require(SAFE_ID.fullmatch(normalized) is not None, "window_manager_identity_invalid")
    return normalized


def environment_result() -> dict[str, Any]:
    require(bool(os.environ.get("DISPLAY")), "display_environment_unavailable")
    try:
        os_release = platform.freedesktop_os_release()
    except OSError as error:
        raise ObserverError("operating_system_observation_failed") from error
    operating_system = safe_text(
        os_release.get("PRETTY_NAME", os_release.get("NAME", "")),
        "operating_system_identity_invalid",
    )
    architecture = safe_text(platform.machine(), "architecture_identity_invalid")
    qmake = configured_path(
        "PRISMDRAKE_PD2_QMAKE",
        (Path("/usr/lib64/qt6/bin/qmake"), Path("/usr/bin/qmake6")),
    )
    qt_version = command_text((str(qmake), "-query", "QT_VERSION"))
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", qt_version) is not None,
            "qt_version_invalid")
    compilers = [
        value
        for value in (
            compiler("gcc", "PRISMDRAKE_PD2_GCC", (Path("/usr/bin/gcc"),)),
            compiler("clang", "PRISMDRAKE_PD2_CLANG", (Path("/usr/bin/clang"),)),
        )
        if value is not None
    ]
    require(1 <= len(compilers) <= 2, "compiler_environment_invalid")
    return {
        "kind": "reference_vm",
        "reference_environment_id": binding()["reference_environment_id"],
        "operating_system": operating_system,
        "architecture": architecture,
        "qt_version": qt_version,
        "display_backend": "x11",
        "window_manager": observe_window_manager(),
        "compilers": compilers,
    }


def validate_performance_document(document: Any, revision_value: str) -> None:
    require(isinstance(document, dict), "performance_evidence_invalid")
    performance_path = str(Path(__file__).resolve().parent)
    tools_path = str(ROOT / "tools")
    if performance_path not in sys.path:
        sys.path.insert(0, performance_path)
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    from validate import validate_schema  # pylint: disable=import-outside-toplevel
    from validate_evidence import (  # pylint: disable=import-outside-toplevel
        FIXTURE_SIZES,
        MEASUREMENT_SHAPE,
        valid_summary,
    )

    schema = load_json(Path(__file__).with_name("performance-evidence.schema.json"))
    require(
        not validate_schema(document, schema, schema, "performance_evidence"),
        "performance_evidence_schema_invalid",
    )
    require(document.get("source_revision") == revision_value,
            "performance_evidence_revision_mismatch")
    require(document.get("reference_environment_id") == binding()["reference_environment_id"],
            "performance_evidence_environment_mismatch")
    method = document["method"]
    measurements = document["measurements"]
    shape = [(item["name"], item["fixture_count"]) for item in measurements]
    require(method["fixture_sizes"] == FIXTURE_SIZES and shape == MEASUREMENT_SHAPE,
            "performance_evidence_shape_invalid")
    require(
        all(valid_summary(item["statistics"], method["measured_iterations"])
            for item in measurements),
        "performance_evidence_summary_invalid",
    )


def measurement(document: dict[str, Any], name: str, fixture_count: int | None) -> dict[str, Any]:
    matches = [
        item for item in document["measurements"]
        if item["name"] == name and item["fixture_count"] == fixture_count
    ]
    require(len(matches) == 1, "performance_measurement_missing")
    return matches[0]


def reviewed_skip(reason_code: str, reason: str) -> dict[str, Any]:
    return {
        "status": "skipped",
        "reviewed": True,
        "reason_code": reason_code,
        "reason": reason,
    }


def performance_result(group: str, endpoint: str | None) -> dict[str, Any]:
    probe = PERFORMANCE_PROBES.get((group, endpoint))
    if probe is None:
        return reviewed_skip(
            "live_adapter_unimplemented",
            "Endpoint still requires an installed live session observer",
        )
    current_revision = binding()["source_revision"]
    baseline_revision = required_environment(
        "PRISMDRAKE_PD2_PD1_REFERENCE_REVISION", REVISION
    )
    baseline = load_json(configured_regular_file("PRISMDRAKE_PD2_PD1_EVIDENCE"))
    validate_performance_document(baseline, baseline_revision)
    runner = configured_path("PRISMDRAKE_PD2_PERFORMANCE_RUNNER")
    completed = run_command(
        (
            str(runner), "--revision", current_revision,
            "--environment-id", binding()["reference_environment_id"],
            "--iterations", "7",
        ),
        60.0,
    )
    require(completed.returncode == 0 and not completed.stderr,
            "performance_runner_failed")
    require(0 < len(completed.stdout) <= MAX_INPUT_BYTES,
            "performance_runner_output_invalid")
    try:
        current = json.loads(
            completed.stdout.decode("utf-8"),
            object_pairs_hook=duplicate_safe_object,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise ObserverError("performance_runner_output_invalid") from error
    validate_performance_document(current, current_revision)
    name, fixture_count = probe
    current_item = measurement(current, name, fixture_count)
    baseline_item = measurement(baseline, name, fixture_count)
    samples = [sample / 1_000_000.0 for sample in current_item["statistics"]["samples_ns"]]
    baseline_median = baseline_item["statistics"]["median_ns"] / 1_000_000.0
    comparison: dict[str, Any] = {
        "status": "no_regression",
        "statistic": "median",
        "pd1_value": baseline_median,
    }
    if statistics.median(samples) > baseline_median:
        comparison = {
            "status": "regression",
            "statistic": "median",
            "pd1_value": baseline_median,
            "explanation": "Current model median exceeds the selected PD1 reference median",
        }
    return {
        "status": "observed",
        "unit": "milliseconds",
        "samples": samples,
        "method": "Steady clock model runner with a controlled reference fixture",
        "pd1_comparison": comparison,
    }


def accessibility_result(surface: str | None, check: str | None,
                         global_check: str | None) -> dict[str, Any]:
    if global_check is not None:
        if not os.environ.get("DISPLAY"):
            return reviewed_skip(
                "runtime_surface_unavailable",
                "The isolated graphical session is not active",
            )
        if global_check == "live_atspi":
            dependency = run_command((sys.executable, "-c", "import pyatspi"), 10.0)
            if dependency.returncode != 0:
                return reviewed_skip(
                    "dependency_unavailable",
                    "The reference environment does not provide the AT SPI Python bridge",
                )
        elif global_check == "screen_reader_workflow" and not Path("/usr/bin/orca").is_file():
            return reviewed_skip(
                "dependency_unavailable",
                "The reference environment does not provide Orca",
            )
        return reviewed_skip(
            "live_adapter_unimplemented",
            "The check requires controlled live session orchestration",
        )
    require(surface is not None and check is not None, "accessibility_probe_invalid")
    if (surface, check) in NOT_APPLICABLE_ACCESSIBILITY:
        return reviewed_skip(
            "not_applicable",
            "The named semantic object does not expose this state",
        )
    return reviewed_skip(
        "live_adapter_unimplemented",
        "The check requires the installed live AT SPI surface observer",
    )


def ctest_result(check: str) -> dict[str, Any]:
    expression = I18N_TESTS.get(check)
    if expression is None:
        require(check == "fragile_fragment_audit", "internationalization_probe_invalid")
        return fragile_fragment_result()
    ctest = configured_path("PRISMDRAKE_PD2_CTEST", (Path("/usr/bin/ctest"),))
    build = Path(os.environ.get("PRISMDRAKE_PD2_BUILD_DIRECTORY", ""))
    require(build.is_absolute() and build.is_dir(), "build_directory_invalid")
    completed = run_command(
        (
            str(ctest), "--test-dir", str(build), "--output-on-failure",
            "--no-tests=error", "-R", expression,
        ),
        180.0,
    )
    method = "Executed the bounded CTest internationalization fixture selection"
    if completed.returncode != 0:
        return {"status": "failed", "method": method,
                "finding": "The selected internationalization tests did not pass"}
    try:
        output = completed.stdout.decode("utf-8")
    except UnicodeError as error:
        raise ObserverError("ctest_output_invalid") from error
    require(re.search(r"(?m)^\s*[1-9][0-9]*/[1-9][0-9]* Test", output) is not None,
            "ctest_selection_empty")
    return {"status": "passed", "method": method}


def fragile_fragment_result() -> dict[str, str]:
    fragments = (
        re.compile(r"qsTr\s*\([^;\n]{0,512}\)\s*\+"),
        re.compile(r"\+\s*qsTr\s*\("),
        re.compile(r"\btr\s*\([^;\n]{0,512}\)\s*\+"),
        re.compile(r"\+\s*\btr\s*\("),
    )
    try:
        files = sorted(
            path for path in (ROOT / "src").rglob("*")
            if path.suffix in {".cpp", ".hpp", ".qml"} and path.is_file()
        )
        require(files, "fragment_audit_sources_missing")
        for path in files:
            raw = path.read_bytes()
            require(len(raw) <= 2_097_152, "fragment_audit_source_oversized")
            text = raw.decode("utf-8")
            if any(pattern.search(text) is not None for pattern in fragments):
                return {
                    "status": "failed",
                    "method": "Bounded source audit for concatenated translated fragments",
                    "finding": "A translated fragment is concatenated in a source file",
                }
    except (OSError, UnicodeError) as error:
        raise ObserverError("fragment_audit_failed") from error
    return {
        "status": "passed",
        "method": "Bounded source audit for concatenated translated fragments",
    }


def cleanup_result() -> dict[str, bool]:
    evidence_path = configured_regular_file("PRISMDRAKE_PD2_INSTALLED_DEMO_EVIDENCE")
    integration_path = str(ROOT / "tests" / "integration")
    if integration_path not in sys.path:
        sys.path.insert(0, integration_path)
    import pd2_demo  # pylint: disable=import-outside-toplevel

    document = load_json(evidence_path, pd2_demo.MAX_EVIDENCE_BYTES)
    try:
        pd2_demo.validate_evidence(document)
    except pd2_demo.HarnessError as error:
        raise ObserverError("installed_demo_evidence_invalid") from error
    require(document.get("source_revision") == binding()["source_revision"],
            "installed_demo_revision_mismatch")
    return {"clean": True}


def parse_arguments() -> argparse.Namespace:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument(
        "--role",
        required=True,
        choices=("environment", "performance", "accessibility", "internationalization", "cleanup"),
    )
    parser.add_argument("--group")
    parser.add_argument("--endpoint")
    parser.add_argument("--surface")
    parser.add_argument("--check")
    parser.add_argument("--global", dest="global_check")
    options = parser.parse_args()
    role_arguments = {
        "environment": (None, None, None, None, None),
        "performance": (options.group, options.endpoint, None, None, None),
        "accessibility": (None, None, options.surface, options.check, options.global_check),
        "internationalization": (None, None, None, options.check, None),
        "cleanup": (None, None, None, None, None),
    }
    expected = role_arguments[options.role]
    actual = (options.group, options.endpoint, options.surface, options.check, options.global_check)
    require(actual == expected, "arguments_invalid")
    if options.role == "performance":
        require(options.group is not None, "arguments_invalid")
    elif options.role == "accessibility":
        require(
            (options.global_check is not None) !=
            (options.surface is not None and options.check is not None),
            "arguments_invalid",
        )
    elif options.role == "internationalization":
        require(options.check is not None, "arguments_invalid")
    return options


def observe(options: argparse.Namespace) -> dict[str, Any]:
    if options.role == "environment":
        return environment_result()
    if options.role == "performance":
        return performance_result(options.group, options.endpoint)
    if options.role == "accessibility":
        return accessibility_result(options.surface, options.check, options.global_check)
    if options.role == "internationalization":
        return ctest_result(options.check)
    return cleanup_result()


def main() -> int:
    try:
        options = parse_arguments()
        envelope = {"binding": binding(), "result": observe(options)}
        print(json.dumps(envelope, ensure_ascii=True, separators=(",", ":"), sort_keys=True))
        return 0
    except (ObserverError, OSError, subprocess.SubprocessError) as error:
        identifier = str(error) if isinstance(error, ObserverError) else "external_operation_failed"
        print(f"pd2_reference_observer: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
