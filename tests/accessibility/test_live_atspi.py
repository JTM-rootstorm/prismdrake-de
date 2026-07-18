#!/usr/bin/env python3
"""Negative and schema-source tests for the live AT-SPI evidence contract."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import live_atspi
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

    def test_parses_bounded_xdotool_window_geometry(self) -> None:
        completed = SimpleNamespace(
            returncode=0,
            stdout="WINDOW=17\nX=90\nY=80\nWIDTH=300\nHEIGHT=240\nSCREEN=0\n",
        )
        with mock.patch.object(live_atspi, "_run_checked", return_value=completed):
            self.assertEqual(
                live_atspi._window_geometry(Path("/usr/bin/xdotool"), "17"),
                (90, 80, 300, 240),
            )

    def test_selects_unique_shell_window_containing_accessible_center(self) -> None:
        search = SimpleNamespace(returncode=0, stdout="11\n17\n")
        first = SimpleNamespace(
            returncode=0,
            stdout="WINDOW=11\nX=0\nY=0\nWIDTH=50\nHEIGHT=50\nSCREEN=0\n",
        )
        second = SimpleNamespace(
            returncode=0,
            stdout="WINDOW=17\nX=90\nY=80\nWIDTH=300\nHEIGHT=240\nSCREEN=0\n",
        )
        node = mock.Mock()
        node.get_process_id.return_value = 42
        node.get_component_iface.return_value.get_extents.return_value = SimpleNamespace(
            x=100, y=100, width=20, height=20
        )
        atspi = SimpleNamespace(CoordType=SimpleNamespace(SCREEN=1))
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[search, first, second]
        ):
            self.assertEqual(
                live_atspi._window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node
                ),
                "17",
            )


if __name__ == "__main__":
    unittest.main()
