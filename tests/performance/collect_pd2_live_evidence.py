#!/usr/bin/env python3
"""Run the closed command-backed PD2 WP15 collector on the reference VM."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

from collector_common import (
    ClosedArgumentParser,
    CollectorError,
    environment_id,
    executable,
    positive_integer,
    revision,
)
from pd2_evidence_contract import EvidenceError
from pd2_live_evidence import (
    LIVE_METHOD_ID,
    SKIP_CODES,
    CommandLiveEvidencePort,
    LiveCollectionConfig,
    ObserverCommand,
    ObserverCommands,
    collect_evidence,
    write_evidence,
)


ROOT = Path(__file__).resolve().parents[2]
SAFE_TEXT = re.compile(r"[A-Za-z0-9][A-Za-z0-9 .,:;_+()'=-]{0,255}")
OBSERVER_ENVIRONMENT_KEYS = (
    "AT_SPI_BUS_ADDRESS",
    "DBUS_SESSION_BUS_ADDRESS",
    "DISPLAY",
    "LANG",
    "LC_ALL",
    "QT_ACCESSIBILITY",
    "QT_LINUX_ACCESSIBILITY_ALWAYS_ON",
    "QT_QPA_PLATFORM",
    "QT_QUICK_BACKEND",
    "QT_QUICK_CONTROLS_STYLE",
    "TZ",
    "XDG_RUNTIME_DIR",
    "PRISMDRAKE_PD2_BUILD_DIRECTORY",
    "PRISMDRAKE_PD2_PERFORMANCE_RUNNER",
    "PRISMDRAKE_PD2_ASSET_PERFORMANCE_RUNNER",
    "PRISMDRAKE_PD2_RUNTIME_PERFORMANCE_EVIDENCE",
    "PRISMDRAKE_PD2_IDLE_SERIES",
    "PRISMDRAKE_PD2_PD1_EVIDENCE",
    "PRISMDRAKE_PD2_INSTALLED_DEMO_EVIDENCE",
)


def safe_text(value: str) -> str:
    if SAFE_TEXT.fullmatch(value) is None:
        raise CollectorError("invalid_safe_text")
    return value


def evidence_id(value: str) -> str:
    if re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,63}", value) is None:
        raise CollectorError("invalid_evidence_id")
    return value


def reviewed_skip(value: str) -> tuple[str, str]:
    key, separator, reason_code = value.partition("=")
    if not separator or not key or reason_code not in SKIP_CODES:
        raise CollectorError("invalid_reviewed_skip")
    return key, reason_code


def output_path(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.parent.is_dir() or path.exists():
        raise CollectorError("invalid_output_path")
    return path


def parse_arguments() -> object:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument("--source-revision", required=True, type=revision)
    parser.add_argument("--environment-id", required=True, type=environment_id)
    parser.add_argument("--pd1-reference-revision", required=True, type=revision)
    parser.add_argument("--pd1-evidence-id", required=True, type=evidence_id)
    parser.add_argument(
        "--search-catalog-size",
        action="append",
        required=True,
        type=lambda value: positive_integer(value, 1_000_000),
    )
    parser.add_argument("--reviewed-skip", action="append", default=[], type=reviewed_skip)
    parser.add_argument("--limitation", action="append", required=True, type=safe_text)
    parser.add_argument("--observer", type=executable)
    parser.add_argument("--environment-observer", type=executable)
    parser.add_argument("--performance-observer", type=executable)
    parser.add_argument("--accessibility-observer", type=executable)
    parser.add_argument("--internationalization-observer", type=executable)
    parser.add_argument("--cleanup-observer", type=executable)
    parser.add_argument("--output", required=True, type=output_path)
    return parser.parse_args()


def build_observer_environment(options: object) -> dict[str, str]:
    environment = {
        key: os.environ[key]
        for key in OBSERVER_ENVIRONMENT_KEYS
        if key in os.environ
    }
    environment.update(
        {
            "LC_ALL": "C.UTF-8",
            "PRISMDRAKE_PD2_SOURCE_REVISION": options.source_revision,
            "PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID": options.environment_id,
            "PRISMDRAKE_PD2_METHOD_ID": LIVE_METHOD_ID,
            "PRISMDRAKE_PD2_PD1_REFERENCE_REVISION": options.pd1_reference_revision,
        }
    )
    return environment


def observer_commands(options: object) -> ObserverCommands:
    """Resolve the unified observer with the dedicated AT-SPI implementation."""

    individual = (
        options.environment_observer,
        options.performance_observer,
        options.accessibility_observer,
        options.internationalization_observer,
        options.cleanup_observer,
    )
    if options.observer is not None:
        forbidden_overrides = (
            options.environment_observer,
            options.performance_observer,
            options.internationalization_observer,
            options.cleanup_observer,
        )
        if any(value is not None for value in forbidden_overrides):
            raise CollectorError("observer_mode_conflict")
        commands = ObserverCommands.unified(options.observer)
        if options.accessibility_observer is not None:
            commands = ObserverCommands(
                commands.environment,
                commands.performance,
                options.accessibility_observer,
                commands.internationalization,
                commands.cleanup,
            )
        return commands
    if any(value is None for value in individual):
        raise CollectorError("observer_set_incomplete")
    return ObserverCommands(*individual)


def attest_source_and_observers(
    source_revision: str,
    commands: ObserverCommands,
    *,
    git_executable: Path | None = None,
) -> None:
    """Bind every executable observer to one clean exact source checkout."""

    git = git_executable or Path("/usr/bin/git")
    if not git.is_absolute() or not git.is_file() or not os.access(git, os.X_OK):
        raise CollectorError("source_attestation_unavailable")

    def git_command(*arguments: str) -> subprocess.CompletedProcess[bytes]:
        try:
            return subprocess.run(
                (str(git), "-C", str(ROOT), *arguments),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=10.0,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise CollectorError("source_attestation_failed") from error

    head = git_command("rev-parse", "--verify", "HEAD")
    try:
        observed_revision = head.stdout.decode("ascii").strip()
    except UnicodeError as error:
        raise CollectorError("source_attestation_failed") from error
    if head.returncode != 0 or head.stderr or observed_revision != source_revision:
        raise CollectorError("source_revision_mismatch")

    root = git_command("rev-parse", "--show-toplevel")
    try:
        observed_root = Path(root.stdout.decode("utf-8").strip()).resolve()
    except (UnicodeError, OSError) as error:
        raise CollectorError("source_attestation_failed") from error
    if root.returncode != 0 or root.stderr or observed_root != ROOT.resolve():
        raise CollectorError("source_directory_mismatch")

    for arguments in (("diff", "--quiet"), ("diff", "--cached", "--quiet")):
        result = git_command(*arguments)
        if result.returncode != 0 or result.stdout or result.stderr:
            raise CollectorError("source_checkout_dirty")
    status = git_command("status", "--porcelain=v1", "--untracked-files=all")
    if status.returncode != 0 or status.stdout or status.stderr:
        raise CollectorError("source_checkout_dirty")

    source_root = ROOT.resolve()
    for value in (
        commands.environment,
        commands.performance,
        commands.accessibility,
        commands.internationalization,
        commands.cleanup,
    ):
        executable_path = (
            value.executable if isinstance(value, ObserverCommand) else value
        )
        try:
            resolved = executable_path.resolve(strict=True)
        except OSError as error:
            raise CollectorError("observer_source_binding_invalid") from error
        if executable_path.is_symlink() or not resolved.is_relative_to(source_root):
            raise CollectorError("observer_source_binding_invalid")


def run(options: object) -> None:
    skips = dict(options.reviewed_skip)
    if len(skips) != len(options.reviewed_skip):
        raise CollectorError("duplicate_reviewed_skip")
    config = LiveCollectionConfig(
        source_revision=options.source_revision,
        reference_environment_id=options.environment_id,
        pd1_reference={
            "status": "identified",
            "source_revision": options.pd1_reference_revision,
            "evidence_id": options.pd1_evidence_id,
        },
        search_catalog_sizes=tuple(options.search_catalog_size),
        reviewed_skips=skips,
        limitations=tuple(options.limitation),
    )
    commands = observer_commands(options)
    attest_source_and_observers(options.source_revision, commands)
    port = CommandLiveEvidencePort(commands, build_observer_environment(options))
    write_evidence(options.output, collect_evidence(config, port))


def main() -> int:
    try:
        run(parse_arguments())
        return 0
    except (CollectorError, EvidenceError, OSError, subprocess.SubprocessError) as error:
        identifier = (
            str(error)
            if isinstance(error, (CollectorError, EvidenceError))
            else "external_operation_failed"
        )
        print(f"collect_pd2_live_evidence: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
