#!/usr/bin/env python3
"""Positive and negative tests for validation-record revalidation."""

from __future__ import annotations

import copy
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "validate_evidence_record.py"
SCHEMA_PATH = ROOT / "schemas" / "prismdrake-validation-record.schema.json"
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "validation-record-v1.json"
SPEC = importlib.util.spec_from_file_location("prismdrake_validate_evidence_record", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
validator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validator
SPEC.loader.exec_module(validator)

VALIDATION_TIME = datetime(2026, 8, 23, 12, 0, 0, tzinfo=timezone.utc)
FIXTURE_ANCHOR = validator.AnchorExpectation(
    type="append-only-custody",
    root_sha256="838a51242559e08d7abd8634eca72ae3276ff25304cd579108d1255536e27c2a",
    authority="synthetic-fixture-custodian",
    location="custody:synthetic-public-fixture-v1",
    created_at="2026-08-23T00:00:00Z",
    verification_procedure="verify-synthetic-custody-assertion",
)


def valid_document() -> dict[str, object]:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def reanchor(document: dict[str, object]) -> validator.AnchorExpectation:
    anchor = document["integrity_anchor"]
    assert isinstance(anchor, dict)
    anchor["root_sha256"] = validator.canonical_payload_sha256(document)
    return validator.AnchorExpectation(
        type=anchor["type"],
        root_sha256=anchor["root_sha256"],
        authority=anchor["authority"],
        location=anchor["location"],
        created_at=anchor["created_at"],
        verification_procedure=anchor["verification_procedure"],
    )


def cli_arguments(root_sha256: str = FIXTURE_ANCHOR.root_sha256) -> list[str]:
    return [
        str(FIXTURE.relative_to(ROOT)),
        "--anchor-type",
        FIXTURE_ANCHOR.type,
        "--anchor-root-sha256",
        root_sha256,
        "--anchor-authority",
        FIXTURE_ANCHOR.authority,
        "--anchor-location",
        FIXTURE_ANCHOR.location,
        "--anchor-created-at",
        FIXTURE_ANCHOR.created_at,
        "--anchor-verification-procedure",
        FIXTURE_ANCHOR.verification_procedure,
    ]


class EvidenceRecordTests(unittest.TestCase):
    def assert_rejected(
        self,
        document: object,
        reason: str,
        expected_anchor: object = FIXTURE_ANCHOR,
    ) -> None:
        with self.assertRaisesRegex(validator.ValidationError, f"^{reason}$"):
            validator.validate_document(
                document,
                expected_anchor,
                validation_time=VALIDATION_TIME,
            )

    def test_accepts_strict_synthetic_fixture(self) -> None:
        validator.validate_document(
            valid_document(),
            FIXTURE_ANCHOR,
            validation_time=VALIDATION_TIME,
        )

    def test_cli_revalidates_saved_fixture_without_recollection(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(validator.main(cli_arguments()), 0)
        self.assertEqual(output.getvalue(), "Prismdrake validation record accepted\n")

    def test_cli_fails_closed_without_or_with_mismatched_external_anchor(self) -> None:
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as missing:
                validator.main([str(FIXTURE.relative_to(ROOT))])
        self.assertEqual(missing.exception.code, 2)

        output = io.StringIO()
        with redirect_stderr(output):
            self.assertEqual(validator.main(cli_arguments("f" * 64)), 1)
        self.assertEqual(output.getvalue(), "Prismdrake validation record rejected\n")

    def test_anchor_binds_canonical_payload_and_external_assertion(self) -> None:
        changed_payload = valid_document()
        changed_payload["result"]["status"] = "fail"  # type: ignore[index]
        self.assert_rejected(
            changed_payload,
            "integrity_anchor_payload_mismatch",
        )

        changed_anchor = valid_document()
        changed_anchor["integrity_anchor"]["authority"] = "different-custodian"  # type: ignore[index]
        self.assert_rejected(
            changed_anchor,
            "integrity_anchor_external_mismatch",
        )

        self.assert_rejected(
            valid_document(),
            "integrity_anchor_expectation_required",
            expected_anchor=None,
        )

    def test_rejects_raw_argv_private_paths_and_secret_assignments(self) -> None:
        raw_argv = valid_document()
        raw_argv["selection"]["command"] = ["python3", "-m", "unittest"]  # type: ignore[index]
        self.assert_rejected(raw_argv, "command_invalid")

        private_path = valid_document()
        private_path["selection"]["command"] = "/home/alice/private-test"  # type: ignore[index]
        self.assert_rejected(private_path, "private_path_detected")

        secret_assignment = valid_document()
        secret_assignment["selection"]["command"] = "--token=not-safe-to-store"  # type: ignore[index]
        self.assert_rejected(secret_assignment, "secret_pattern_detected")

    def test_rejects_unknown_forbidden_fields_and_secret_values(self) -> None:
        unknown = valid_document()
        unknown["unexpected"] = True
        self.assert_rejected(unknown, "record_shape_invalid")

        forbidden = valid_document()
        forbidden["environment"]["credential_value"] = "redacted"  # type: ignore[index]
        self.assert_rejected(forbidden, "forbidden_field_present")

        for secret in (
            "Bearer abcdefghijklmnopqrstuvwxyz",
            "https://alice:password@example.invalid",
            "eyJabcdefghijk.abcdefghijkl.abcdefghijkl",
        ):
            with self.subTest(secret=secret):
                document = valid_document()
                document["environment"]["identity"] = secret  # type: ignore[index]
                self.assert_rejected(document, "secret_pattern_detected")

    def test_rejects_absolute_parent_or_dot_log_paths(self) -> None:
        for path in (
            "/private/log.txt",
            "evidence/../private/log.txt",
            "evidence/./log.txt",
            ".hidden/log.txt",
        ):
            with self.subTest(path=path):
                document = valid_document()
                document["result"]["log"] = {  # type: ignore[index]
                    "relative_path": path,
                    "sha256": "7" * 64,
                }
                expected = (
                    "private_path_detected" if path.startswith("/") else "log_path_invalid"
                )
                self.assert_rejected(document, expected)

    def test_missing_log_is_recorded_data_and_is_not_dereferenced(self) -> None:
        document = valid_document()
        document["result"]["log"] = {  # type: ignore[index]
            "relative_path": "evidence/intentionally-missing.log",
            "sha256": "7" * 64,
        }
        expected = reanchor(document)
        validator.validate_document(
            document,
            expected,
            validation_time=VALIDATION_TIME,
        )

    def test_rejects_non_distinct_role_names_or_digests(self) -> None:
        duplicate_name = valid_document()
        duplicate_name["identities"]["validator"]["name"] = duplicate_name["identities"][  # type: ignore[index]
            "collector"
        ]["name"]
        self.assert_rejected(duplicate_name, "role_identity_names_not_distinct")

        duplicate_digest = valid_document()
        duplicate_digest["identities"]["validator"]["sha256"] = duplicate_digest["identities"][  # type: ignore[index]
            "collector"
        ]["sha256"]
        self.assert_rejected(duplicate_digest, "role_identity_digests_not_distinct")

    def test_rejects_anchor_under_writer_authority_or_wrong_location_kind(self) -> None:
        same_authority = valid_document()
        same_authority["integrity_anchor"]["authority"] = same_authority["custody"][  # type: ignore[index]
            "writer_authority"
        ]
        self.assert_rejected(same_authority, "integrity_anchor_authority_not_separate")

        wrong_kind = valid_document()
        wrong_kind["integrity_anchor"]["location"] = "signature:synthetic"  # type: ignore[index]
        self.assert_rejected(wrong_kind, "integrity_anchor_location_invalid")

    def test_rejects_ambiguous_or_inconsistent_retention(self) -> None:
        neither = valid_document()
        neither["custody"]["durable_retention_justification"] = None  # type: ignore[index]
        self.assert_rejected(neither, "retention_lifetime_invalid")

        both = valid_document()
        both["custody"]["expires_at"] = "2026-08-24T00:00:00Z"  # type: ignore[index]
        self.assert_rejected(both, "retention_lifetime_invalid")

        wrong_class = valid_document()
        wrong_class["custody"]["retention_class"] = "project-private-durable"  # type: ignore[index]
        wrong_class["custody"]["authorized_location"] = "project-local-private-evidence"  # type: ignore[index]
        self.assert_rejected(wrong_class, "public_fixture_custody_invalid")

    def test_rejects_expired_or_overlong_private_retention(self) -> None:
        expired = valid_document()
        custody = expired["custody"]  # type: ignore[assignment]
        custody.update(
            {
                "retention_class": "project-private-ephemeral",
                "authorized_location": "project-local-private-evidence",
                "authorized_audience": ["prismdrake-maintainers"],
                "disclosure_classification": "project-private",
                "mode": "0600",
                "expires_at": "2026-08-23T06:00:00Z",
                "durable_retention_justification": None,
                "final_disposition": "delete",
            }
        )
        self.assert_rejected(expired, "record_retention_expired")

        overlong = copy.deepcopy(expired)
        overlong["custody"]["expires_at"] = "2026-09-23T00:00:01Z"  # type: ignore[index]
        self.assert_rejected(overlong, "retention_lifetime_exceeded")

    def test_accepts_bounded_private_ephemeral_custody(self) -> None:
        document = valid_document()
        document["custody"].update(  # type: ignore[union-attr]
            {
                "retention_class": "project-private-ephemeral",
                "authorized_location": "project-local-private-evidence",
                "authorized_audience": ["prismdrake-maintainers"],
                "disclosure_classification": "project-private",
                "mode": "0600",
                "expires_at": "2026-09-01T00:00:00Z",
                "durable_retention_justification": None,
                "final_disposition": "delete",
            }
        )
        expected = reanchor(document)
        validator.validate_document(
            document,
            expected,
            validation_time=VALIDATION_TIME,
        )

    def test_rejects_noncanonical_or_future_anchor_timestamp(self) -> None:
        offset = valid_document()
        offset["integrity_anchor"]["created_at"] = "2026-08-23T00:00:00+00:00"  # type: ignore[index]
        self.assert_rejected(offset, "integrity_anchor_created_at_invalid")

        future = valid_document()
        future["integrity_anchor"]["created_at"] = "2026-08-23T12:06:00Z"  # type: ignore[index]
        self.assert_rejected(future, "integrity_anchor_created_in_future")

    def test_schema_and_manual_validator_contracts_are_aligned(self) -> None:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        definitions = schema["$defs"]
        self.assertEqual(definitions["selection"]["properties"]["command"]["$ref"], "#/$defs/command_id")
        self.assertEqual(
            set(definitions["custody"]["properties"]["retention_class"]["enum"]),
            set(validator.RETENTION_CLASSES),
        )
        self.assertEqual(
            set(definitions["custody"]["properties"]["authorized_location"]["enum"]),
            {value[0] for value in validator.RETENTION_LOCATIONS.values()},
        )
        self.assertEqual(definitions["timestamp"]["minLength"], 20)
        self.assertEqual(definitions["timestamp"]["maxLength"], 20)
        self.assertEqual(definitions["integrity_anchor"]["properties"]["algorithm"]["const"], "sha256")
        self.assertIn("canonical SHA-256", definitions["integrity_anchor"]["$comment"])

    def test_file_loader_rejects_symlink_hard_link_and_oversize(self) -> None:
        with tempfile.TemporaryDirectory(prefix="build-evidence-record-test-", dir=ROOT) as temporary:
            directory = Path(temporary)
            symlink = directory / "symlink.json"
            symlink.symlink_to(FIXTURE)
            with self.assertRaisesRegex(validator.ValidationError, "^record_path_unsafe$"):
                validator.load_record(symlink)

            source = directory / "source.json"
            hard_link = directory / "hard-link.json"
            source.write_text("{}", encoding="utf-8")
            os.link(source, hard_link)
            with self.assertRaisesRegex(validator.ValidationError, "^record_hard_link_rejected$"):
                validator.load_record(hard_link)

            oversized = directory / "oversized.json"
            oversized.write_bytes(b" " * (validator.MAX_RECORD_BYTES + 1))
            with self.assertRaisesRegex(validator.ValidationError, "^record_too_large$"):
                validator.load_record(oversized)

    def test_file_loader_rejects_duplicate_keys_and_nonfinite_numbers(self) -> None:
        with tempfile.TemporaryDirectory(prefix="build-evidence-record-test-", dir=ROOT) as temporary:
            directory = Path(temporary)
            duplicate = directory / "duplicate.json"
            duplicate.write_text('{"schema_version": 1, "schema_version": 1}', encoding="utf-8")
            with self.assertRaisesRegex(validator.ValidationError, "^record_duplicate_key$"):
                validator.load_record(duplicate)

            nonfinite = directory / "nonfinite.json"
            nonfinite.write_text('{"value": NaN}', encoding="utf-8")
            with self.assertRaisesRegex(validator.ValidationError, "^record_number_invalid$"):
                validator.load_record(nonfinite)

    def test_file_loader_rejects_group_or_world_writable_record(self) -> None:
        with tempfile.TemporaryDirectory(prefix="build-evidence-record-test-", dir=ROOT) as temporary:
            path = Path(temporary) / "writable.json"
            path.write_text(json.dumps(valid_document()), encoding="utf-8")
            path.chmod(0o666)
            with self.assertRaisesRegex(
                validator.ValidationError,
                "^record_file_permissions_unsafe$",
            ):
                validator.load_record(path)

    def test_file_loader_detects_same_descriptor_mutation(self) -> None:
        before = FIXTURE.stat()
        values = list(before)
        values[8] = values[8] + 1
        after = os.stat_result(values)
        with mock.patch.object(validator.os, "fstat", side_effect=(before, after)):
            with self.assertRaisesRegex(
                validator.ValidationError,
                "^record_changed_during_read$",
            ):
                validator.load_record(FIXTURE)

    def test_actual_file_mode_must_match_anchored_custody(self) -> None:
        with tempfile.TemporaryDirectory(prefix="build-evidence-record-test-", dir=ROOT) as temporary:
            path = Path(temporary) / "mode-mismatch.json"
            path.write_text(json.dumps(valid_document()), encoding="utf-8")
            path.chmod(0o600)
            loaded = validator.load_record(path)
            validator.validate_document(
                loaded.document,
                FIXTURE_ANCHOR,
                validation_time=VALIDATION_TIME,
            )
            with self.assertRaisesRegex(
                validator.ValidationError,
                "^record_file_mode_mismatch$",
            ):
                validator.validate_file_custody(loaded)

    def test_actual_file_location_must_match_anchored_custody(self) -> None:
        with tempfile.TemporaryDirectory(prefix="build-evidence-record-test-", dir=ROOT) as temporary:
            path = Path(temporary) / "wrong-location.json"
            path.write_text(json.dumps(valid_document()), encoding="utf-8")
            path.chmod(0o644)
            loaded = validator.load_record(path)
            validator.validate_document(
                loaded.document,
                FIXTURE_ANCHOR,
                validation_time=VALIDATION_TIME,
            )
            with self.assertRaisesRegex(
                validator.ValidationError,
                "^record_authorized_location_mismatch$",
            ):
                validator.validate_file_custody(loaded)

    def test_rejects_record_input_outside_project_without_opening_it(self) -> None:
        with self.assertRaisesRegex(
            validator.ValidationError,
            "^record_path_outside_project$",
        ):
            validator.load_record(Path("/tmp/not-a-prismdrake-record.json"))


if __name__ == "__main__":
    unittest.main()
