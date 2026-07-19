#!/usr/bin/env python3
"""Negative contract tests for PD2 asset-performance evidence."""

from __future__ import annotations

import copy
import unittest

from validate_pd2_asset_performance import AssetEvidenceError, validate_document


REVISION = "a" * 40
ENVIRONMENT = "gentoo-reference-vm"


def document() -> dict[str, object]:
    return {
        "schema_version": 1,
        "evidence_kind": "pd2_asset_performance",
        "source_revision": REVISION,
        "reference_environment_id": ENVIRONMENT,
        "method": {
            "clock": "std_chrono_steady_clock",
            "completion": "production_async_callback_on_context_thread",
            "icon_fixture": "warm_file_cache_controlled_local_png_128",
            "measured_iterations": 3,
            "wallpaper_fixture": "warm_file_cache_controlled_local_png_1600x900",
        },
        "measurements": [
            {"name": name, "samples_ns": [1, 2, 3]}
            for name in (
                "icon_decode",
                "icon_cache_hit",
                "wallpaper_decode_publication",
            )
        ],
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "diagnostics_are_closed_ids": True,
        },
    }


class AssetPerformanceTests(unittest.TestCase):
    def test_accepts_exact_ordered_asset_measurements(self) -> None:
        validate_document(document(), REVISION, ENVIRONMENT)

    def test_rejects_reordered_or_noninteger_samples(self) -> None:
        reordered = document()
        reordered["measurements"] = list(reversed(reordered["measurements"]))
        with self.assertRaisesRegex(
            AssetEvidenceError, "^asset_evidence_measurements_invalid$"
        ):
            validate_document(reordered, REVISION, ENVIRONMENT)

        noninteger = copy.deepcopy(document())
        noninteger["measurements"][0]["samples_ns"][0] = 1.5
        with self.assertRaisesRegex(
            AssetEvidenceError, "^asset_evidence_measurements_invalid$"
        ):
            validate_document(noninteger, REVISION, ENVIRONMENT)


if __name__ == "__main__":
    unittest.main()
