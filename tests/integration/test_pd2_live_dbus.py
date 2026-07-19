#!/usr/bin/env python3
"""Hermetic tests for the strict PD2 live D-Bus probe."""

from __future__ import annotations

import json
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_dbus import (  # noqa: E402
    MAX_DIAGNOSTICS,
    MAX_NOTIFICATION_STATE_BYTES,
    METHODS,
    LiveDbusProbe,
    LiveDbusProbeError,
    MethodSpec,
    PrivateBusEndpoint,
    ProbeResult,
    _BoundedGioTransport,
    _validate_marshaled_reply_size,
    contract_manifest,
    inspect_private_bus_socket,
    validate_evidence,
)


SOURCE_ROOT = Path(__file__).resolve().parents[2]
INTERFACE_FILES = (
    SOURCE_ROOT / "interfaces/dbus/org.prismdrake.Settings1.xml",
    SOURCE_ROOT / "interfaces/dbus/org.prismdrake.Shell1.xml",
    SOURCE_ROOT / "interfaces/dbus/org.prismdrake.NotificationCenter1.xml",
    SOURCE_ROOT / "interfaces/dbus/org.prismdrake.Session1.xml",
)


class FakeTransport:
    def __init__(self, replies: dict[str, tuple[Any, ...]] | None = None) -> None:
        self.replies = replies or {}
        self.calls: list[tuple[Any, tuple[Any, ...]]] = []
        self.closed = False

    def call(self, spec: Any, arguments: tuple[Any, ...]) -> tuple[Any, ...]:
        self.calls.append((spec, arguments))
        reply = self.replies.get(spec.member)
        if reply is None:
            raise RuntimeError("private method failure")
        return reply

    def close(self) -> None:
        self.closed = True


def private_environment(endpoint: PrivateBusEndpoint) -> dict[str, str]:
    return {
        "DBUS_SESSION_BUS_ADDRESS": endpoint.address,
        "DISPLAY": ":93",
        "LC_ALL": "C",
        "XDG_RUNTIME_DIR": str(Path(endpoint.address.removeprefix("unix:path=")).parent),
    }


def factory(transport: FakeTransport, addresses: list[str]):
    def create(address: str) -> FakeTransport:
        addresses.append(address)
        return transport

    return create


def xml_manifest() -> frozenset[tuple[str, str, str, str, str, str]]:
    methods: set[tuple[str, str, str, str, str, str]] = set()
    services = {
        "org.prismdrake.Settings1": "org.prismdrake.Settings1",
        "org.prismdrake.SettingsSnapshot1": "org.prismdrake.Settings1",
        "org.prismdrake.Shell1": "org.prismdrake.Shell1",
        "org.prismdrake.NotificationCenter1": "org.prismdrake.NotificationCenter1",
        "org.prismdrake.Session1": "org.prismdrake.Session1",
    }
    for path in INTERFACE_FILES:
        root = ElementTree.parse(path).getroot()
        object_path = root.attrib["name"]
        for interface in root.findall("interface"):
            interface_name = interface.attrib["name"]
            for method in interface.findall("method"):
                inputs = "".join(
                    argument.attrib["type"]
                    for argument in method.findall("arg")
                    if argument.attrib.get("direction") == "in"
                )
                outputs = "".join(
                    argument.attrib["type"]
                    for argument in method.findall("arg")
                    if argument.attrib.get("direction") == "out"
                )
                methods.add(
                    (
                        services[interface_name],
                        object_path,
                        interface_name,
                        method.attrib["name"],
                        inputs,
                        outputs,
                    )
                )
    return frozenset(methods)


def blocking_helper(
    sender: Any, address: str, spec: MethodSpec, arguments: tuple[Any, ...]
) -> None:
    del sender, address, spec, arguments
    time.sleep(60)


def notification_payload(generation: int, body: str = "Fixture body") -> bytes:
    document = {
        "schema_version": 1,
        "generation": generation,
        "history_mode": "session",
        "effective_do_not_disturb": False,
        "runtime_do_not_disturb_override": None,
        "records": [
            {
                "record_id": 1,
                "content_generation": 1,
                "application_name": "Fixture application",
                "desktop_file_id": "fixture.desktop",
                "summary": "Fixture summary",
                "body": body,
                "urgency": "normal",
                "received_at_unix_ms": 1,
                "dismissible": True,
                "transient": False,
                "actions": [
                    {"action_id": "open", "label": "Open", "enabled": True}
                ],
                "notification_id": 1,
            }
        ],
    }
    return json.dumps(
        document, ensure_ascii=False, allow_nan=False, separators=(",", ":")
    ).encode("utf-8")


class LiveDbusProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="prismdrake-dbus-test-")
        self.runtime = Path(self.temporary.name)
        self.runtime.chmod(0o700)
        self.socket_path = self.runtime / "bus"
        self.socket_path.write_bytes(b"hermetic socket identity seam")
        metadata = self.socket_path.lstat()
        self.endpoint = PrivateBusEndpoint(
            f"unix:path={self.socket_path}", metadata.st_dev, metadata.st_ino
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def inspect_fixture_socket(path: Path) -> tuple[int, int] | None:
        try:
            metadata = path.lstat()
        except OSError:
            return None
        if path.is_symlink():
            return None
        return metadata.st_dev, metadata.st_ino

    def make_probe(
        self, replies: dict[str, tuple[Any, ...]] | None = None
    ) -> tuple[LiveDbusProbe, FakeTransport, list[str]]:
        transport = FakeTransport(replies)
        addresses: list[str] = []
        probe = LiveDbusProbe(
            private_environment(self.endpoint),
            endpoint=self.endpoint,
            transport_factory=factory(transport, addresses),
            socket_inspector=self.inspect_fixture_socket,
        )
        return probe, transport, addresses

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(LiveDbusProbeError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)
        self.assertIsNone(raised.exception.__cause__)
        self.assertIsNone(raised.exception.__context__)

    def test_allowlist_exactly_matches_current_experimental_xml_methods(self) -> None:
        manifest = contract_manifest()
        self.assertEqual(frozenset(entry[:6] for entry in manifest), xml_manifest())
        self.assertEqual(len(METHODS), 18)
        self.assertTrue(all(entry[6] in {2000, 6000} for entry in manifest))
        self.assertEqual(
            {entry[7] for entry in manifest},
            {spec.category for spec in METHODS.values()},
        )

    def test_uses_only_the_explicit_matching_private_address(self) -> None:
        probe, _transport, addresses = self.make_probe()
        self.assertEqual(addresses, [self.endpoint.address])
        probe.close()

        mismatched = private_environment(self.endpoint)
        wrong_endpoint = PrivateBusEndpoint(
            f"unix:path={self.runtime / 'other-bus'}",
            self.endpoint.socket_device,
            self.endpoint.socket_inode,
        )
        self.assert_category(
            "dbus_private_address_mismatch",
            lambda: LiveDbusProbe(
                mismatched,
                endpoint=wrong_endpoint,
                transport_factory=lambda _address: FakeTransport(),
                socket_inspector=self.inspect_fixture_socket,
            ),
        )

    def test_rejects_non_unix_out_of_runtime_and_escaped_bus_addresses(self) -> None:
        for address in (
            "unix:abstract=not-private-enough",
            "tcp:host=127.0.0.1",
            "unix:path=/tmp/outside/bus",
            "unix:path=relative/bus",
            f"unix:path={self.runtime}/%2e%2e/bus",
        ):
            endpoint = PrivateBusEndpoint(
                address, self.endpoint.socket_device, self.endpoint.socket_inode
            )
            environment = private_environment(endpoint)
            environment["XDG_RUNTIME_DIR"] = str(self.runtime)
            environment["DBUS_SESSION_BUS_ADDRESS"] = address
            self.assert_category(
                "dbus_private_address_invalid",
                lambda environment=environment: LiveDbusProbe(
                    environment,
                    endpoint=endpoint,
                    transport_factory=lambda _address: FakeTransport(),
                    socket_inspector=self.inspect_fixture_socket,
                ),
            )

    def test_rejects_symlinked_and_replaced_socket_identity(self) -> None:
        self.assertIsNone(inspect_private_bus_socket(self.socket_path))
        symlink_path = self.runtime / "symlink-bus"
        symlink_path.symlink_to(self.socket_path.name)
        self.assertIsNone(inspect_private_bus_socket(symlink_path))
        symlink_endpoint = PrivateBusEndpoint(
            f"unix:path={symlink_path}",
            self.endpoint.socket_device,
            self.endpoint.socket_inode,
        )
        self.assert_category(
            "dbus_private_socket_identity_invalid",
            lambda: LiveDbusProbe(
                private_environment(symlink_endpoint),
                endpoint=symlink_endpoint,
                transport_factory=lambda _address: FakeTransport(),
                socket_inspector=self.inspect_fixture_socket,
            ),
        )

        runtime_symlink = self.runtime / "runtime-link"
        runtime_symlink.symlink_to(".", target_is_directory=True)
        linked_socket = runtime_symlink / self.socket_path.name
        linked_endpoint = PrivateBusEndpoint(
            f"unix:path={linked_socket}",
            self.endpoint.socket_device,
            self.endpoint.socket_inode,
        )
        self.assert_category(
            "dbus_private_socket_identity_invalid",
            lambda: LiveDbusProbe(
                private_environment(linked_endpoint),
                endpoint=linked_endpoint,
                transport_factory=lambda _address: FakeTransport(),
                socket_inspector=self.inspect_fixture_socket,
            ),
        )

        probe, transport, _addresses = self.make_probe({"GetSessionState": (0, "idle")})
        self.socket_path.unlink()
        self.socket_path.write_bytes(b"replacement")
        self.assert_category(
            "dbus_private_socket_identity_invalid",
            lambda: probe.call("session_get_state"),
        )
        self.assertEqual(transport.calls, [])

    def test_calls_fixed_settings_method_with_exact_signature_and_timeout(self) -> None:
        probe, transport, _addresses = self.make_probe(
            {"GetCurrentProfile": ("forge", 7)}
        )
        result = probe.call("settings_get_current_profile")

        spec, arguments = transport.calls[0]
        self.assertEqual(
            (
                spec.service,
                spec.path,
                spec.interface,
                spec.member,
                spec.input_signature,
                spec.output_signature,
                spec.timeout_milliseconds,
            ),
            (
                "org.prismdrake.Settings1",
                "/org/prismdrake/Settings1",
                "org.prismdrake.Settings1",
                "GetCurrentProfile",
                "",
                "st",
                2000,
            ),
        )
        self.assertEqual(arguments, ())
        self.assertEqual(result.payload, ("forge", 7))
        self.assertEqual(
            result.evidence(),
            {"category": "settings_profile_observed", "status": "passed"},
        )
        validate_evidence(result)

    def test_validates_profile_and_snapshot_schema_before_transport(self) -> None:
        probe, transport, _addresses = self.make_probe()
        self.assert_category(
            "dbus_profile_invalid",
            lambda: probe.call("settings_request_profile_change", "private-profile"),
        )
        self.assert_category(
            "dbus_schema_version_invalid",
            lambda: probe.call("settings_get_current_snapshot", 1),
        )
        self.assertEqual(transport.calls, [])

    def test_rejects_incomplete_snapshot_and_duplicate_keys(self) -> None:
        payload = b'{"generation":9,"profile":"lustre"}'
        probe, _transport, _addresses = self.make_probe(
            {"GetCurrentSnapshot": (9, payload)}
        )
        self.assert_category(
            "dbus_json_schema_invalid",
            lambda: probe.call("settings_get_current_snapshot", 2),
        )

        duplicate = b'{"generation":9,"generation":9}'
        bad_probe, _transport, _addresses = self.make_probe(
            {"GetCurrentSnapshot": (9, duplicate)}
        )
        self.assert_category(
            "dbus_json_duplicate_key",
            lambda: bad_probe.call("settings_get_current_snapshot", 2),
        )

    def test_bounds_notification_state_and_never_serializes_it_as_evidence(self) -> None:
        private_body = "do not expose this notification body"
        payload = notification_payload(4, private_body)
        probe, _transport, _addresses = self.make_probe(
            {"GetCurrentState": (4, payload)}
        )
        result = probe.call("notification_get_current_state", 1)
        self.assertIn(private_body, result.payload[1]["records"][0]["body"])
        serialized = json.dumps(result.evidence())
        self.assertNotIn(private_body, serialized)
        self.assertNotIn("records", serialized)

        oversized_probe, _transport, _addresses = self.make_probe(
            {"GetCurrentState": (4, b"{" + b" " * MAX_NOTIFICATION_STATE_BYTES)}
        )
        self.assert_category(
            "dbus_byte_array_bound",
            lambda: oversized_probe.call("notification_get_current_state", 1),
        )

    def test_rejects_noncanonical_nonfinite_deep_and_unknown_state_json(self) -> None:
        canonical = notification_payload(4)
        cases = (
            ("dbus_json_noncanonical", canonical + b"\n"),
            (
                "dbus_json_invalid",
                canonical.replace(b'"received_at_unix_ms":1', b'"received_at_unix_ms":NaN'),
            ),
            (
                "dbus_json_schema_invalid",
                canonical.replace(b'"records":', b'"unknown":false,"records":'),
            ),
            (
                "dbus_json_schema_invalid",
                canonical.replace(b'"history_mode":"session",', b""),
            ),
            (
                "dbus_json_depth_bound",
                b'{"generation":4,"nested":' + b"[" * 65 + b"0" + b"]" * 65 + b"}",
            ),
        )
        for category, payload in cases:
            probe, _transport, _addresses = self.make_probe(
                {"GetCurrentState": (4, payload)}
            )
            self.assert_category(
                category,
                lambda probe=probe: probe.call("notification_get_current_state", 1),
            )

    def test_accepts_only_bounded_notification_mutation_identities(self) -> None:
        probe, transport, _addresses = self.make_probe(
            {"InvokeAction": (5,), "Dismiss": (6,)}
        )
        result = probe.call("notification_invoke_action", 10, 2, "open")
        self.assertEqual(result.payload, (5,))
        self.assertEqual(transport.calls[-1][1], (10, 2, "open"))
        self.assert_category(
            "dbus_unsigned_invalid",
            lambda: probe.call("notification_dismiss", 0, 2),
        )
        self.assert_category(
            "dbus_argument_oversized",
            lambda: probe.call("notification_invoke_action", 10, 2, "x" * 65),
        )

    def test_validates_candidate_diagnostic_shape_and_bound(self) -> None:
        diagnostic = ("user", "profile", "invalid_value", "profile_id", "use_default")
        probe, _transport, _addresses = self.make_probe(
            {"ValidateCandidate": (False, [diagnostic])}
        )
        result = probe.call("settings_validate_candidate", b"profile = 'nope'")
        self.assertEqual(result.payload, (False, (diagnostic,)))

        too_many = [diagnostic] * (MAX_DIAGNOSTICS + 1)
        bad_probe, _transport, _addresses = self.make_probe(
            {"ValidateCandidate": (False, too_many)}
        )
        self.assert_category(
            "dbus_diagnostic_bound",
            lambda: bad_probe.call("settings_validate_candidate", b""),
        )

    def test_requires_closed_session_state_and_transaction_pair(self) -> None:
        idle_probe, _transport, _addresses = self.make_probe(
            {"GetSessionState": (0, "idle")}
        )
        self.assertEqual(idle_probe.call("session_get_state").payload, (0, "idle"))

        for reply in ((7, "idle"), (0, "confirmed"), (7, "private-state")):
            probe, _transport, _addresses = self.make_probe(
                {"GetSessionState": reply}
            )
            self.assert_category(
                "dbus_session_state_invalid", lambda probe=probe: probe.call("session_get_state")
            )

    def test_shell_call_has_fixed_target_and_bounded_output_name(self) -> None:
        probe, transport, _addresses = self.make_probe({"FocusPanel": ()})
        result = probe.call("shell_focus_panel", "DUMMY1")
        spec, arguments = transport.calls[-1]
        self.assertEqual(spec.interface, "org.prismdrake.Shell1")
        self.assertEqual(spec.member, "FocusPanel")
        self.assertEqual(arguments, ("DUMMY1",))
        self.assertEqual(result.payload, ())
        self.assert_category(
            "dbus_argument_oversized",
            lambda: probe.call("shell_focus_panel", "x" * 256),
        )

    def test_rejects_arbitrary_operations_and_all_properties(self) -> None:
        probe, transport, _addresses = self.make_probe()
        self.assert_category(
            "dbus_operation_not_reviewed",
            lambda: probe.call("org.freedesktop.DBus.ListNames"),
        )
        self.assert_category(
            "dbus_property_not_reviewed", lambda: probe.get_property("anything")
        )
        self.assertEqual(transport.calls, [])

    def test_redacts_transport_failures_and_enforces_close(self) -> None:
        probe, transport, _addresses = self.make_probe()
        self.assert_category(
            "dbus_call_failed", lambda: probe.call("session_request_logout")
        )
        probe.close()
        self.assertTrue(transport.closed)
        probe.close()
        self.assert_category(
            "dbus_probe_closed", lambda: probe.call("session_request_logout")
        )

        def failing_factory(address: str) -> FakeTransport:
            raise RuntimeError(f"private connection text at {address}")

        self.assert_category(
            "dbus_private_connection_failed",
            lambda: LiveDbusProbe(
                private_environment(self.endpoint),
                endpoint=self.endpoint,
                transport_factory=failing_factory,
                socket_inspector=self.inspect_fixture_socket,
            ),
        )

    def test_outer_helper_bounds_connect_call_and_close_lifecycle(self) -> None:
        transport = _BoundedGioTransport(
            self.endpoint.address,
            helper=blocking_helper,
            connect_close_seconds=0.05,
        )
        spec = MethodSpec(
            "org.prismdrake.Session1",
            "/org/prismdrake/Session1",
            "org.prismdrake.Session1",
            "GetSessionState",
            "",
            "ts",
            1,
            "session_state_observed",
        )
        started = time.monotonic()
        self.assert_category("dbus_helper_timeout", lambda: transport.call(spec, ()))
        self.assertLess(time.monotonic() - started, 3.0)

    def test_bounds_marshaled_reply_before_child_process_unpack(self) -> None:
        state_spec = METHODS["notification_get_current_state"]
        _validate_marshaled_reply_size(
            state_spec, MAX_NOTIFICATION_STATE_BYTES + 65_536
        )
        self.assert_category(
            "dbus_reply_oversized",
            lambda: _validate_marshaled_reply_size(
                state_spec, MAX_NOTIFICATION_STATE_BYTES + 65_537
            ),
        )
        self.assert_category(
            "dbus_reply_size_invalid",
            lambda: _validate_marshaled_reply_size(state_spec, True),
        )

    def test_rejects_forged_evidence(self) -> None:
        forged = ProbeResult(
            "settings_get_current_profile", "private_reply_value", ("lustre", 1)
        )
        self.assert_category("dbus_evidence_invalid", lambda: validate_evidence(forged))


if __name__ == "__main__":
    unittest.main()
