#!/usr/bin/env python3
"""Canonical file manifest for the installed PD2 demonstration harness."""

from __future__ import annotations

from collections.abc import Callable
import hashlib
import json
from pathlib import Path
from typing import Any


HARNESS_RELATIVE_PATHS = (
    "tests/gentoo/pd2-installed-artifact-attestation.schema.json",
    "tests/gentoo/pd2_harness_manifest.py",
    "tests/gentoo/pd2_installed_artifact_attestation.py",
    "tests/gentoo/pd2_lifecycle_hashes.py",
    "tests/integration/pd2-demo-evidence.schema.json",
    "tests/integration/pd2_demo.py",
    "tests/integration/pd2_live_atspi.py",
    "tests/integration/pd2_live_dbus.py",
    "tests/integration/pd2_live_lifecycle_workflow.py",
    "tests/integration/pd2_live_notification_workflow.py",
    "tests/integration/pd2_live_notifications.py",
    "tests/integration/pd2_live_settings_workflow.py",
    "tests/integration/pd2_live_xorg.py",
    "tests/integration/pd2_notification_failure_replay.py",
    "tests/integration/pd2_tier_c_workflow.py",
    "tests/integration/pd2_x11trace.py",
    "tests/integration/pd2_x11trace_supervisor.py",
    "tests/integration/pd2_xwd.py",
    "tools/validate.py",
)


def canonical_manifest_sha256(
        files: list[dict[str, str]], controlled_fixture_sha256: str,
) -> str:
    """Hash the complete normalized manifest and controlled fixture binding."""

    payload = {
        "controlled_fixture_sha256": controlled_fixture_sha256,
        "files": files,
    }
    encoded = json.dumps(
        payload, ensure_ascii=True, separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def capture_harness_manifest(
        source_root: Path, controlled_fixture: Path,
        hash_file: Callable[[Path], str],
) -> dict[str, Any]:
    """Hash every reviewed live harness input using stable repository paths."""

    files = [
        {"path": relative, "sha256": hash_file(source_root / relative)}
        for relative in HARNESS_RELATIVE_PATHS
    ]
    fixture_hash = hash_file(controlled_fixture)
    return {
        "kind": "pd2_installed_demonstration_harness",
        "manifest_sha256": canonical_manifest_sha256(files, fixture_hash),
        "controlled_fixture_sha256": fixture_hash,
        "files": files,
    }
