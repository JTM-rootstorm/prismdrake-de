#!/usr/bin/env python3
"""Hermetic tests for the Gentoo VM safety helpers and script policy."""

from __future__ import annotations

from pathlib import Path
import shlex
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKS = ROOT / "tools/gentoo/vm-checks.sh"
BOOTSTRAP = ROOT / "tools/gentoo/bootstrap-vm.sh"
VERIFY = ROOT / "tools/gentoo/verify-vm.sh"


class ShellHelperTestCase(unittest.TestCase):
    def call_helper(
        self, function: str, *arguments: str, check: bool = False
    ) -> subprocess.CompletedProcess[str]:
        command = f". {shlex.quote(str(CHECKS))}\n{function} \"$@\""
        return subprocess.run(
            ["/bin/sh", "-c", command, "vm-helper-test", *arguments],
            check=check,
            text=True,
            capture_output=True,
        )

    def assert_helper_fails(
        self, function: str, arguments: tuple[str, ...], message: str
    ) -> None:
        result = self.call_helper(function, *arguments)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), message)
        self.assertEqual(result.stderr, "")


class MountValidationTests(ShellHelperTestCase):
    def test_accepts_read_write_virtiofs(self) -> None:
        result = self.call_helper(
            "prismdrake_validate_mount_state", "virtiofs", "rw,relatime"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_missing_mount(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_mount_state",
            ("", ""),
            "no mount contains the requested path",
        )

    def test_rejects_wrong_filesystem(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_mount_state",
            ("ext4", "rw,relatime"),
            "requested path is not on virtiofs",
        )

    def test_rejects_read_only_virtiofs(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_mount_state",
            ("virtiofs", "ro,relatime"),
            "virtiofs mount is not reported read-write",
        )


class OwnershipValidationTests(ShellHelperTestCase):
    def test_accepts_controlled_access_for_mapped_user(self) -> None:
        result = self.call_helper(
            "prismdrake_validate_owner_state", "1001", "builder", "true", "true"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_root_owned_workspace(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_owner_state",
            ("0", "root", "true", "true"),
            "workspace must be owned by an ordinary guest user",
        )

    def test_rejects_unmapped_owner_uid(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_owner_state",
            ("1001", "", "true", "true"),
            "workspace owner UID 1001 has no guest account",
        )

    def test_rejects_owner_without_write_access(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_owner_state",
            ("1001", "builder", "true", "false"),
            "workspace owner builder cannot write the local repository",
        )

    def test_rejects_malformed_owner_uid(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_owner_state",
            ("unknown", "builder", "true", "true"),
            "could not determine workspace owner UID",
        )


class ResourceValidationTests(ShellHelperTestCase):
    def test_accepts_exact_safety_floors(self) -> None:
        result = self.call_helper(
            "prismdrake_validate_resources", "5242880", "4194304"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_insufficient_disk(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_resources",
            ("5242879", "4194304"),
            "less than 5 GiB is free on the guest root filesystem",
        )

    def test_rejects_insufficient_memory(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_resources",
            ("5242880", "4194303"),
            "less than 4 GiB of memory is available to the guest",
        )

    def test_rejects_malformed_disk_observation(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_resources",
            ("", "4194304"),
            "could not determine free space on the guest root filesystem",
        )

    def test_rejects_malformed_memory_observation(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_resources",
            ("5242880", "not-a-number"),
            "could not determine guest memory",
        )


class RepositoryValidationTests(ShellHelperTestCase):
    def test_accepts_canonical_repo_name(self) -> None:
        result = self.call_helper("prismdrake_validate_repo_name", "prismdrake-local")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_missing_repo_name(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_repo_name",
            ("",),
            "local repository metadata has a missing or unexpected repo_name",
        )

    def test_rejects_wrong_repo_name(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_repo_name",
            ("stale-copy",),
            "local repository metadata has a missing or unexpected repo_name",
        )

    def test_accepts_matching_canonical_repository(self) -> None:
        result = self.call_helper(
            "prismdrake_validate_registered_repo",
            "/mnt/shared/repository",
            "/mnt/shared/repository",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_missing_registration(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_registered_repo",
            ("/mnt/shared/repository", ""),
            "Portage does not recognize prismdrake-local",
        )

    def test_rejects_stale_registration(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_registered_repo",
            ("/mnt/shared/repository", "/var/db/repos/prismdrake-old"),
            "Portage resolved prismdrake-local to a stale or unexpected path",
        )


class ModeAndReconciliationTests(ShellHelperTestCase):
    def test_rejects_world_writable_path(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_mode",
            ("777", "/mnt/shared"),
            "refusing world-writable path: /mnt/shared",
        )

    def test_rejects_malformed_mode(self) -> None:
        self.assert_helper_fails(
            "prismdrake_validate_mode",
            ("", "/mnt/shared"),
            "could not determine permissions for /mnt/shared",
        )

    def call_install(self, target: Path, content: str, backup_dir: Path) -> None:
        result = self.call_helper(
            "prismdrake_install_project_file",
            str(target),
            content,
            str(backup_dir),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_identical_content_with_unsafe_mode_is_replaced_and_backed_up(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "active/package.use/prismdrake-dev"
            backup_dir = root / "backups"
            target.parent.mkdir(parents=True)
            target.write_text("policy\n", encoding="utf-8")
            target.chmod(0o600)

            self.call_install(target, "policy", backup_dir)

            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o644)
            self.assertEqual(target.read_text(encoding="utf-8"), "policy\n")
            backups = list(backup_dir.iterdir())
            self.assertEqual(len(backups), 1)
            self.assertEqual(stat.S_IMODE(backups[0].stat().st_mode), 0o600)

    def test_backups_are_unique_during_rapid_reconciliation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "active/repos.conf/prismdrake-local.conf"
            backup_dir = root / "backups"
            target.parent.mkdir(parents=True)
            target.write_text("first\n", encoding="utf-8")
            target.chmod(0o644)

            self.call_install(target, "second", backup_dir)
            target.write_text("third\n", encoding="utf-8")
            self.call_install(target, "fourth", backup_dir)

            backups = list(backup_dir.iterdir())
            self.assertEqual(len(backups), 2)
            self.assertEqual(len({path.name for path in backups}), 2)

    def test_unchanged_safe_file_creates_no_backup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "active/prismdrake-dev"
            backup_dir = root / "backups"
            target.parent.mkdir(parents=True)
            target.write_text("policy\n", encoding="utf-8")
            target.chmod(0o644)

            self.call_install(target, "policy", backup_dir)

            self.assertFalse(backup_dir.exists())


class ScriptPolicyTests(unittest.TestCase):
    def test_shell_scripts_parse(self) -> None:
        for path in (CHECKS, BOOTSTRAP, VERIFY):
            with self.subTest(path=path):
                result = subprocess.run(
                    ["/bin/sh", "-n", str(path)], text=True, capture_output=True
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_bootstrap_has_no_host_or_global_portage_mutations(self) -> None:
        source = BOOTSTRAP.read_text(encoding="utf-8")
        for forbidden in (
            "emerge --depclean",
            "/etc/portage/make.conf",
            "ACCEPT_KEYWORDS=",
            "virsh",
            "qemu-system",
            "qemu:///system",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

    def test_verifier_contains_no_apply_commands(self) -> None:
        source = VERIFY.read_text(encoding="utf-8")
        for forbidden in ("--ask", "pkgdev manifest", "emaint sync", "emerge --sync"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

    def test_verifier_gates_qa_and_resolution_on_valid_state(self) -> None:
        source = VERIFY.read_text(encoding="utf-8")
        self.assertIn('[ "$OVERLAY_VALID" = true ] && command -v pkgcheck', source)
        self.assertIn('[ "$REGISTERED_REPO_VALID" = true ]', source)
        self.assertIn('prismdrake_validate_mount_state', source)
        self.assertIn('prismdrake_validate_resources', source)
        self.assertIn('prismdrake_validate_repo_name', source)

    def test_sync_without_apply_stops_before_guest_inspection(self) -> None:
        result = subprocess.run(
            [str(BOOTSTRAP), "--sync"], text=True, capture_output=True
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--sync requires --apply", result.stderr)

    def test_unknown_arguments_are_rejected(self) -> None:
        for script in (BOOTSTRAP, VERIFY):
            with self.subTest(script=script):
                result = subprocess.run(
                    [str(script), "--not-a-real-option"],
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn("Unknown argument", result.stderr)

    def test_missing_path_values_are_rejected(self) -> None:
        for script in (BOOTSTRAP, VERIFY):
            with self.subTest(script=script):
                result = subprocess.run(
                    [str(script), "--workspace"], text=True, capture_output=True
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("missing value for --workspace", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
