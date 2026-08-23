#!/usr/bin/env python3
"""List and run Prismdrake's explicitly bounded validation selectors."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parent.parent
LOCAL_TIERS = frozenset({"T0", "T1", "T2"})


@dataclass(frozen=True)
class Selector:
    """One fixed validation entry point and its escalation boundary."""

    name: str
    tier: str
    description: str
    command: tuple[str, ...] | None
    runnable_locally: bool


SELECTORS = (
    Selector(
        "contracts",
        "T0",
        "Repository documents, schemas, manifests, and local links",
        (sys.executable, "tools/validate.py"),
        True,
    ),
    Selector(
        "validation-selector-tests",
        "T1",
        "Unit tests for this validation-tier boundary",
        (
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/validation",
            "-p",
            "test_*.py",
        ),
        True,
    ),
    Selector(
        "gentoo-saved-evidence",
        "T1",
        "Saved Gentoo evidence validators; does not recollect evidence",
        (
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/gentoo",
            "-p",
            "test_*evidence.py",
        ),
        True,
    ),
    Selector(
        "performance-saved-evidence",
        "T1",
        "Saved performance evidence validators; does not recollect evidence",
        (
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/performance",
            "-p",
            "test_*evidence*.py",
        ),
        True,
    ),
    Selector(
        "compiled-gcc",
        "T2",
        "Configured GCC component, D-Bus, Qt/QML, and X11 CTest suite",
        ("ctest", "--preset", "gcc-debug"),
        True,
    ),
    Selector(
        "qml-lint",
        "T2",
        "Configured aggregate Qt QML lint target",
        ("cmake", "--build", "--preset", "clang-debug", "--target", "all_qmllint"),
        True,
    ),
    Selector(
        "package-checkpoint",
        "T3",
        "Package, install, and installed-runtime workflow",
        None,
        False,
    ),
    Selector(
        "live-environment",
        "T4",
        "Explicitly authorized hardware or final live-environment workflow",
        None,
        False,
    ),
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "List or run a fixed Prismdrake validation selector. T3/T4 are "
            "never run by this local front end."
        )
    )
    parser.add_argument("--list", action="store_true", help="list selectors")
    parser.add_argument("--selector", help="select one exact entry by name")
    parser.add_argument("--tier", choices=("T0", "T1", "T2", "T3", "T4"))
    parser.add_argument("--run", action="store_true", help="run the selected local entry")
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    return parser


def _selected(name: str | None, tier: str | None) -> list[Selector]:
    entries = list(SELECTORS)
    if name is not None:
        entries = [entry for entry in entries if entry.name == name]
    if tier is not None:
        entries = [entry for entry in entries if entry.tier == tier]
    return entries


def _record(entry: Selector) -> dict[str, object]:
    record = asdict(entry)
    if entry.command is not None:
        record["command"] = list(entry.command)
    return record


def _print_entries(entries: Sequence[Selector], as_json: bool) -> None:
    if as_json:
        print(json.dumps([_record(entry) for entry in entries], indent=2, sort_keys=True))
        return

    for entry in entries:
        command = " ".join(entry.command) if entry.command is not None else "BLOCKED"
        print(f"{entry.tier} {entry.name}: {command}", flush=True)
        print(f"  {entry.description}", flush=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)

    if args.run and args.selector is None:
        parser.error("--run requires --selector")
    if not args.list and args.selector is None:
        parser.error("choose --list or --selector")

    entries = _selected(args.selector, args.tier)
    if not entries:
        print("validation selector not found for the requested tier", file=sys.stderr)
        return 2

    _print_entries(entries, args.json)

    if not args.run:
        return 0

    entry = entries[0]
    if (
        entry.tier not in LOCAL_TIERS
        or not entry.runnable_locally
        or entry.command is None
    ):
        print(
            f"refusing implicit {entry.tier} escalation: {entry.name} must run "
            "through a separately authorized project-owned workflow",
            file=sys.stderr,
        )
        return 2

    completed = subprocess.run(entry.command, cwd=ROOT, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
