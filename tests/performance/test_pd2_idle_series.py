#!/usr/bin/env python3
"""Contract tests for assembling repeated PD2 idle-wakeup samples."""

from __future__ import annotations

import copy
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import pd2_idle_series as idle_series
from validate_external_evidence import idle_fixture


REVISION = "a" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"


def trial(value: int) -> dict[str, object]:
    document = idle_fixture()
    document["source_revision"] = REVISION
    document["reference_environment_id"] = ENVIRONMENT_ID
    document["trial_id"] = f"{value:032x}"
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
        self.assertEqual(document["samples_wakeups_per_minute"], [8, 12, 16])
        self.assertNotIn("results", document)

    def test_rejects_duplicate_trial_identity_and_input_file(self) -> None:
        duplicate = trial(1)
        with self.assertRaisesRegex(
            idle_series.IdleSeriesError, "^idle_trial_duplicate$"
        ):
            idle_series.assemble_series(
                [duplicate, duplicate, trial(3)], REVISION, ENVIRONMENT_ID
            )

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trial.json"
            path.write_text("{}", encoding="ascii")
            options = type(
                "Options",
                (),
                {
                    "input": [path, path, path],
                    "source_revision": REVISION,
                    "environment_id": ENVIRONMENT_ID,
                    "output": Path(temporary) / "output.json",
                },
            )()
            with self.assertRaisesRegex(
                idle_series.IdleSeriesError, "^input_path_duplicate$"
            ):
                idle_series.run(options)

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
            with mock.patch.object(os, "fsync", wraps=os.fsync) as sync:
                idle_series.write_series(output, document)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertEqual(sync.call_count, 2)
            with self.assertRaisesRegex(
                idle_series.IdleSeriesError, "^output_path_invalid$"
            ):
                idle_series.write_series(output, document)

    def test_writer_preserves_output_created_during_publication(self) -> None:
        document = idle_series.assemble_series(
            [trial(1), trial(2), trial(3)], REVISION, ENVIRONMENT_ID
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "series.json"

            def retain_output(_source: object, destination: object, **_kwargs: object) -> None:
                Path(destination).write_bytes(b"retained")
                raise FileExistsError

            with (
                mock.patch.object(os, "link", side_effect=retain_output),
                self.assertRaisesRegex(
                    idle_series.IdleSeriesError, "^output_write_failed$"
                ),
            ):
                idle_series.write_series(output, document)
            self.assertEqual(output.read_bytes(), b"retained")

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
