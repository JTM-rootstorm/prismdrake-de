#!/usr/bin/env python3
"""Display-free structural checks for the removable PD1 toolkit experiment."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def require(path: pathlib.Path, fragment: str) -> None:
    text = path.read_text(encoding="utf-8")
    if fragment not in text:
        raise AssertionError(f"{path.relative_to(ROOT)} lacks required fragment: {fragment}")


def forbid_tree(fragment: str) -> None:
    for path in ROOT.rglob("*"):
        if path.is_file() and path.suffix in {".cpp", ".hpp", ".qml", ".txt"}:
            if fragment in path.read_text(encoding="utf-8"):
                raise AssertionError(
                    f"{path.relative_to(ROOT)} contains forbidden fragment: {fragment}"
                )


def main() -> int:
    cmake = ROOT / "CMakeLists.txt"
    qml = ROOT / "qml" / "Main.qml"
    adapter = ROOT / "src" / "X11DockAdapter.cpp"

    require(cmake, "# This experiment deliberately has no install() rules")
    require(qml, "KeyNavigation.tab")
    require(qml, "KeyNavigation.backtab")
    require(qml, "Accessible.name")
    require(qml, 'sequence: "Escape"')
    require(adapter, '"_NET_WM_WINDOW_TYPE_DOCK"')
    require(adapter, '"_NET_WM_STRUT_PARTIAL"')
    forbid_tree("GW_")
    forbid_tree("grabWindow")
    forbid_tree("ScreenCapture")
    print("PD1 toolkit spike source-contract checks passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
