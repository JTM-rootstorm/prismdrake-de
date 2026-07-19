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
    ObserverCommands,
    collect_evidence,
    write_evidence,
)


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
    parser.add_argument("--environment-observer", required=True, type=executable)
    parser.add_argument("--performance-observer", required=True, type=executable)
    parser.add_argument("--accessibility-observer", required=True, type=executable)
    parser.add_argument("--internationalization-observer", required=True, type=executable)
    parser.add_argument("--cleanup-observer", required=True, type=executable)
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
        }
    )
    return environment


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
    commands = ObserverCommands(
        environment=options.environment_observer,
        performance=options.performance_observer,
        accessibility=options.accessibility_observer,
        internationalization=options.internationalization_observer,
        cleanup=options.cleanup_observer,
    )
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

