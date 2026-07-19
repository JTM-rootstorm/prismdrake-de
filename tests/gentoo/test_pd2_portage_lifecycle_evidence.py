#!/usr/bin/env python3
"""Hermetic negative tests for the PD2 installed lifecycle contract."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

from pd2_installed_artifact_attestation import INSTALLED_EXECUTABLE_PATHS
from pd2_portage_lifecycle_evidence import (
    DEMONSTRATION_PHASES,
    EXCLUDED_TESTS,
    MAX_EVIDENCE_BYTES,
    Pd2EvidenceError,
    SCHEMA_PATH,
    USER_DATA_KINDS,
    example_evidence_document,
    load_evidence,
    validate_evidence_document,
)


class Pd2LifecycleEvidenceTests(unittest.TestCase):
    def assert_rejected(self, document: object) -> None:
        with self.assertRaises(Pd2EvidenceError):
            validate_evidence_document(document)

    def test_accepts_complete_synthetic_lifecycle(self) -> None:
        document = example_evidence_document()
        validate_evidence_document(document)
        self.assertEqual(tuple(item["path"] for item in
                               document["initial_install"]["executables"]),
                         INSTALLED_EXECUTABLE_PATHS)
        self.assertEqual(tuple(item["phase"] for item in
                               document["installed_demonstrations"]),
                         DEMONSTRATION_PHASES)

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

    def test_rejects_missing_sections_and_unknown_fields(self) -> None:
        for field in example_evidence_document():
            with self.subTest(field=field):
                document = example_evidence_document()
                del document[field]
                self.assert_rejected(document)
        document = example_evidence_document()
        document["private_path"] = "/home/user/value"
        self.assert_rejected(document)

    def test_rejects_mismatched_revision_ebuild_and_artifact_bindings(self) -> None:
        locations = (
            ("resolutions", 0, "source_revision", "f" * 40),
            ("package_test", None, "ebuild_sha256", "9" * 64),
            ("initial_install", None, "artifact_sha256", "8" * 64),
            ("direct_linkage", None, "source_revision", "f" * 40),
            ("sandbox_exclusions", 2, "ebuild_sha256", "9" * 64),
            ("installed_demonstrations", 0, "artifact_sha256", "8" * 64),
            ("unmerge", None, "source_revision", "f" * 40),
            ("ordinary_reinstall", None, "ebuild_sha256", "9" * 64),
        )
        for section, index, key, value in locations:
            with self.subTest(section=section, key=key):
                document = example_evidence_document()
                record = document[section] if index is None else document[section][index]
                record[key] = value
                self.assert_rejected(document)

    def test_rejects_resolution_and_package_test_drift(self) -> None:
        mutations = (
            lambda value: value["resolutions"].reverse(),
            lambda value: value["resolutions"][0].update({"use_test": True}),
            lambda value: value["resolutions"][1].update({
                "dependency_graph_sha256": value["resolutions"][0]["dependency_graph_sha256"]}),
            lambda value: value["package_test"]["excluded_tests"].reverse(),
            lambda value: value["package_test"].update({"executed_test_count": 0}),
            lambda value: value["package_test"]["skipped_tests"][0].update(
                {"test_id": "ContractTest.Ordinary"}),
            lambda value: value["package_test"].update({"skipped_test_count": 0}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_incomplete_five_executable_or_full_ownership_records(self) -> None:
        mutations = (
            lambda value: value["initial_install"]["executables"].pop(),
            lambda value: value["initial_install"]["executables"].reverse(),
            lambda value: value["initial_install"]["ownership"].update({"path_count": 5}),
            lambda value: value["initial_install"]["ownership"]["paths"].reverse(),
            lambda value: value["initial_install"]["ownership"]["paths"][0].update(
                {"path": "/tmp/private"}),
            lambda value: value["initial_install"]["ownership"]["paths"].append(
                copy.deepcopy(value["initial_install"]["ownership"]["paths"][0])),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_linkage_drift_or_noncanonical_capture(self) -> None:
        mutations = (
            lambda value: value["direct_linkage"]["entries"].pop(),
            lambda value: value["direct_linkage"]["entries"].reverse(),
            lambda value: value["direct_linkage"]["entries"][0].update(
                {"executable_sha256": "0" * 64}),
            lambda value: value["direct_linkage"]["entries"][1]["needed"].reverse(),
            lambda value: value["direct_linkage"].update({"capture_sha256": "0" * 64}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_unjustified_or_nonrepeating_exclusions(self) -> None:
        mutations = (
            lambda value: value["sandbox_exclusions"].pop(),
            lambda value: value["sandbox_exclusions"][0].update(
                {"test_id": EXCLUDED_TESTS[1]}),
            lambda value: value["sandbox_exclusions"][2].update({"attempts": 4, "passes": 4}),
            lambda value: value["sandbox_exclusions"][3].update({"passes": 4}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_partial_or_unbound_installed_demonstrations(self) -> None:
        mutations = (
            lambda value: value["installed_demonstrations"].pop(),
            lambda value: value["installed_demonstrations"].reverse(),
            lambda value: value["installed_demonstrations"][0].update({"result": "failed"}),
            lambda value: value["installed_demonstrations"][1].update(
                {"harness_manifest_sha256": "0" * 64}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_unmerge_or_user_data_preservation_drift(self) -> None:
        mutations = (
            lambda value: value["unmerge"].update({"owned_removable_paths_absent": False}),
            lambda value: value["unmerge"].update({"absent_path_count": 5}),
            lambda value: value["unmerge"].update({"removable_path_set_sha256": "0" * 64}),
            lambda value: value["unmerge"]["user_data"].pop(),
            lambda value: value["unmerge"]["user_data"].reverse(),
            lambda value: value["unmerge"]["user_data"][0].update({"after_sha256": "0" * 64}),
            lambda value: value["unmerge"]["user_data"][1].update({"path": "/home/user/pins"}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)
        self.assertEqual(tuple(item["kind"] for item in
                               example_evidence_document()["unmerge"]["user_data"]),
                         USER_DATA_KINDS)

    def test_rejects_nonidentical_ordinary_reinstall(self) -> None:
        mutations = (
            lambda value: value["ordinary_reinstall"]["executables"][0].update(
                {"sha256": "0" * 64}),
            lambda value: value["ordinary_reinstall"]["executables"].reverse(),
            lambda value: value["ordinary_reinstall"]["ownership"].update(
                {"path_set_sha256": "0" * 64}),
            lambda value: value["ordinary_reinstall"]["ownership"]["paths"].pop(),
            lambda value: value["ordinary_reinstall"].update({"use_test": True}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_loader_is_strict_bounded_and_fixture_is_private_data_free(self) -> None:
        encoded = json.dumps(example_evidence_document(), ensure_ascii=True, sort_keys=True)
        self.assertLessEqual(len(encoded.encode("utf-8")), MAX_EVIDENCE_BYTES)
        for marker in ("/home/", "/tmp/", "/mnt/", '"pid"', '"command"'):
            self.assertNotIn(marker, encoded.lower())
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            for payload in ('{"schema_version":1,"schema_version":1}', '{"value":NaN}'):
                path.write_text(payload, encoding="utf-8")
                with self.assertRaisesRegex(Pd2EvidenceError, "^evidence_json_invalid$"):
                    load_evidence(path)
            path.write_bytes(b" " * (MAX_EVIDENCE_BYTES + 1))
            with self.assertRaisesRegex(Pd2EvidenceError, "^evidence_size_invalid$"):
                load_evidence(path)


if __name__ == "__main__":
    unittest.main()
