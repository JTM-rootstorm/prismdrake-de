#!/usr/bin/env python3
"""Hermetic seam tests for the non-executing PD2 lifecycle collector."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import stat
import tempfile
import unittest

from pd2_installed_artifact_attestation import (
    INSTALLED_EXECUTABLE_PATHS,
    REQUIRED_INSTALLED_FILE_GROUPS,
    example_attestation_document,
)
from pd2_harness_manifest import HARNESS_RELATIVE_PATHS
from pd2_portage_lifecycle_collector import (
    BUILD_PROVENANCE_PATH,
    Pd2CollectorError,
    atomic_write_json,
    capture_preflight_attestation,
    capture_unmerge_absence,
    capture_user_state_preservation,
    finalize_lifecycle,
    load_capture_json,
    sha256_regular_file,
    verify_exact_source,
)
from pd2_portage_lifecycle_evidence import (
    USER_DATA_KINDS,
    example_evidence_document,
)


class Pd2LifecycleCollectorTests(unittest.TestCase):
    def test_captures_five_executables_and_complete_owned_path_set(self) -> None:
        fixture = example_attestation_document()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkout = root / "source"
            checkout.mkdir()
            bundle = root / "source.bundle"
            bundle.touch()
            controlled_fixture = root / "fixture"
            hashes = {item["path"]: item["sha256"]
                      for item in fixture["installed"]["executables"]}
            hashes.update({str(checkout / relative): "7" * 64
                           for relative in HARNESS_RELATIVE_PATHS})
            hashes[str(checkout / "packaging/gentoo/repository/x11-misc/prismdrake/prismdrake-9999.ebuild")] = "a" * 64
            hashes[str(bundle)] = "9" * 64
            hashes[str(controlled_fixture)] = "8" * 64
            hashes[str(BUILD_PROVENANCE_PATH)] = "6" * 64
            revision = fixture["source"]["revision"]

            def runner(command: list[str], cwd: Path | None) -> str:
                del cwd
                if "rev-parse" in command:
                    return revision + "\n"
                if "status" in command or "verify" in command:
                    return ""
                return f"{revision} HEAD\n"

            document = capture_preflight_attestation(
                checkout, bundle,
                copy.deepcopy(fixture["installed"]["ownership"]["paths"]),
                controlled_fixture,
                hash_file=lambda path: hashes[str(path)],
                is_executable=lambda path: str(path) in INSTALLED_EXECUTABLE_PATHS,
                command_runner=runner,
                provenance_loader=lambda path, value: {
                    "schema_version": 1, "source_revision": value},
            )
        self.assertEqual(document, fixture)
        self.assertEqual(document["installed"]["ownership"]["path_count"],
                         len(document["installed"]["ownership"]["paths"]))

    def test_preflight_fails_closed_on_ownership_and_executable_drift(self) -> None:
        fixture = example_attestation_document()
        hashes = {item["path"]: item["sha256"]
                  for item in fixture["installed"]["executables"]}
        # Source verification and manifest coverage are exercised independently;
        # this case isolates installed ownership/executable drift.
        for mutation, executable_check in (
            (lambda paths: paths.pop(0), lambda path: True),
            (lambda paths: paths[0].update({"path": "/home/user/private"}),
             lambda path: True),
            (lambda paths: next(
                item for item in paths
                if item["path"] == REQUIRED_INSTALLED_FILE_GROUPS[0][0]
            ).update({"path": "/usr/share/prismdrake/unexpected-resource"}),
             lambda path: True),
            (lambda paths: None,
             lambda path: str(path) != INSTALLED_EXECUTABLE_PATHS[2]),
        ):
            with self.subTest(mutation=mutation):
                owned = copy.deepcopy(fixture["installed"]["ownership"]["paths"])
                mutation(owned)
                with self.assertRaises(Pd2CollectorError):
                    # An ownership failure occurs before source/file observers.
                    capture_preflight_attestation(
                        Path("/missing"), Path("/missing.bundle"), owned,
                        Path("/fixture"), hash_file=lambda path: hashes[str(path)],
                        is_executable=executable_check,
                    )

    def test_captures_only_equal_user_state_kind_and_hashes(self) -> None:
        before = {kind: Path(f"/before/{kind}") for kind in USER_DATA_KINDS}
        after = {kind: Path(f"/after/{kind}") for kind in USER_DATA_KINDS}
        hashes = {str(path): str(index + 1) * 64
                  for index, path in enumerate(before.values())}
        hashes.update({str(after[kind]): hashes[str(before[kind])] for kind in USER_DATA_KINDS})
        records = capture_user_state_preservation(
            before, after, hash_file=lambda path: hashes[str(path)],
        )
        self.assertEqual(tuple(item["kind"] for item in records), USER_DATA_KINDS)
        self.assertTrue(all(set(item) == {"kind", "before_sha256", "after_sha256"}
                            for item in records))
        hashes[str(after["pins"])] = "f" * 64
        with self.assertRaisesRegex(Pd2CollectorError,
                                    "^user_data_changed_during_unmerge$"):
            capture_user_state_preservation(
                before, after, hash_file=lambda path: hashes[str(path)],
            )
        hashes[str(after["pins"])] = hashes[str(before["pins"])]
        hashes[str(after["config_toml"])] = "invalid"
        with self.assertRaisesRegex(Pd2CollectorError, "^capture_sha256_invalid$"):
            capture_user_state_preservation(
                before, after, hash_file=lambda path: hashes[str(path)],
            )

    def test_unmerge_checks_every_owned_regular_path(self) -> None:
        owned = example_attestation_document()["installed"]["ownership"]["paths"]
        visited: list[str] = []

        def absent(path: Path) -> bool:
            visited.append(str(path))
            return False

        capture = capture_unmerge_absence(owned, path_exists=absent)
        expected = [item["path"] for item in owned if item["kind"] == "regular"]
        self.assertEqual(visited, expected)
        self.assertEqual(capture["absent_path_count"], len(expected))
        self.assertTrue(capture["owned_regular_paths_absent"])
        with self.assertRaisesRegex(Pd2CollectorError, "^owned_regular_path_remains$"):
            capture_unmerge_absence(
                owned,
                path_exists=lambda path: str(path) == INSTALLED_EXECUTABLE_PATHS[4],
            )

    def test_finalizer_requires_exact_preflight_and_both_demo_harness_bindings(self) -> None:
        attestation = example_attestation_document()
        draft = example_evidence_document()
        self.assertEqual(finalize_lifecycle(attestation, draft), draft)
        mutations = (
            lambda value: value["source"].update({"revision": "f" * 40}),
            lambda value: value["tested_artifact"].update({"artifact_sha256": "0" * 64}),
            lambda value: value["initial_install"]["executables"][0].update(
                {"sha256": "0" * 64}),
            lambda value: value["installed_demonstrations"][1].update(
                {"harness_manifest_sha256": "0" * 64}),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                changed = example_evidence_document()
                mutation(changed)
                with self.assertRaises(Pd2CollectorError):
                    finalize_lifecycle(attestation, changed)

    def test_hashes_bounded_regular_files_without_following_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            regular = root / "regular"
            regular.write_bytes(b"prismdrake")
            self.assertEqual(sha256_regular_file(regular),
                             hashlib.sha256(b"prismdrake").hexdigest())
            symlink = root / "symlink"
            symlink.symlink_to(regular)
            with self.assertRaisesRegex(Pd2CollectorError, "^capture_file_invalid$"):
                sha256_regular_file(symlink)

    def test_atomic_writer_publishes_mode_0600_and_rejects_symlink_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "evidence.json"
            atomic_write_json(output, {"value": 1})
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), {"value": 1})
            atomic_write_json(output, {"value": 2})
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), {"value": 2})
            output.unlink()
            output.symlink_to(root / "elsewhere")
            with self.assertRaisesRegex(Pd2CollectorError, "^output_target_invalid$"):
                atomic_write_json(output, {"value": 3})

    def test_capture_loader_rejects_duplicate_nonfinite_and_oversized_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "capture.json"
            for payload in ('{"value":1,"value":2}', '{"value":Infinity}'):
                path.write_text(payload, encoding="utf-8")
                with self.assertRaisesRegex(Pd2CollectorError, "^capture_json_invalid$"):
                    load_capture_json(path)
            path.write_bytes(b" " * (524_288 + 1))
            with self.assertRaisesRegex(Pd2CollectorError, "^capture_json_size_invalid$"):
                load_capture_json(path)

    def test_collector_has_no_portage_execution_dependency(self) -> None:
        module = Path(__file__).with_name("pd2_portage_lifecycle_collector.py")
        source = module.read_text(encoding="utf-8")
        self.assertNotIn("os.system", source)
        self.assertNotIn("execv", source)
        self.assertNotIn("depclean", source)

    def test_source_verifier_rejects_dirty_or_mismatched_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkout = root / "checkout"
            checkout.mkdir()
            bundle = root / "source.bundle"
            bundle.write_bytes(b"bundle")
            revision = "1" * 40

            def hash_file(path: Path) -> str:
                return "2" * 64 if path == bundle else "3" * 64

            def runner(command: list[str], cwd: Path | None) -> str:
                del cwd
                if "rev-parse" in command:
                    return revision + "\n"
                if "status" in command:
                    return ""
                if "verify" in command:
                    return ""
                return f"{revision} HEAD\n"

            source = verify_exact_source(checkout, bundle, hash_file, runner)
            self.assertEqual(source["revision"], revision)
            self.assertEqual(source["bundle_sha256"], "2" * 64)

            def dirty(command: list[str], cwd: Path | None) -> str:
                if "status" in command:
                    return "?? untracked\n"
                return runner(command, cwd)

            with self.assertRaisesRegex(Pd2CollectorError,
                                        "^source_checkout_not_clean$"):
                verify_exact_source(checkout, bundle, hash_file, dirty)


if __name__ == "__main__":
    unittest.main()
