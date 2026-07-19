#!/usr/bin/env python3
"""Hermetic contracts for PD2 notification failure replay."""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_dbus import PrivateBusEndpoint, ProbeResult  # noqa: E402
from pd2_notification_failure_replay import (  # noqa: E402
    HISTORY_RELATIVE_PATH,
    MALFORMED_HISTORY_BYTES,
    NOTIFYD_CONFLICT_DIAGNOSTIC,
    FailureEvidence,
    GioNotificationOwnerLease,
    NotificationFailureError,
    create_malformed_history_fixture,
    validate_failure_evidence,
    verify_conflicting_notification_owner,
    verify_malformed_history_recovery,
)


def cooperative_owner_worker(channel: Any, address: str) -> None:
    del address
    channel.send(("ready",))
    while True:
        command = channel.recv()
        if command == "verify":
            channel.send(("owned",))
        elif command == "close":
            channel.send(("closed",))
            channel.close()
            return
        else:
            channel.send(("failed",))
            return


def lost_owner_worker(channel: Any, address: str) -> None:
    del address
    channel.send(("ready",))
    if channel.recv() == "verify":
        channel.send(("lost",))
    channel.close()


def malformed_owner_worker(channel: Any, address: str) -> None:
    del address
    channel.send(("ready", "private"))
    channel.close()


def stalled_owner_worker(channel: Any, address: str) -> None:
    del channel, address
    time.sleep(10)


class FakeProbe:
    def __init__(self, document: dict[str, Any], *, generation: int = 1) -> None:
        self.document = document
        self.generation = generation

    def call(self, operation: str, schema_version: int) -> ProbeResult:
        if operation != "notification_get_current_state" or schema_version != 1:
            raise RuntimeError("unexpected")
        return ProbeResult(
            operation,
            "notification_state_observed",
            (self.generation, self.document),
        )


class FakeOwner:
    def __init__(self, *, fail_at: int | None = None) -> None:
        self.calls = 0
        self.fail_at = fail_at

    def verify(self) -> None:
        self.calls += 1
        if self.calls == self.fail_at:
            raise NotificationFailureError("notification_failure_owner_lost")


class PrivateFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.root.chmod(0o700)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def private_bus(self) -> tuple[dict[str, str], PrivateBusEndpoint, Any]:
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        bus_path = runtime / "bus"
        bus_path.write_bytes(b"attested-private-bus")
        bus_path.chmod(0o600)
        listener = bus_path.open("rb")
        metadata = bus_path.lstat()
        environment = {
            "DBUS_SESSION_BUS_ADDRESS": f"unix:path={bus_path}",
            "XDG_RUNTIME_DIR": str(runtime),
            "PATH": "/usr/bin:/bin",
        }
        endpoint = PrivateBusEndpoint(
            environment["DBUS_SESSION_BUS_ADDRESS"], metadata.st_dev, metadata.st_ino
        )
        return environment, endpoint, listener

    @staticmethod
    def fake_attestor(
        environment: dict[str, str], endpoint: PrivateBusEndpoint
    ) -> str:
        if environment.get("DBUS_SESSION_BUS_ADDRESS") != endpoint.address:
            raise NotificationFailureError("notification_failure_environment_invalid")
        path = Path(endpoint.address.removeprefix("unix:path="))
        metadata = path.lstat()
        if (metadata.st_dev, metadata.st_ino) != (
            endpoint.socket_device,
            endpoint.socket_inode,
        ):
            raise NotificationFailureError("notification_failure_owner_lost")
        return endpoint.address

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(NotificationFailureError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)
        self.assertIsNone(raised.exception.__cause__)
        self.assertNotIn(str(self.root), str(raised.exception))


class MalformedHistoryReplayTests(PrivateFixture):
    @staticmethod
    def empty_state(*, mode: str = "persistent") -> dict[str, Any]:
        return {
            "schema_version": 1,
            "generation": 1,
            "history_mode": mode,
            "effective_do_not_disturb": False,
            "runtime_do_not_disturb_override": None,
            "records": [],
        }

    def test_creates_private_known_fixture_and_accepts_empty_persistent_state(self) -> None:
        fixture = create_malformed_history_fixture(self.root)
        self.assertEqual(fixture.path, self.root / HISTORY_RELATIVE_PATH)
        self.assertEqual(fixture.path.read_bytes(), MALFORMED_HISTORY_BYTES)
        self.assertEqual(stat.S_IMODE(fixture.path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(fixture.path.parent.stat().st_mode), 0o700)

        result = verify_malformed_history_recovery(
            fixture, FakeProbe(self.empty_state())  # type: ignore[arg-type]
        )
        self.assertEqual(
            result.as_dict(),
            {"category": "malformed_persistent_history_recovered", "status": "passed"},
        )
        validate_failure_evidence(result)
        self.assertEqual(fixture.path.read_bytes(), MALFORMED_HISTORY_BYTES)

    def test_never_replaces_existing_history(self) -> None:
        path = self.root / HISTORY_RELATIVE_PATH
        prismdrake = self.root / "prismdrake"
        prismdrake.mkdir(mode=0o700)
        path.parent.mkdir(mode=0o700)
        path.write_bytes(b"private existing history")
        path.chmod(0o600)
        self.assert_category(
            "notification_failure_history_exists",
            lambda: create_malformed_history_fixture(self.root),
        )
        self.assertEqual(path.read_bytes(), b"private existing history")

    def test_rejects_unsafe_root_and_child_without_following_symlinks(self) -> None:
        self.root.chmod(0o755)
        self.assert_category(
            "notification_failure_state_root_invalid",
            lambda: create_malformed_history_fixture(self.root),
        )
        self.root.chmod(0o700)
        outside = self.root / "outside"
        outside.mkdir(mode=0o700)
        (self.root / "prismdrake").symlink_to(outside, target_is_directory=True)
        self.assert_category(
            "notification_failure_history_setup_failed",
            lambda: create_malformed_history_fixture(self.root),
        )

    def test_detects_replacement_mode_change_and_content_change(self) -> None:
        for mutation in ("replace", "mode", "content"):
            with self.subTest(mutation=mutation):
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    root.chmod(0o700)
                    fixture = create_malformed_history_fixture(root)
                    if mutation == "replace":
                        fixture.path.unlink()
                        fixture.path.write_bytes(MALFORMED_HISTORY_BYTES)
                        fixture.path.chmod(0o600)
                    elif mutation == "mode":
                        fixture.path.chmod(0o644)
                    else:
                        fixture.path.write_bytes(b"private mutation")
                    self.assert_category(
                        "notification_failure_history_invalid", fixture.verify_retained
                    )

    def test_rejects_nonpersistent_nonempty_and_invalid_probe_results(self) -> None:
        fixture = create_malformed_history_fixture(self.root)
        nonempty = self.empty_state()
        nonempty["records"] = [
            {
                "record_id": 1,
                "content_generation": 1,
                "notification_id": None,
                "urgency": "normal",
                "transient": False,
                "actions": [],
            }
        ]
        for document in (self.empty_state(mode="session"), nonempty, {"private": True}):
            with self.subTest(document=document):
                self.assert_category(
                    "notification_failure_history_state_invalid",
                    lambda document=document: verify_malformed_history_recovery(
                        fixture, FakeProbe(document)  # type: ignore[arg-type]
                    ),
                )


class NotificationOwnerLeaseTests(PrivateFixture):
    def test_holds_and_releases_opaque_owner_with_bounded_protocol(self) -> None:
        environment, endpoint, listener = self.private_bus()
        try:
            with GioNotificationOwnerLease(
                environment,
                endpoint,
                worker=cooperative_owner_worker,
                attestor=self.fake_attestor,
            ) as owner:
                owner.verify()
        finally:
            listener.close()

    def test_rejects_lost_malformed_and_stalled_workers_redacted(self) -> None:
        scenarios = (
            (lost_owner_worker, 0.5, "notification_failure_owner_lost"),
            (malformed_owner_worker, 0.5, "notification_failure_owner_protocol_invalid"),
            (stalled_owner_worker, 0.05, "notification_failure_owner_timeout"),
        )
        for worker, timeout, category in scenarios:
            with self.subTest(category=category):
                with tempfile.TemporaryDirectory() as temporary:
                    self.root = Path(temporary)
                    self.root.chmod(0o700)
                    environment, endpoint, listener = self.private_bus()
                    try:
                        self.assert_category(
                            category,
                            lambda: GioNotificationOwnerLease(
                                environment,
                                endpoint,
                                worker=worker,
                                attestor=self.fake_attestor,
                                command_seconds=timeout,
                            ),
                        )
                    finally:
                        listener.close()

    def test_rejects_replaced_bus_socket(self) -> None:
        environment, endpoint, listener = self.private_bus()
        owner = GioNotificationOwnerLease(
            environment,
            endpoint,
            worker=cooperative_owner_worker,
            attestor=self.fake_attestor,
        )
        listener.close()
        bus_path = Path(environment["XDG_RUNTIME_DIR"]) / "bus"
        bus_path.unlink()
        bus_path.write_bytes(b"replacement-private-bus")
        bus_path.chmod(0o600)
        try:
            self.assert_category("notification_failure_owner_lost", owner.verify)
        finally:
            try:
                owner.close()
            except NotificationFailureError:
                pass


class ConflictingOwnerReplayTests(PrivateFixture):
    def make_executable(self, body: str) -> Path:
        executable = self.root / "prismdrake-notifyd"
        executable.write_text("#!/bin/sh\n" + body, encoding="utf-8")
        executable.chmod(0o700)
        return executable

    @staticmethod
    def stable_siblings() -> dict[str, tuple[int, int]]:
        return {
            "prismdrake-settingsd": (101, 1001),
            "prismdrake-shell": (102, 1002),
        }

    def test_requires_exact_unavailable_exit_diagnostic_owner_and_siblings(self) -> None:
        environment, endpoint, listener = self.private_bus()
        executable = self.make_executable(
            "printf '%s' 'prismdrake-notifyd: A notification service name is already owned.' >&2\n"
            "printf '\\n' >&2\n"
            "exit 3\n"
        )
        owner = FakeOwner()
        try:
            result = verify_conflicting_notification_owner(
                executable,
                environment,
                endpoint,
                owner,
                self.stable_siblings,
                attestor=self.fake_attestor,
            )
        finally:
            listener.close()
        self.assertEqual(owner.calls, 2)
        self.assertEqual(
            result.as_dict(),
            {"category": "notification_owner_conflict_isolated", "status": "passed"},
        )
        validate_failure_evidence(result)
        runtime = Path(environment["XDG_RUNTIME_DIR"])
        self.assertEqual([path.name for path in runtime.iterdir()], ["bus"])

    def test_rejects_wrong_exit_output_and_owner_loss_without_leaking_output(self) -> None:
        cases = (
            ("exit 0\n", FakeOwner(), "notification_failure_process_failed"),
            (
                "printf 'private notification body' >&2\nexit 3\n",
                FakeOwner(),
                "notification_failure_process_failed",
            ),
            (
                "printf '%s' 'prismdrake-notifyd: A notification service name is already owned.' >&2\n"
                "printf '\\n' >&2\nexit 3\n",
                FakeOwner(fail_at=2),
                "notification_failure_owner_lost",
            ),
        )
        for body, owner, category in cases:
            with self.subTest(category=category):
                with tempfile.TemporaryDirectory() as temporary:
                    self.root = Path(temporary)
                    self.root.chmod(0o700)
                    environment, endpoint, listener = self.private_bus()
                    try:
                        self.assert_category(
                            category,
                            lambda: verify_conflicting_notification_owner(
                                self.make_executable(body),
                                environment,
                                endpoint,
                                owner,
                                self.stable_siblings,
                                attestor=self.fake_attestor,
                            ),
                        )
                    finally:
                        listener.close()

    def test_rejects_timeout_changed_or_malformed_siblings_and_bad_executable(self) -> None:
        environment, endpoint, listener = self.private_bus()
        try:
            self.assert_category(
                "notification_failure_process_timeout",
                lambda: verify_conflicting_notification_owner(
                    self.make_executable("sleep 10\n"),
                    environment,
                    endpoint,
                    FakeOwner(),
                    self.stable_siblings,
                    timeout_seconds=0.05,
                    attestor=self.fake_attestor,
                ),
            )

            observations = iter(
                (
                    self.stable_siblings(),
                    {
                        "prismdrake-settingsd": (101, 1001),
                        "prismdrake-shell": (999, 1002),
                    },
                )
            )
            self.assert_category(
                "notification_failure_sibling_changed",
                lambda: verify_conflicting_notification_owner(
                    self.make_executable(
                        "printf '%s' 'prismdrake-notifyd: A notification service name is already owned.' >&2\n"
                        "printf '\\n' >&2\nexit 3\n"
                    ),
                    environment,
                    endpoint,
                    FakeOwner(),
                    lambda: next(observations),
                    attestor=self.fake_attestor,
                ),
            )
            self.assert_category(
                "notification_failure_sibling_invalid",
                lambda: verify_conflicting_notification_owner(
                    self.make_executable(
                        "printf '%s' 'prismdrake-notifyd: A notification service name is already owned.' >&2\n"
                        "printf '\\n' >&2\nexit 3\n"
                    ),
                    environment,
                    endpoint,
                    FakeOwner(),
                    lambda: {"prismdrake-shell": (1, 1)},
                    attestor=self.fake_attestor,
                ),
            )
            self.assert_category(
                "notification_failure_executable_invalid",
                lambda: verify_conflicting_notification_owner(
                    self.root / "missing",
                    environment,
                    endpoint,
                    FakeOwner(),
                    self.stable_siblings,
                    attestor=self.fake_attestor,
                ),
            )
        finally:
            listener.close()

    def test_diagnostic_constant_matches_production_contract(self) -> None:
        source = (
            Path(__file__).resolve().parents[2] / "src/notifyd/main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn(
            NOTIFYD_CONFLICT_DIAGNOSTIC.decode("utf-8").strip(), source
        )
        self.assertIn("ExitStatus::unavailable", source)


class EvidenceTests(PrivateFixture):
    def test_rejects_nonclosed_evidence(self) -> None:
        for value in (
            object(),
            FailureEvidence("private"),
            FailureEvidence("notification_owner_conflict_isolated", "failed"),
        ):
            with self.subTest(value=value):
                self.assert_category(
                    "notification_failure_process_failed",
                    lambda value=value: validate_failure_evidence(value),
                )


if __name__ == "__main__":
    unittest.main()
