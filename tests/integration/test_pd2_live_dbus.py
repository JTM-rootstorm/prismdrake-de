#!/usr/bin/env python3
"""Hermetic tests for the strict PD2 live D-Bus probe."""

from __future__ import annotations

import json
import sys
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
    ProbeResult,
    contract_manifest,
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


def private_environment() -> dict[str, str]:
    return {
        "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/prismdrake-private/bus",
        "DISPLAY": ":93",
        "LC_ALL": "C",
        "XDG_RUNTIME_DIR": "/tmp/prismdrake-private",
    }


def factory(transport: FakeTransport, addresses: list[str]):
    def create(address: str) -> FakeTransport:
        addresses.append(address)
        return transport

    return create


def xml_manifest() -> frozenset[tuple[str, str, str, str]]:
    methods: set[tuple[str, str, str, str]] = set()
    for path in INTERFACE_FILES:
        root = ElementTree.parse(path).getroot()
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
                methods.add((interface_name, method.attrib["name"], inputs, outputs))
    return frozenset(methods)


class LiveDbusProbeTests(unittest.TestCase):
    def make_probe(
        self, replies: dict[str, tuple[Any, ...]] | None = None
    ) -> tuple[LiveDbusProbe, FakeTransport, list[str]]:
        transport = FakeTransport(replies)
        addresses: list[str] = []
        probe = LiveDbusProbe(
            private_environment(), transport_factory=factory(transport, addresses)
        )
        return probe, transport, addresses

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(LiveDbusProbeError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)

    def test_allowlist_exactly_matches_current_experimental_xml_methods(self) -> None:
        self.assertEqual(contract_manifest(), xml_manifest())
        self.assertEqual(len(METHODS), 18)

    def test_uses_only_the_explicit_matching_private_address(self) -> None:
        probe, _transport, addresses = self.make_probe()
        self.assertEqual(addresses, ["unix:path=/tmp/prismdrake-private/bus"])
        probe.close()

        mismatched = private_environment()
        self.assert_category(
            "dbus_private_address_mismatch",
            lambda: LiveDbusProbe(
                mismatched,
                address="unix:path=/tmp/some-other-bus",
                transport_factory=lambda _address: FakeTransport(),
            ),
        )

    def test_rejects_non_unix_and_out_of_runtime_bus_addresses(self) -> None:
        for address in (
            "unix:abstract=not-private-enough",
            "tcp:host=127.0.0.1",
            "unix:path=/tmp/outside/bus",
            "unix:path=relative/bus",
        ):
            environment = private_environment()
            environment["DBUS_SESSION_BUS_ADDRESS"] = address
            self.assert_category(
                "dbus_private_address_invalid",
                lambda environment=environment: LiveDbusProbe(
                    environment, transport_factory=lambda _address: FakeTransport()
                ),
            )

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

    def test_validates_snapshot_json_and_outer_generation(self) -> None:
        payload = json.dumps({"generation": 9, "profile": "lustre"}).encode()
        probe, _transport, _addresses = self.make_probe(
            {"GetCurrentSnapshot": (9, payload)}
        )
        result = probe.call("settings_get_current_snapshot", 2)
        self.assertEqual(result.payload[0], 9)
        self.assertEqual(result.payload[1]["profile"], "lustre")

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
        payload = json.dumps(
            {"generation": 4, "records": [{"body": private_body}]}
        ).encode()
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

    def test_rejects_forged_evidence(self) -> None:
        forged = ProbeResult(
            "settings_get_current_profile", "private_reply_value", ("lustre", 1)
        )
        self.assert_category("dbus_evidence_invalid", lambda: validate_evidence(forged))


if __name__ == "__main__":
    unittest.main()
