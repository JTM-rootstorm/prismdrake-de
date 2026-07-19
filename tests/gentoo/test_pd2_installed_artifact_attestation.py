#!/usr/bin/env python3
"""Hermetic negative tests for the PD2 installed preflight contract."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

from pd2_installed_artifact_attestation import (
    INSTALLED_EXECUTABLE_PATHS,
    MAX_ATTESTATION_BYTES,
    Pd2AttestationError,
    SCHEMA_PATH,
    example_attestation_document,
    load_attestation,
    validate_attestation_document,
    validate_attested_files,
)
from pd2_lifecycle_hashes import (
    Pd2LifecycleHashError,
    canonical_ownership_sha256,
    normalize_ownership_records,
)


class Pd2InstalledAttestationTests(unittest.TestCase):
    def assert_rejected(self, document: object) -> None:
        with self.assertRaises(Pd2AttestationError):
            validate_attestation_document(document)

    def test_accepts_complete_five_executable_full_ownership_fixture(self) -> None:
        document = example_attestation_document()
        validate_attestation_document(document)
        self.assertEqual(
            tuple(record["path"] for record in document["installed"]["executables"]),
            INSTALLED_EXECUTABLE_PATHS,
        )
        ownership = document["installed"]["ownership"]
        self.assertEqual(ownership["path_count"], len(ownership["paths"]))
        self.assertGreater(ownership["path_count"], len(INSTALLED_EXECUTABLE_PATHS))

    def test_schema_closes_objects_and_bounds_arrays(self) -> None:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

        def inspect(value: object) -> None:
            if isinstance(value, dict):
                if value.get("type") == "object":
                    self.assertIs(value.get("additionalProperties"), False)
                if value.get("type") == "array":
                    self.assertIn("maxItems", value)
                for child in value.values():
                    inspect(child)
            elif isinstance(value, list):
                for child in value:
                    inspect(child)

        inspect(schema)

    def test_rejects_missing_reordered_or_unowned_executable(self) -> None:
        mutations = (
            lambda document: document["installed"]["executables"].pop(),
            lambda document: document["installed"]["executables"].reverse(),
            lambda document: document["installed"]["ownership"]["paths"].pop(0),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_attestation_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_incomplete_or_noncanonical_ownership(self) -> None:
        mutations = (
            lambda value: value.update({"path_count": value["path_count"] - 1}),
            lambda value: value.update({"regular_path_count": 5}),
            lambda value: value.update({"path_set_sha256": "0" * 64}),
            lambda value: value["paths"].reverse(),
            lambda value: value["paths"].append(copy.deepcopy(value["paths"][0])),
            lambda value: value["paths"][0].update({"path": "/home/user/private"}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_attestation_document()
                mutation(document["installed"]["ownership"])
                self.assert_rejected(document)

    def test_rejects_revision_ebuild_and_artifact_mismatch(self) -> None:
        mutations = (
            lambda document: document["installed"].update({"source_revision": "f" * 40}),
            lambda document: document["tested_artifact"].update({"ebuild_sha256": "9" * 64}),
            lambda document: document["installed"].update({"artifact_sha256": "8" * 64}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_attestation_document()
                mutation(document)
                self.assert_rejected(document)

    def test_binds_exact_files_without_recording_driver_path(self) -> None:
        document = example_attestation_document()
        paths = tuple(Path(path) for path in INSTALLED_EXECUTABLE_PATHS)
        hashes = {record["path"]: record["sha256"]
                  for record in document["installed"]["executables"]}
        hashes["/source/pd2_demo.py"] = document["demo_driver"]["sha256"]
        validate_attested_files(
            document,
            paths,
            Path("/source/pd2_demo.py"),
            lambda path: hashes[str(path)],
        )
        hashes[INSTALLED_EXECUTABLE_PATHS[2]] = "0" * 64
        with self.assertRaisesRegex(Pd2AttestationError,
                                    "^installed_executable_hash_mismatch$"):
            validate_attested_files(
                document,
                paths,
                Path("/source/pd2_demo.py"),
                lambda path: hashes[str(path)],
            )

    def test_hashes_reject_private_duplicate_and_unknown_kinds(self) -> None:
        for records in (
            [{"kind": "regular", "path": "/tmp/private"}],
            [
                {"kind": "regular", "path": "/usr/bin/a"},
                {"kind": "regular", "path": "/usr/bin/a"},
            ],
            [{"kind": "device", "path": "/usr/bin/a"}],
        ):
            with self.subTest(records=records):
                with self.assertRaises(Pd2LifecycleHashError):
                    canonical_ownership_sha256(records)
        normalized = normalize_ownership_records([
            {"kind": "regular", "path": "/usr/share/z"},
            {"kind": "directory", "path": "/usr/share/a"},
        ])
        self.assertEqual([record["path"] for record in normalized],
                         ["/usr/share/a", "/usr/share/z"])

    def test_loader_is_strict_bounded_and_private_fixture_is_absent(self) -> None:
        encoded = json.dumps(example_attestation_document(), ensure_ascii=True, sort_keys=True)
        self.assertLessEqual(len(encoded.encode("utf-8")), MAX_ATTESTATION_BYTES)
        for marker in ("/home/", "/tmp/", "/mnt/", '"pid"', '"command"'):
            self.assertNotIn(marker, encoded.lower())

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "attestation.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(Pd2AttestationError, "^attestation_json_invalid$"):
                load_attestation(path)
            path.write_bytes(b" " * (MAX_ATTESTATION_BYTES + 1))
            with self.assertRaisesRegex(Pd2AttestationError, "^attestation_size_invalid$"):
                load_attestation(path)


if __name__ == "__main__":
    unittest.main()
