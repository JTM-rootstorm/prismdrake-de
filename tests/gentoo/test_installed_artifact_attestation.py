#!/usr/bin/env python3
"""Hermetic tests for the installed-artifact preflight contract."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

from installed_artifact_attestation import (
    AttestationError,
    MAX_ATTESTATION_BYTES,
    example_attestation_document,
    load_attestation,
    validate_attestation_document,
    validate_attested_files,
)


class InstalledArtifactAttestationTests(unittest.TestCase):
    def test_accepts_complete_synthetic_attestation(self) -> None:
        validate_attestation_document(example_attestation_document())

    def test_rejects_noncanonical_artifact_hash(self) -> None:
        document = example_attestation_document()
        document["tested_artifact"]["artifact_sha256"] = "f" * 64
        document["installed"]["artifact_sha256"] = "f" * 64
        with self.assertRaisesRegex(AttestationError, "^artifact_sha256_mismatch$"):
            validate_attestation_document(document)

    def test_rejects_unknown_fields_and_private_paths(self) -> None:
        document = example_attestation_document()
        document["demo_driver"]["path"] = "/home/builder/pd1_demo.py"
        with self.assertRaises(AttestationError):
            validate_attestation_document(document)

    def test_binds_exact_executable_and_driver_hashes(self) -> None:
        document = example_attestation_document()
        paths = tuple(Path(record["path"]) for record in document["installed"]["executables"])
        hashes = {record["path"]: record["sha256"]
                  for record in document["installed"]["executables"]}
        hashes["/source/pd1_demo.py"] = document["demo_driver"]["sha256"]
        validate_attested_files(
            document, paths, Path("/source/pd1_demo.py"), lambda path: hashes[str(path)],
        )
        changed = copy.deepcopy(document)
        hashes["/usr/bin/prismdrake-shell"] = "0" * 64
        with self.assertRaisesRegex(AttestationError, "^installed_executable_hash_mismatch$"):
            validate_attested_files(
                changed, paths, Path("/source/pd1_demo.py"), lambda path: hashes[str(path)],
            )

    def test_loader_is_bounded_and_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "attestation.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(AttestationError, "^attestation_json_invalid$"):
                load_attestation(path)
            path.write_bytes(b" " * (MAX_ATTESTATION_BYTES + 1))
            with self.assertRaisesRegex(AttestationError, "^attestation_oversized$"):
                load_attestation(path)

    def test_fixture_is_bounded(self) -> None:
        encoded = json.dumps(example_attestation_document(), ensure_ascii=True, sort_keys=True)
        self.assertLessEqual(len(encoded.encode()), MAX_ATTESTATION_BYTES)


if __name__ == "__main__":
    unittest.main()
