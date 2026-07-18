#!/usr/bin/env python3
"""Require a performance-evidence CLI call to fail with one closed identifier."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        return 2
    executable = Path(sys.argv[1])
    expected_id = sys.argv[2]
    if not executable.is_absolute() or not expected_id.isidentifier():
        return 2
    completed = subprocess.run(
        [str(executable), *sys.argv[3:]],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=4,
        check=False,
    )
    expected = f"prismdrake-pd1-performance-evidence: {expected_id}\n"
    if completed.returncode != 2 or completed.stdout or completed.stderr != expected:
        print("assert_cli_failure: unexpected_cli_contract", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
