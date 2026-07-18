#!/usr/bin/env python3
"""Hermetic tests for canonical lifecycle collection and finalization."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from installed_artifact_attestation import example_attestation_document
from lifecycle_hashes import (
    canonical_artifact_sha256,
    canonical_linkage_sha256,
    canonical_ownership_sha256,
    canonical_tree_sha256,
)
from portage_lifecycle_collector import (
    CollectorError,
    atomic_write_json,
    capture_preflight_attestation,
    finalize_lifecycle,
)
from portage_lifecycle_evidence import example_evidence_document


class LifecycleCollectorTests(unittest.TestCase):
    def test_canonical_hashes_are_order_explicit(self) -> None:
        document = example_evidence_document()
        executables = document["installed"]["executables"]
        self.assertEqual(
            canonical_artifact_sha256(executables),
            document["tested_artifact"]["artifact_sha256"],
        )
        linkage = document["runtime_validation"]["linkage"]
        self.assertEqual(canonical_linkage_sha256(linkage["entries"]),
                         linkage["capture_sha256"])
        self.assertEqual(
            canonical_ownership_sha256(["/usr/share/b", "/usr/bin/a"]),
            canonical_ownership_sha256(["/usr/bin/a", "/usr/share/b"]),
        )

    def test_tree_hash_covers_files_directories_symlinks_and_absence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            root = parent / "tree"
            absent = canonical_tree_sha256(root)
            root.mkdir()
            (root / "directory").mkdir()
            (root / "directory" / "value").write_bytes(b"one")
            os.symlink("directory/value", root / "link")
            first = canonical_tree_sha256(root)
            self.assertNotEqual(absent, first)
            (root / "directory" / "value").write_bytes(b"two")
            self.assertNotEqual(first, canonical_tree_sha256(root))

    def test_finalizer_requires_exact_preflight_bindings(self) -> None:
        attestation = example_attestation_document()
        draft = example_evidence_document()
        self.assertIs(finalize_lifecycle(attestation, draft), draft)
        changed = copy.deepcopy(draft)
        changed["runtime_validation"]["complete_demo"]["driver_sha256"] = "8" * 64
        with self.assertRaisesRegex(CollectorError, "^final_demo_driver_mismatch$"):
            finalize_lifecycle(attestation, changed)

    def test_preflight_capture_measures_exact_installed_paths_and_driver(self) -> None:
        example = example_attestation_document()
        source = example["source"]
        owned_files = list(example["installed"]["ownership"]["paths"])
        owned_files.extend(f"/usr/share/prismdrake/fixture-{index}" for index in range(10))
        hashes = {
            record["path"]: record["sha256"]
            for record in example["installed"]["executables"]
        }
        hashes["/source/pd1_demo.py"] = example["demo_driver"]["sha256"]
        with (
            mock.patch.object(Path, "is_file", return_value=True),
            mock.patch.object(Path, "is_symlink", return_value=False),
            mock.patch("portage_lifecycle_collector.os.access", return_value=True),
        ):
            captured = capture_preflight_attestation(
                source,
                owned_files,
                Path("/source/pd1_demo.py"),
                hash_file=lambda path: hashes[str(path)],
            )
        self.assertEqual(captured["installed"]["ownership"]["owned_file_count"], 13)
        self.assertEqual(captured["demo_driver"]["sha256"], "7" * 64)
        self.assertEqual(
            tuple(item["path"] for item in captured["installed"]["executables"]),
            ("/usr/bin/prismdrake-session", "/usr/bin/prismdrake-settingsd",
             "/usr/bin/prismdrake-shell"),
        )

    def test_atomic_writer_publishes_mode_0600_complete_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "final.json"
            document = {"complete": True}
            atomic_write_json(output, document)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), document)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            atomic_write_json(output, {"complete": False})
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")),
                             {"complete": False})


if __name__ == "__main__":
    unittest.main()
