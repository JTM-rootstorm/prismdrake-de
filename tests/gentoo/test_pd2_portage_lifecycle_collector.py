#!/usr/bin/env python3
"""Hermetic seam tests for the non-executing PD2 lifecycle collector."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import stat
import sys
import tempfile
import unittest
from unittest import mock

import pd2_portage_lifecycle_collector as collector

from pd2_installed_artifact_attestation import (
    INSTALLED_EXECUTABLE_PATHS,
    example_attestation_document,
)
from pd2_harness_manifest import HARNESS_RELATIVE_PATHS
from pd2_portage_lifecycle_collector import (
    BUILD_PROVENANCE_PATH,
    OPTIONAL_PACKAGE_SKIP_TEST,
    PKGCHECK_EXIT_POLICY,
    Pd2CollectorError,
    atomic_write_json,
    capture_preflight_attestation,
    capture_direct_linkage,
    capture_installed_demonstration,
    capture_ordinary_reinstall,
    capture_package_test,
    capture_portage_qa,
    capture_portage_resolutions,
    capture_sandbox_exclusions,
    capture_unmerge_absence,
    capture_user_state_preservation,
    finalize_lifecycle,
    load_capture_json,
    run_checked,
    sha256_regular_file,
    stage_document,
    verify_exact_source,
)
from pd2_portage_lifecycle_evidence import (
    EXCLUDED_TESTS,
    USER_DATA_KINDS,
    example_evidence_document,
)


class Pd2LifecycleCollectorTests(unittest.TestCase):
    @staticmethod
    def create_vdb(root: Path, attestation: dict, use_test: bool) -> tuple[Path, bytes]:
        vdb = root / "vdb"
        vdb.mkdir()
        (vdb / "prismdrake-9999.ebuild").write_bytes(b"ebuild")
        (vdb / "repository").write_text("prismdrake-local\n", encoding="utf-8")
        (vdb / "USE").write_text("test qt6\n" if use_test else "qt6\n", encoding="utf-8")
        lines = []
        for record in attestation["installed"]["ownership"]["paths"]:
            if record["kind"] == "directory":
                lines.append(f"dir {record['path']}")
            elif record["kind"] == "symlink":
                lines.append(f"sym {record['path']} -> target 1")
            else:
                lines.append(f"obj {record['path']} {'1' * 64} 1")
        contents = ("\n".join(lines) + "\n").encode("utf-8")
        (vdb / "CONTENTS").write_bytes(contents)
        return vdb, contents

    @staticmethod
    def equery_output(attestation: dict) -> str:
        return "\n".join(
            item["path"] for item in attestation["installed"]["ownership"]["paths"]
        ) + "\n"

    def test_captures_five_executables_and_complete_owned_path_set(self) -> None:
        fixture = example_attestation_document()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkout = root / "source"
            checkout.mkdir()
            bundle = root / "source.bundle"
            bundle.touch()
            controlled_fixture = root / "fixture"
            vdb, contents = self.create_vdb(root, fixture, True)
            fixture["installed"]["vdb"]["contents_sha256"] = hashlib.sha256(
                contents
            ).hexdigest()
            hashes = {item["path"]: item["sha256"]
                      for item in fixture["installed"]["executables"]}
            hashes.update({str(checkout / relative): "7" * 64
                           for relative in HARNESS_RELATIVE_PATHS})
            hashes[str(checkout / "packaging/gentoo/repository/x11-misc/prismdrake/prismdrake-9999.ebuild")] = "a" * 64
            hashes[str(bundle)] = "9" * 64
            hashes[str(controlled_fixture)] = "8" * 64
            hashes[str(BUILD_PROVENANCE_PATH)] = "6" * 64
            hashes[str(vdb / "prismdrake-9999.ebuild")] = "a" * 64
            revision = fixture["source"]["revision"]

            def runner(command: list[str], cwd: Path | None) -> str:
                del cwd
                if "rev-parse" in command:
                    return revision + "\n"
                if "status" in command or "verify" in command:
                    return ""
                if command[0] == "/usr/bin/equery":
                    return self.equery_output(fixture)
                return f"{revision} HEAD\n"

            document = capture_preflight_attestation(
                checkout, bundle, controlled_fixture,
                vdb_directory=vdb,
                hash_file=lambda path: hashes[str(path)],
                is_executable=lambda path: str(path) in INSTALLED_EXECUTABLE_PATHS,
                command_runner=runner,
                provenance_loader=lambda path, value: {
                    "schema_version": 1, "source_revision": value},
                kind_matches=lambda record: True,
            )
        self.assertEqual(document, fixture)
        self.assertEqual(document["installed"]["ownership"]["path_count"],
                         len(document["installed"]["ownership"]["paths"]))

    def test_preflight_fails_closed_on_vdb_use_and_ownership_drift(self) -> None:
        fixture = example_attestation_document()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            vdb, _contents = self.create_vdb(root, fixture, False)
            source = copy.deepcopy(fixture["source"])
            with self.assertRaisesRegex(Pd2CollectorError, "^vdb_use_test_mismatch$"):
                collector.capture_portage_install_metadata(
                    source, True, vdb,
                    hash_file=lambda path: source["ebuild_sha256"],
                    command_runner=lambda command, cwd: self.equery_output(fixture),
                    kind_matches=lambda record: True,
                )
            (vdb / "USE").write_text("test qt6\n", encoding="utf-8")
            with self.assertRaisesRegex(
                    Pd2CollectorError, "^equery_vdb_ownership_mismatch$"):
                collector.capture_portage_install_metadata(
                    source, True, vdb,
                    hash_file=lambda path: source["ebuild_sha256"],
                    command_runner=lambda command, cwd: "/usr/bin/unowned\n",
                    kind_matches=lambda record: True,
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

    def test_unmerge_checks_every_owned_file_and_symlink(self) -> None:
        owned = example_attestation_document()["installed"]["ownership"]["paths"]
        visited: list[str] = []

        def absent(path: Path) -> bool:
            visited.append(str(path))
            return False

        capture = capture_unmerge_absence(owned, path_exists=absent)
        expected = [item["path"] for item in owned if item["kind"] != "directory"]
        self.assertEqual(visited, expected)
        self.assertEqual(capture["absent_path_count"], len(expected))
        self.assertTrue(capture["owned_removable_paths_absent"])
        with self.assertRaisesRegex(Pd2CollectorError, "^owned_removable_path_remains$"):
            capture_unmerge_absence(
                owned,
                path_exists=lambda path: str(path).endswith("/current-theme"),
            )

    @staticmethod
    def lifecycle_stages(document: dict) -> list[dict]:
        unmerge = document["unmerge"]
        return [
            stage_document("pd2_portage_qa", "portage_qa", document["portage_qa"]),
            stage_document(
                "pd2_portage_resolutions", "resolutions", document["resolutions"],
            ),
            stage_document("pd2_package_test", "package_test", document["package_test"]),
            stage_document(
                "pd2_direct_linkage", "direct_linkage", document["direct_linkage"],
            ),
            stage_document(
                "pd2_sandbox_exclusions", "sandbox_exclusions",
                document["sandbox_exclusions"],
            ),
            stage_document(
                "pd2_installed_demonstration", "demonstration",
                document["installed_demonstrations"][0],
            ),
            stage_document("pd2_unmerge_absence", "unmerge_absence", {
                "owned_removable_paths_absent": unmerge["owned_removable_paths_absent"],
                "absent_path_count": unmerge["absent_path_count"],
                "removable_path_set_sha256": unmerge["removable_path_set_sha256"],
            }),
            stage_document(
                "pd2_user_data_preservation", "user_data", unmerge["user_data"],
            ),
            stage_document(
                "pd2_ordinary_reinstall", "ordinary_reinstall",
                document["ordinary_reinstall"],
            ),
            stage_document(
                "pd2_installed_demonstration", "demonstration",
                document["installed_demonstrations"][1],
            ),
        ]

    def test_finalizer_assembles_only_closed_stage_artifacts(self) -> None:
        attestation = example_attestation_document()
        draft = example_evidence_document()
        stages = self.lifecycle_stages(draft)
        self.assertEqual(finalize_lifecycle(attestation, *stages), draft)

        wrong_kind = copy.deepcopy(stages)
        wrong_kind[0]["stage_kind"] = "pd2_package_test"
        with self.assertRaisesRegex(Pd2CollectorError, "^final_stage_invalid$"):
            finalize_lifecycle(attestation, *wrong_kind)

        extra_key = copy.deepcopy(stages)
        extra_key[1]["caller_note"] = "trust me"
        with self.assertRaisesRegex(Pd2CollectorError, "^final_stage_invalid$"):
            finalize_lifecycle(attestation, *extra_key)

        wrong_phase = copy.deepcopy(stages)
        wrong_phase[-1]["demonstration"]["phase"] = "initial_install"
        with self.assertRaises(Pd2CollectorError):
            finalize_lifecycle(attestation, *wrong_phase)

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

    def test_executes_exact_package_and_outside_sandbox_ctest_selections(self) -> None:
        attestation = example_attestation_document()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            source = root / "source"
            source.mkdir()
            (build / "CMakeCache.txt").write_text(
                "PRISMDRAKE_CONFIGURED_SOURCE_REVISION:INTERNAL="
                f"{attestation['source']['revision']}\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n",
                encoding="utf-8",
            )
            fixture = build / "tests/integration/prismdrake-controlled-window-fixture"
            fixture.parent.mkdir(parents=True)
            fixture.write_bytes(b"fixture")
            test_command = source / "test-command.py"
            test_command.write_bytes(b"test")
            ordinary = "ContractTest.Ordinary"
            all_names = (ordinary, OPTIONAL_PACKAGE_SKIP_TEST, *EXCLUDED_TESTS)
            calls: list[list[str]] = []
            skipped_junit: str | None = None

            def runner(command: list[str], cwd: Path | None) -> str:
                del cwd
                calls.append(command)
                if command[:3] == ["/usr/bin/git", "rev-parse", "--verify"]:
                    return attestation["source"]["revision"] + "\n"
                if command[:3] == ["/usr/bin/git", "status", "--porcelain=v1"]:
                    return ""
                if "--show-only=json-v1" in command:
                    selected = ((ordinary, OPTIONAL_PACKAGE_SKIP_TEST)
                                if "--exclude-regex" in command else all_names)
                    return json.dumps({"tests": [
                        {"name": name, "command": [str(test_command)]}
                        for name in selected
                    ]})
                if "--output-junit" in command:
                    if "--exclude-regex" in command:
                        selected = (ordinary, OPTIONAL_PACKAGE_SKIP_TEST)
                    else:
                        expression = command[command.index("--tests-regex") + 1]
                        selected = tuple(name for name in all_names
                                         if re.fullmatch(expression, name))
                    junit = Path(command[command.index("--output-junit") + 1])
                    junit.write_text(
                        "<testsuite>" + "".join(
                            (f'<testcase name="{name}" status="run"><skipped />'
                             f'</testcase>' if skipped_junit == name else
                             f'<testcase name="{name}" status="run" />')
                            for name in selected
                        ) + "</testsuite>",
                        encoding="utf-8",
                    )
                return ""

            def hash_file(path: Path) -> str:
                if path == fixture:
                    return attestation["demo_harness"]["controlled_fixture_sha256"]
                return "1" * 64

            package = capture_package_test(
                attestation, build, source, fixture,
                hash_file=hash_file, command_runner=runner,
            )
            self.assertEqual(package["executed_test_count"], 2)
            self.assertEqual(package["skipped_tests"], [])
            self.assertEqual(tuple(package["excluded_tests"]), EXCLUDED_TESTS)
            self.assertIn("--exclude-regex", calls[-1])

            calls.clear()
            exclusions = capture_sandbox_exclusions(
                attestation, build, source, fixture,
                hash_file=hash_file, command_runner=runner,
            )
            self.assertEqual([item["attempts"] for item in exclusions], [1, 1, 5, 5])
            self.assertTrue(all(item["test_manifest_sha256"] ==
                                package["test_manifest_sha256"] for item in exclusions))
            executions = [call for call in calls if "--tests-regex" in call]
            self.assertEqual(len(executions), 12)
            skipped_junit = OPTIONAL_PACKAGE_SKIP_TEST
            package = capture_package_test(
                attestation, build, source, fixture,
                hash_file=hash_file, command_runner=runner,
            )
            self.assertEqual(package["passed_test_count"], 1)
            self.assertEqual(package["skipped_tests"], [{
                "test_id": OPTIONAL_PACKAGE_SKIP_TEST,
                "reason": "requires_non_root_owner_transition",
            }])

            skipped_junit = ordinary
            with self.assertRaisesRegex(Pd2CollectorError, "^ctest_not_all_passed$"):
                capture_package_test(
                    attestation, build, source, fixture,
                    hash_file=hash_file, command_runner=runner,
                )

            def disabled_runner(command: list[str], cwd: Path | None) -> str:
                result = runner(command, cwd)
                if "--show-only=json-v1" in command and "--exclude-regex" in command:
                    document = json.loads(result)
                    document["tests"][0]["properties"] = [
                        {"name": "DISABLED", "value": True},
                    ]
                    return json.dumps(document)
                return result

            with self.assertRaisesRegex(Pd2CollectorError,
                                        "^package_test_disabled$"):
                capture_package_test(
                    attestation, build, source, fixture,
                    hash_file=hash_file, command_runner=disabled_runner,
                )

            escaped = root / "escaped-test.py"
            escaped.write_bytes(b"escaped")
            inventory = [{"name": ordinary, "command": [str(source / "../escaped-test.py")]}]
            with self.assertRaisesRegex(Pd2CollectorError,
                                        "^ctest_project_command_unbound$"):
                collector._test_manifest_sha256(
                    inventory, build, source, fixture, hash_file,
                )

            symlinked = source / "symlinked-test.py"
            symlinked.symlink_to(test_command)
            inventory[0]["command"] = [str(symlinked)]
            with self.assertRaisesRegex(Pd2CollectorError,
                                        "^ctest_project_command_unbound$"):
                collector._test_manifest_sha256(
                    inventory, build, source, fixture, hash_file,
                )

    def test_command_output_is_bounded_while_child_runs(self) -> None:
        with mock.patch.object(collector, "MAX_COMMAND_OUTPUT_BYTES", 1024):
            with self.assertRaisesRegex(Pd2CollectorError, "^observer_output_oversized$"):
                run_checked([
                    "/usr/bin/python3", "-c",
                    "import sys; sys.stdout.write('x' * 4096)",
                ])

    def test_executes_disposable_qa_and_both_bound_resolutions(self) -> None:
        attestation = example_attestation_document()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            repository = source / "packaging/gentoo/repository"
            ebuild = repository / "x11-misc/prismdrake/prismdrake-9999.ebuild"
            ebuild.parent.mkdir(parents=True)
            ebuild.write_text("EAPI=8\n", encoding="utf-8")
            (repository / "profiles").mkdir()
            (repository / "profiles/repo_name").write_text(
                "prismdrake-local\n", encoding="utf-8",
            )
            installed_repository = root / "installed-repository"
            installed_ebuild = installed_repository / attestation["source"]["ebuild_path"]
            installed_ebuild.parent.mkdir(parents=True)
            installed_ebuild.write_text("EAPI=8\n", encoding="utf-8")
            calls: list[tuple[list[str], Path | None]] = []

            def runner(command: list[str], cwd: Path | None) -> str:
                calls.append((command, cwd))
                if command[:3] == ["/usr/bin/git", "rev-parse", "--verify"]:
                    return attestation["source"]["revision"] + "\n"
                if command[:3] == ["/usr/bin/git", "status", "--porcelain=v1"]:
                    return ""
                if command[0] == "/usr/bin/pkgdev":
                    assert cwd is not None
                    (cwd / "Manifest").write_text("generated\n", encoding="utf-8")
                    return "manifest complete\n"
                if command[0] == "/usr/bin/pkgcheck":
                    return ""
                if command[0] == "/usr/bin/portageq":
                    return str(installed_repository) + "\n"
                if command[0] == "/usr/bin/env":
                    return ("[ebuild N] x11-misc/prismdrake-9999 " +
                            ("test\n" if command[1] == "USE=test" else "-test\n"))
                raise AssertionError(command)

            def hash_file(path: Path) -> str:
                if path.name == "prismdrake-9999.ebuild":
                    return attestation["source"]["ebuild_sha256"]
                return collector.sha256_regular_file(path)

            qa = capture_portage_qa(
                attestation, source, hash_file=hash_file, command_runner=runner,
            )
            self.assertEqual(qa["result"], "passed")
            self.assertFalse((repository / "Manifest").exists())
            self.assertRegex(qa["repository_tree_sha256"], r"^[0-9a-f]{64}$")
            pkgcheck_calls = [command for command, _cwd in calls
                              if command[0] == "/usr/bin/pkgcheck"]
            self.assertEqual(pkgcheck_calls[0][2:4],
                             ["--exit", PKGCHECK_EXIT_POLICY])

            def qa_finding(command: list[str], cwd: Path | None) -> str:
                if command[0] == "/usr/bin/pkgcheck":
                    raise Pd2CollectorError("observer_command_failed")
                return runner(command, cwd)

            with self.assertRaisesRegex(Pd2CollectorError,
                                        "^observer_command_failed$"):
                capture_portage_qa(
                    attestation, source, hash_file=hash_file,
                    command_runner=qa_finding,
                )

            records = capture_portage_resolutions(
                attestation, source, hash_file=hash_file, command_runner=runner,
            )
            self.assertEqual([item["mode"] for item in records], ["default", "use_test"])
            self.assertNotEqual(records[0]["dependency_graph_sha256"],
                                records[1]["dependency_graph_sha256"])
            emerge_calls = [command for command, _cwd in calls
                            if command[0] == "/usr/bin/env"]
            self.assertEqual([command[1] for command in emerge_calls],
                             ["USE=-test", "USE=test"])

            def same_graph(command: list[str], cwd: Path | None) -> str:
                if command[0] == "/usr/bin/env":
                    return "[ebuild N] x11-misc/prismdrake-9999\n"
                return runner(command, cwd)

            with self.assertRaisesRegex(
                    Pd2CollectorError, "^resolution_dependency_graphs_not_distinct$"):
                capture_portage_resolutions(
                    attestation, source, hash_file=hash_file,
                    command_runner=same_graph,
                )
    def test_scanelf_and_reinstall_are_bound_to_attested_bytes(self) -> None:
        attestation = example_attestation_document()
        hashes = {item["path"]: item["sha256"]
                  for item in attestation["installed"]["executables"]}
        hashes[str(BUILD_PROVENANCE_PATH)] = \
            attestation["installed"]["build_provenance"]["sha256"]
        scanelf_output = "\n".join(
            f"{path};libstdc++.so.6{',libtomlplusplus.so.3' if index == 1 else ''}"
            for index, path in enumerate(INSTALLED_EXECUTABLE_PATHS)
        ) + "\n"
        linkage = capture_direct_linkage(
            attestation,
            hash_file=lambda path: hashes[str(path)],
            command_runner=lambda command, cwd: scanelf_output,
        )
        self.assertEqual(tuple(item["path"] for item in linkage["entries"]),
                         INSTALLED_EXECUTABLE_PATHS)

        with tempfile.TemporaryDirectory() as temporary:
            vdb, contents = self.create_vdb(Path(temporary), attestation, False)
            contents_hash = hashlib.sha256(contents).hexdigest()
            attestation["installed"]["vdb"]["contents_sha256"] = contents_hash
            hashes[str(vdb / "prismdrake-9999.ebuild")] = \
                attestation["source"]["ebuild_sha256"]
            reinstall = capture_ordinary_reinstall(
                attestation,
                vdb_directory=vdb,
                hash_file=lambda path: hashes[str(path)],
                is_executable=lambda path: str(path) in INSTALLED_EXECUTABLE_PATHS,
                provenance_loader=lambda path, revision: {
                    "schema_version": 1, "source_revision": revision},
                command_runner=lambda command, cwd: self.equery_output(attestation),
                kind_matches=lambda record: True,
            )
        self.assertEqual(reinstall["executables"],
                         attestation["installed"]["executables"])
        self.assertEqual(reinstall["ownership"], attestation["installed"]["ownership"])

    def test_normalizes_demo_output_from_attested_executable_hashes(self) -> None:
        integration_path = str(Path(__file__).resolve().parents[1] / "integration")
        if integration_path not in sys.path:
            sys.path.insert(0, integration_path)
        import pd2_demo  # pylint: disable=import-outside-toplevel

        attestation = example_attestation_document()
        executable_hashes = {
            Path(item["path"]).name: item["sha256"]
            for item in attestation["installed"]["executables"]
        }

        class Port:
            def installed_executable_hashes(self):
                return executable_hashes

            def capture_process_ownership(self):
                return {role: object() for role in pd2_demo.PROCESS_ROLES}

            def run_step(self, phase, identifier):
                del phase, identifier
                return pd2_demo.Observation(True, True)

            def cleanup(self, phase):
                del phase
                return pd2_demo.Observation(True, True)

        evidence = pd2_demo.run_harness(
            attestation["source"]["revision"],
            attestation["demo_harness"]["manifest_sha256"],
            Port(),
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "demo.json"
            path.write_text(json.dumps(evidence), encoding="utf-8")
            capture = capture_installed_demonstration(
                attestation, "initial_install", path,
            )
        self.assertEqual(capture["normalized_results_sha256"],
                         evidence["normalized_results_sha256"])
        self.assertEqual(capture["harness_manifest_sha256"],
                         attestation["demo_harness"]["manifest_sha256"])

        substituted = pd2_demo.run_harness(
            attestation["source"]["revision"], "0" * 64, Port()
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "substituted-demo.json"
            path.write_text(json.dumps(substituted), encoding="utf-8")
            with self.assertRaisesRegex(
                    Pd2CollectorError, "^demonstration_harness_manifest_mismatch$"):
                capture_installed_demonstration(attestation, "initial_install", path)


if __name__ == "__main__":
    unittest.main()
