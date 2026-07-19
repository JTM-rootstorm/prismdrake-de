#!/usr/bin/env python3
"""Contract tests for assembling repeated PD2 idle-wakeup samples."""

from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path

import pd2_idle_series as idle_series
from validate_external_evidence import idle_fixture


REVISION = "a" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"


def trial(value: int) -> dict[str, object]:
    document = idle_fixture()
    document["source_revision"] = REVISION
    document["reference_environment_id"] = ENVIRONMENT_ID
    for record in document["results"]:
        record["sched_wakeup_count"] = value
        record["sched_wakeup_new_count"] = 1
        record["received_wakeup_count"] = value + 1
    return document


class IdleSeriesTests(unittest.TestCase):
    def test_assembles_only_redacted_role_totals(self) -> None:
        document = idle_series.assemble_series(
            [trial(1), trial(2), trial(3)], REVISION, ENVIRONMENT_ID
        )
        idle_series.validate_series(document, REVISION, ENVIRONMENT_ID)
        self.assertEqual(document["samples_wakeups_per_minute"], [6, 9, 12])
        self.assertNotIn("results", document)

    def test_rejects_wrong_revision_and_invalid_semantics(self) -> None:
        wrong_revision = trial(1)
        wrong_revision["source_revision"] = "b" * 40
        with self.assertRaisesRegex(
            idle_series.IdleSeriesError, "^idle_trial_revision_mismatch$"
        ):
            idle_series.assemble_series(
                [wrong_revision, trial(2), trial(3)], REVISION, ENVIRONMENT_ID
            )

        invalid = copy.deepcopy(trial(1))
        invalid["results"][0]["received_wakeup_count"] += 1
        with self.assertRaisesRegex(
            idle_series.IdleSeriesError, "^idle_trial_semantics_invalid$"
        ):
            idle_series.assemble_series(
                [invalid, trial(2), trial(3)], REVISION, ENVIRONMENT_ID
            )

    def test_writes_private_artifact_without_overwrite(self) -> None:
        document = idle_series.assemble_series(
            [trial(1), trial(2), trial(3)], REVISION, ENVIRONMENT_ID
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "series.json"
            idle_series.write_series(output, document)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            with self.assertRaisesRegex(
                idle_series.IdleSeriesError, "^output_path_invalid$"
            ):
                idle_series.write_series(output, document)

    def test_rejects_boolean_sample_as_noninteger_evidence(self) -> None:
        document = idle_series.assemble_series(
            [trial(1), trial(2), trial(3)], REVISION, ENVIRONMENT_ID
        )
        document["samples_wakeups_per_minute"][0] = True
        with self.assertRaisesRegex(
            idle_series.IdleSeriesError, "^idle_series_samples_invalid$"
        ):
            idle_series.validate_series(document, REVISION, ENVIRONMENT_ID)


if __name__ == "__main__":
    unittest.main()
