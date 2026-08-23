#!/usr/bin/env python3
"""List and run Prismdrake's explicitly bounded validation selectors."""

from __future__ import annotations

import argparse
import json
import os
import stat
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Mapping, Sequence


ROOT = Path(__file__).resolve().parent.parent
STATE_ROOT = ROOT / "build" / "validation-state"
PYTHON_EXECUTABLE_INPUT = sys.executable
PYTHON_EXECUTABLE = str(Path(sys.executable).resolve(strict=False))
RUNNABLE_TIERS = frozenset({"T0", "T1"})
SELECTOR_TIMEOUT_SECONDS = 300


class BoundaryError(RuntimeError):
    """A local selector failed a trust or state-confinement check."""


@dataclass(frozen=True)
class Selector:
    """One fixed validation entry point and its escalation boundary."""

    name: str
    tier: str
    description: str
    command: tuple[str, ...] | None
    runnable_locally: bool
    blocked_reason: str | None = None


SELECTORS = (
    Selector(
        "contracts",
        "T0",
        "Repository documents, schemas, manifests, and local links",
        (PYTHON_EXECUTABLE, "-I", "-B", "tools/validate.py"),
        True,
    ),
    Selector(
        "validation-selector-tests",
        "T1",
        "Unit tests for this validation-tier boundary",
        (
            PYTHON_EXECUTABLE,
            "-I",
            "-B",
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
            PYTHON_EXECUTABLE,
            "-I",
            "-B",
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
            PYTHON_EXECUTABLE,
            "-I",
            "-B",
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
        None,
        False,
        "a committed tier manifest and exact build identity are not yet established",
    ),
    Selector(
        "qml-lint",
        "T2",
        "Configured aggregate Qt QML lint target",
        None,
        False,
        "a committed tier manifest and exact build identity are not yet established",
    ),
    Selector(
        "package-checkpoint",
        "T3",
        "Package, install, and installed-runtime workflow",
        None,
        False,
        "package and installed-runtime work requires a separately authorized workflow",
    ),
    Selector(
        "live-environment",
        "T4",
        "Explicitly authorized hardware or final live-environment workflow",
        None,
        False,
        "hardware and live-environment work requires a separately authorized workflow",
    ),
)

SELECTOR_ENVIRONMENT_ALLOWLIST: Mapping[str, Mapping[str, str]] = {
    entry.name: {} for entry in SELECTORS if entry.tier in RUNNABLE_TIERS
}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "List or run a fixed Prismdrake validation selector. T2-T4 are "
            "never run by this local front end."
        )
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true", help="list selectors")
    mode.add_argument("--selector", help="select one exact entry by name")
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
        if entry.blocked_reason is not None:
            print(f"  blocked: {entry.blocked_reason}", flush=True)


def _trusted_python_executable() -> Path:
    candidate = Path(PYTHON_EXECUTABLE_INPUT)
    if not candidate.is_absolute():
        raise BoundaryError("the active Python executable is not an absolute path")

    try:
        resolved = candidate.resolve(strict=True)
        metadata = resolved.stat()
    except OSError as error:
        raise BoundaryError("the active Python executable cannot be resolved") from error

    if not stat.S_ISREG(metadata.st_mode) or not os.access(resolved, os.X_OK):
        raise BoundaryError("the active Python executable is not a regular executable file")
    if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise BoundaryError("the active Python executable is group- or world-writable")
    if _is_beneath(resolved, ROOT):
        raise BoundaryError("the active Python executable is inside the writable repository")
    return resolved


def _is_beneath(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _ensure_state_directory(path: Path) -> Path:
    if not path.is_absolute() or not _is_beneath(path, ROOT) or path == ROOT:
        raise BoundaryError("a validation state path is outside the repository")

    relative = path.relative_to(ROOT)
    current = ROOT
    for component in relative.parts:
        current = current / component
        if current.is_symlink():
            raise BoundaryError("a validation state path contains a symbolic link")
        if current.exists():
            if not current.is_dir():
                raise BoundaryError("a validation state path contains a non-directory")
        else:
            try:
                current.mkdir(mode=0o700)
            except FileExistsError:
                pass
            except OSError as error:
                raise BoundaryError("a validation state directory cannot be created") from error
        if current.is_symlink():
            raise BoundaryError("a validation state path contains a symbolic link")

    try:
        resolved = path.resolve(strict=True)
        metadata = path.lstat()
    except OSError as error:
        raise BoundaryError("a validation state directory cannot be resolved") from error
    if not _is_beneath(resolved, ROOT) or resolved != path:
        raise BoundaryError("a validation state path escaped the repository")
    if not stat.S_ISDIR(metadata.st_mode):
        raise BoundaryError("a validation state path is not a directory")

    if path == STATE_ROOT or STATE_ROOT in path.parents:
        if metadata.st_uid != os.getuid():
            raise BoundaryError("a validation state directory has an unexpected owner")
        if stat.S_IMODE(metadata.st_mode) & 0o077:
            raise BoundaryError("a validation state directory is not private")
    return resolved


def _prepare_state_paths(selector_name: str) -> dict[str, Path]:
    if selector_name not in SELECTOR_ENVIRONMENT_ALLOWLIST:
        raise BoundaryError("the selector has no local state policy")

    _ensure_state_directory(STATE_ROOT)
    selector_root = _ensure_state_directory(STATE_ROOT / selector_name)
    return {
        name: _ensure_state_directory(selector_root / name)
        for name in ("home", "tmp", "cache", "config", "data", "state", "runtime")
    }


def _selector_environment(entry: Selector) -> dict[str, str]:
    additions = SELECTOR_ENVIRONMENT_ALLOWLIST.get(entry.name)
    if additions is None:
        raise BoundaryError("the selector has no local environment policy")

    paths = _prepare_state_paths(entry.name)
    environment = {
        "HOME": str(paths["home"]),
        "TMPDIR": str(paths["tmp"]),
        "TMP": str(paths["tmp"]),
        "TEMP": str(paths["tmp"]),
        "XDG_CACHE_HOME": str(paths["cache"]),
        "XDG_CONFIG_HOME": str(paths["config"]),
        "XDG_DATA_HOME": str(paths["data"]),
        "XDG_STATE_HOME": str(paths["state"]),
        "XDG_RUNTIME_DIR": str(paths["runtime"]),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
    }
    environment.update(additions)
    return environment


def _run_selector(entry: Selector) -> int:
    try:
        python = _trusted_python_executable()
        if entry.command is None or entry.command[0] != PYTHON_EXECUTABLE:
            raise BoundaryError("the selector command is not bound to the active Python")
        environment = _selector_environment(entry)
        command = (str(python), *entry.command[1:])
        print(f"{entry.tier} {entry.name}: running", flush=True)
        completed = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            timeout=SELECTOR_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        print(f"{entry.tier} {entry.name}: timed out", flush=True)
        return 124
    except (BoundaryError, OSError):
        print(
            f"{entry.tier} {entry.name}: blocked by the local execution boundary",
            file=sys.stderr,
            flush=True,
        )
        return 2

    status = "passed" if completed.returncode == 0 else f"failed ({completed.returncode})"
    print(f"{entry.tier} {entry.name}: {status}", flush=True)
    return completed.returncode


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)

    if args.run and args.list:
        parser.error("--run cannot be combined with --list")
    if args.run and args.selector is None:
        parser.error("--run requires --selector")
    if args.json and not args.list:
        parser.error("--json is available only with --list")
    if args.json and args.run:
        parser.error("--json cannot be combined with --run")

    entries = _selected(args.selector, args.tier)
    if not entries:
        print("validation selector not found for the requested tier", file=sys.stderr)
        return 2

    if not args.run:
        _print_entries(entries, args.json)
        return 0

    entry = entries[0]
    if (
        entry.tier not in RUNNABLE_TIERS
        or not entry.runnable_locally
        or entry.command is None
    ):
        reason = entry.blocked_reason or "no local execution policy is defined"
        print(
            f"refusing {entry.tier} selector {entry.name}: {reason}",
            file=sys.stderr,
            flush=True,
        )
        return 2

    return _run_selector(entry)


if __name__ == "__main__":
    raise SystemExit(main())
