#!/usr/bin/env python3
"""Hermetic tests for the strict PD1 Portage lifecycle evidence contract."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

from portage_lifecycle_evidence import (
    EvidenceError,
    EXCLUDED_TESTS,
    INSTALLED_PATHS,
    MAX_EVIDENCE_BYTES,
    SCHEMA_PATH,
    example_evidence_document,
    load_evidence,
    validate_evidence_document,
    validate_evidence_schema,
)


class EvidenceContractTests(unittest.TestCase):
    def assert_rejected(self, document: object) -> None:
        with self.assertRaises(EvidenceError):
            validate_evidence_document(document)

    def test_accepts_complete_synthetic_contract_fixture(self) -> None:
        document = example_evidence_document()
        validate_evidence_schema(document)
        validate_evidence_document(document)

    def test_schema_closes_every_object_and_bounds_every_array(self) -> None:
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
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        self.assertEqual(schema["$defs"]["revision"]["pattern"], "^[0-9a-f]{40}$")
        self.assertEqual(schema["$defs"]["sha256"]["pattern"], "^[0-9a-f]{64}$")

    def test_serialized_fixture_is_bounded_and_redacted(self) -> None:
        encoded = json.dumps(example_evidence_document(), ensure_ascii=True, sort_keys=True)
        self.assertLessEqual(len(encoded.encode("utf-8")), MAX_EVIDENCE_BYTES)
        for forbidden in (
            "DBUS_SESSION_BUS_ADDRESS",
            "AT_SPI_BUS_ADDRESS",
            "unix:",
            '"pid"',
            '"xid"',
            "/home/",
            "/tmp/",
            "/mnt/",
        ):
            self.assertNotIn(forbidden, encoded)

    def test_rejects_each_missing_lifecycle_section(self) -> None:
        required = tuple(example_evidence_document())
        for field in required:
            with self.subTest(field=field):
                document = example_evidence_document()
                del document[field]
                self.assert_rejected(document)

    def test_rejects_partial_resolution_and_runtime_records(self) -> None:
        mutations = (
            lambda document: document["resolutions"].pop(),
            lambda document: document["installed"]["executables"].pop(),
            lambda document: document["runtime_validation"].pop("atspi"),
            lambda document: document["runtime_validation"].pop("complete_demo"),
            lambda document: document["runtime_validation"]["linkage"]["entries"].pop(),
            lambda document: document["sandbox_exclusions"].pop(),
            lambda document: document["unmerge"]["user_data"].pop(),
            lambda document: document["ordinary_reinstall"]["executables"].pop(),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_unknown_root_and_nested_fields(self) -> None:
        document = example_evidence_document()
        document["extra"] = True
        self.assert_rejected(document)

        document = example_evidence_document()
        document["installed"]["ownership"]["output"] = "redacted"
        self.assert_rejected(document)

    def test_rejects_private_or_unsafe_fields_and_values(self) -> None:
        mutations = (
            lambda document: document["runtime_validation"].update({"pid": 42}),
            lambda document: document["source"].update({"username": "builder"}),
            lambda document: document["runtime_validation"]["linkage"]["entries"][0][
                "needed"
            ].append("unix:path=/private"),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_invalid_or_non_exact_source_revisions(self) -> None:
        for revision in ("abc", "A" * 40, "0" * 39, "0" * 41):
            with self.subTest(revision=revision):
                document = example_evidence_document()
                document["source"]["revision"] = revision
                self.assert_rejected(document)

    def test_rejects_revision_mismatch_in_every_bound_phase(self) -> None:
        locations = (
            ("resolutions", 0),
            ("tested_artifact", None),
            ("package_test", None),
            ("installed", None),
            ("runtime_validation", None),
            ("sandbox_exclusions", 1),
            ("unmerge", None),
            ("ordinary_reinstall", None),
        )
        for section, index in locations:
            with self.subTest(section=section, index=index):
                document = example_evidence_document()
                record = document[section] if index is None else document[section][index]
                record["source_revision"] = "f" * 40
                self.assert_rejected(document)

    def test_rejects_ebuild_hash_mismatch(self) -> None:
        for section, index in (
            ("resolutions", 1),
            ("tested_artifact", None),
            ("package_test", None),
            ("ordinary_reinstall", None),
        ):
            with self.subTest(section=section):
                document = example_evidence_document()
                record = document[section] if index is None else document[section][index]
                record["ebuild_sha256"] = "9" * 64
                self.assert_rejected(document)

    def test_rejects_artifact_hash_mismatch_in_every_bound_phase(self) -> None:
        locations = (
            ("package_test", None),
            ("installed", None),
            ("runtime_validation", None),
            ("sandbox_exclusions", 2),
            ("unmerge", None),
            ("ordinary_reinstall", None),
        )
        for section, index in locations:
            with self.subTest(section=section, index=index):
                document = example_evidence_document()
                record = document[section] if index is None else document[section][index]
                record["artifact_sha256"] = "9" * 64
                self.assert_rejected(document)

    def test_rejects_invalid_hash_forms(self) -> None:
        for digest in ("f" * 63, "f" * 65, "F" * 64, "g" * 64):
            with self.subTest(digest=digest[:8]):
                document = example_evidence_document()
                document["source"]["ebuild_sha256"] = digest
                self.assert_rejected(document)

    def test_rejects_noncanonical_repository_or_ebuild_identity(self) -> None:
        mutations = (
            lambda document: document["source"].update({"atom": "x11-misc/prismdrake-9999"}),
            lambda document: document["source"].update({"version": "1"}),
            lambda document: document["source"].update({"repository_name": "local"}),
            lambda document: document["source"].update({"repository_uri": "https://example.invalid"}),
            lambda document: document["tested_artifact"].update({"atom": "x11-misc/prismdrake"}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_incomplete_or_mislabeled_pretend_outcomes(self) -> None:
        mutations = (
            lambda document: document["resolutions"].reverse(),
            lambda document: document["resolutions"][0].update({"use_test": True}),
            lambda document: document["resolutions"][1].update({"result": "failed"}),
            lambda document: document["resolutions"][1].update(
                {"dependency_graph_sha256": document["resolutions"][0]["dependency_graph_sha256"]}
            ),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_failed_or_partial_package_test(self) -> None:
        mutations = (
            lambda document: document["package_test"].update({"result": "failed"}),
            lambda document: document["package_test"].update({"features_test": False}),
            lambda document: document["package_test"].update({"executed_test_count": 0}),
            lambda document: document["package_test"]["excluded_tests"].reverse(),
            lambda document: document["package_test"]["excluded_tests"].pop(),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_build_tree_falsely_labeled_as_installed(self) -> None:
        mutations = (
            lambda document: document["tested_artifact"].update({"provenance": "build_tree"}),
            lambda document: document["installed"].update({"provenance": "build_tree"}),
            lambda document: document["runtime_validation"]["complete_demo"].update(
                {"artifact_provenance": "build_tree"}
            ),
            lambda document: document["ordinary_reinstall"].update(
                {"provenance": "build_tree"}
            ),
            lambda document: document["installed"]["executables"][0].update(
                {"path": "build/bin/prismdrake-session"}
            ),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_relative_private_or_wrong_installed_paths(self) -> None:
        for path in (
            "usr/bin/prismdrake-session",
            "/tmp/prismdrake-session",
            "/mnt/shared/prismdrake-session",
            "/usr/local/bin/prismdrake-session",
            "/usr/bin/other",
        ):
            with self.subTest(path=path):
                document = example_evidence_document()
                document["installed"]["executables"][0]["path"] = path
                self.assert_rejected(document)

    def test_rejects_missing_executable_or_ownership_identity(self) -> None:
        mutations = (
            lambda document: document["installed"]["executables"][0].update(
                {"regular_executable": False}
            ),
            lambda document: document["installed"]["executables"].reverse(),
            lambda document: document["installed"]["ownership"].update(
                {"owner_atom": "x11-misc/other-9999"}
            ),
            lambda document: document["installed"]["ownership"]["paths"].reverse(),
            lambda document: document["installed"]["ownership"].update({"owned_file_count": 1}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_incomplete_atspi_or_demo_results(self) -> None:
        mutations = (
            lambda document: document["runtime_validation"]["atspi"].update(
                {"result": "failed"}
            ),
            lambda document: document["runtime_validation"]["atspi"].update(
                {"complete": False}
            ),
            lambda document: document["runtime_validation"]["complete_demo"].update(
                {"result": "failed"}
            ),
            lambda document: document["runtime_validation"]["complete_demo"].update(
                {"complete": False}
            ),
            lambda document: document["runtime_validation"]["complete_demo"].pop(
                "driver_sha256"
            ),
            lambda document: document["runtime_validation"]["complete_demo"].update(
                {"driver_sha256": "short"}
            ),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_inconsistent_or_incomplete_runtime_linkage(self) -> None:
        mutations = (
            lambda document: document["runtime_validation"]["linkage"]["entries"][0].update(
                {"executable_sha256": "9" * 64}
            ),
            lambda document: document["runtime_validation"]["linkage"]["entries"].reverse(),
            lambda document: document["runtime_validation"]["linkage"]["entries"][1].update(
                {"needed": ["libQt6Core.so.6"]}
            ),
            lambda document: document["runtime_validation"]["linkage"]["entries"][0][
                "needed"
            ].reverse(),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_any_unapproved_or_failed_sandbox_exclusion(self) -> None:
        mutations = (
            lambda document: document["sandbox_exclusions"].reverse(),
            lambda document: document["sandbox_exclusions"][0].update(
                {"test_id": EXCLUDED_TESTS[1]}
            ),
            lambda document: document["sandbox_exclusions"][0].update(
                {"reason": "openbox_preload_interference"}
            ),
            lambda document: document["sandbox_exclusions"][1].update({"passes": 0}),
            lambda document: document["sandbox_exclusions"][2].update(
                {"attempts": 4, "passes": 4}
            ),
            lambda document: document["sandbox_exclusions"][2].update(
                {"attempts": 5, "passes": 4}
            ),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_unmerge_failure_or_user_data_change(self) -> None:
        mutations = (
            lambda document: document["unmerge"].update({"result": "failed"}),
            lambda document: document["unmerge"].update(
                {"installed_executables_absent": False}
            ),
            lambda document: document["unmerge"]["user_data"][0].update(
                {"after_sha256": "9" * 64}
            ),
            lambda document: document["unmerge"]["user_data"][0].update(
                {"byte_identical": False}
            ),
            lambda document: document["unmerge"]["user_data"].reverse(),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)

    def test_rejects_nonordinary_or_inconsistent_reinstall(self) -> None:
        mutations = (
            lambda document: document["ordinary_reinstall"].update({"use_test": True}),
            lambda document: document["ordinary_reinstall"].update({"result": "failed"}),
            lambda document: document["ordinary_reinstall"]["executables"][0].update(
                {"sha256": "9" * 64}
            ),
            lambda document: document["ordinary_reinstall"].update(
                {"ownership_atom": "x11-misc/other-9999"}
            ),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                document = example_evidence_document()
                mutation(document)
                self.assert_rejected(document)


class StrictLoaderTests(unittest.TestCase):
    def test_loads_bounded_valid_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            document = example_evidence_document()
            path.write_text(json.dumps(document), encoding="utf-8")
            self.assertEqual(load_evidence(path), document)

    def test_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaises(EvidenceError):
                load_evidence(path)

    def test_rejects_oversized_input_before_parsing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            path.write_bytes(b" " * (MAX_EVIDENCE_BYTES + 1))
            with self.assertRaisesRegex(EvidenceError, "evidence_oversized"):
                load_evidence(path)

    def test_rejects_nonfinite_json_number(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            path.write_text('{"value":NaN}', encoding="utf-8")
            with self.assertRaises(EvidenceError):
                load_evidence(path)


if __name__ == "__main__":
    unittest.main()
