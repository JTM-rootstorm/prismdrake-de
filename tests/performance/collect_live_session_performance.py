#!/usr/bin/env python3
"""Collect startup and idle evidence from one bounded supervised live tree."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import collect_idle_wakeups
import collect_startup_to_panel
from collector_common import (
    ClosedArgumentParser,
    CollectorError,
    environment_id,
    executable,
    revision,
)


def artifact_path(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.parent.is_dir() or path.exists():
        raise CollectorError("invalid_artifact_path")
    return path


def parse_arguments() -> object:
    parser = ClosedArgumentParser(add_help=False)
    for name in ("session", "settingsd", "shell", "xev", "xprop", "stdbuf", "perf", "sleep"):
        parser.add_argument(f"--{name}", required=True, type=executable)
    parser.add_argument("--revision", required=True, type=revision)
    parser.add_argument("--environment-id", required=True, type=environment_id)
    parser.add_argument("--startup-output", required=True, type=artifact_path)
    parser.add_argument("--idle-output", required=True, type=artifact_path)
    options = parser.parse_args()
    if options.startup_output == options.idle_output:
        raise CollectorError("duplicate_artifact_path")
    return options


def write_artifact(path: Path, document: dict[str, object]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")


def run(options: object) -> None:
    startup_options = SimpleNamespace(
        **vars(options),
        timeout_seconds=collect_startup_to_panel.STARTUP_DEADLINE_SECONDS,
    )

    def collect_idle(live: collect_startup_to_panel.LiveSession) -> dict[str, object]:
        idle_options = SimpleNamespace(
            perf=options.perf,
            sleep=options.sleep,
            session_pid=live.session_pid,
            settingsd_pid=live.settingsd_pid,
            shell_pid=live.shell_pid,
            session_executable=options.session,
            settingsd_executable=options.settingsd,
            shell_executable=options.shell,
            xprop=options.xprop,
            runtime_directory=live.runtime_directory,
            revision=options.revision,
            environment_id=options.environment_id,
            duration_seconds=60,
        )
        return collect_idle_wakeups.collect(idle_options)

    result = collect_startup_to_panel.collect(startup_options, collect_idle)
    if not isinstance(result, tuple):
        raise CollectorError("live_collection_failed")
    startup, idle = result
    write_artifact(options.startup_output, startup)
    write_artifact(options.idle_output, idle)


def main() -> int:
    try:
        run(parse_arguments())
        return 0
    except (CollectorError, OSError, subprocess.SubprocessError) as error:
        identifier = str(error) if isinstance(error, CollectorError) else "external_operation_failed"
        print(f"collect_live_session_performance: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
