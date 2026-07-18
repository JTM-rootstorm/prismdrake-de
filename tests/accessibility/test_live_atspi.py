#!/usr/bin/env python3
"""Negative and schema-source tests for the live AT-SPI evidence contract."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from live_atspi import (
    EvidenceError,
    example_evidence_document,
    validate_evidence_document,
    validate_evidence_schema,
)


class AccessibilityEvidenceContractTests(unittest.TestCase):
    def test_example_satisfies_semantic_contract(self) -> None:
        evidence = example_evidence_document()
        validate_evidence_schema(evidence)
        validate_evidence_document(evidence)

    def test_schema_source_is_strict_version_one_json(self) -> None:
        schema_path = Path(__file__).with_name("atspi-evidence.schema.json")
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        self.assertEqual(schema["properties"]["phases"]["minItems"], 7)
        self.assertEqual(schema["properties"]["phases"]["maxItems"], 7)
        self.assertFalse(schema["$defs"]["phase"]["additionalProperties"])
        self.assertFalse(schema["$defs"]["control"]["additionalProperties"])

    def test_rejects_missing_phase(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"].pop()
        with self.assertRaisesRegex(EvidenceError, "wrong phase count"):
            validate_evidence_document(evidence)

    def test_rejects_reordered_focus_evidence(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][1]["focused_control"] = "Prismdrake Evidence App"
        with self.assertRaisesRegex(EvidenceError, "unexpected focused control"):
            validate_evidence_document(evidence)

    def test_rejects_duplicate_control(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][1] = copy.deepcopy(
            evidence["phases"][0]["controls"][0]
        )
        with self.assertRaisesRegex(EvidenceError, "missing or reordered"):
            validate_evidence_document(evidence)

    def test_rejects_private_runtime_data(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][0]["description"] = "unix:path=/tmp/private"
        with self.assertRaisesRegex(EvidenceError, "private runtime data"):
            validate_evidence_document(evidence)

    def test_rejects_non_fixture_description(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][0]["description"] = "Unexpected user content"
        with self.assertRaisesRegex(EvidenceError, "outside the redacted fixture allow-list"):
            validate_evidence_document(evidence)

    def test_rejects_missing_button_action(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][0]["actions"] = []
        with self.assertRaisesRegex(EvidenceError, "button has no Press action"):
            validate_evidence_document(evidence)


if __name__ == "__main__":
    unittest.main()
