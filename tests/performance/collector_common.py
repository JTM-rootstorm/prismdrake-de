#!/usr/bin/env python3
"""Shared closed-contract helpers for PD1 external performance collectors."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


REVISION_PATTERN = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
ENVIRONMENT_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")


class CollectorError(RuntimeError):
    """One closed collector failure identifier."""


class ClosedArgumentParser(argparse.ArgumentParser):
    def error(self, _message: str) -> None:
        raise CollectorError("invalid_arguments")


def revision(value: str) -> str:
    if REVISION_PATTERN.fullmatch(value) is None:
        raise CollectorError("invalid_source_revision")
    return value


def environment_id(value: str) -> str:
    if ENVIRONMENT_PATTERN.fullmatch(value) is None:
        raise CollectorError("invalid_environment_id")
    return value


def executable(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.is_file():
        raise CollectorError("invalid_executable")
    return path


def positive_integer(value: str, maximum: int) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise CollectorError("invalid_integer") from error
    if parsed <= 0 or parsed > maximum:
        raise CollectorError("invalid_integer")
    return parsed


def redaction_contract() -> dict[str, bool]:
    return {
        "contains_filesystem_paths": False,
        "contains_process_or_thread_ids": False,
        "contains_host_or_user_names": False,
        "contains_application_or_window_content": False,
        "diagnostics_are_closed_ids": True,
    }


def summarize(samples: list[int]) -> dict[str, int | list[int]]:
    if not samples or any(sample < 0 for sample in samples):
        raise CollectorError("invalid_samples")
    ordered = sorted(samples)
    p95_index = ((len(ordered) - 1) * 95 + 99) // 100
    return {
        "sample_count": len(samples),
        "minimum_ns": ordered[0],
        "median_ns": ordered[(len(ordered) - 1) // 2],
        "p95_ns": ordered[p95_index],
        "maximum_ns": ordered[-1],
        "samples_ns": samples,
    }
