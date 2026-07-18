#!/usr/bin/env python3
"""Contract tests for the strict PD1 development-demonstration evidence."""

from __future__ import annotations

import copy
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd1_demo import (
    accept_first_result_from_focused_search,
    DemoError,
    ProcessIdentity,
    capture_process_identity,
    close_identities,
    example_evidence,
    environment_has_marker,
    focused_window_id,
    named_action_nodes,
    showing_named_action_nodes,
    openbox_command,
    openbox_environment,
    parse_atom_list,
    parse_cardinals,
    parse_utf8_property,
    parse_window_property,
    process_identity_alive,
    require_distinct_process_identity,
    require_normal_ready,
    require_normal_snapshot,
    select_sole_owned_panel,
    select_current_workarea,
    validate_cleanup_identities,
    validate_evidence,
    validate_output_target,
    validate_process_group_target,
    validate_schema,
    validate_session_log,
    validate_wm_identity,
    wait_until,
    write_evidence,
)
import pd1_demo


class EvidenceContractTests(unittest.TestCase):
    def assert_rejected(self, document: object) -> None:
        with self.assertRaises(DemoError):
            validate_evidence(document)

    def test_accepts_closed_fixture_and_schema(self) -> None:
        document = example_evidence()
        validate_evidence(document)
        validate_schema(document)

    def test_accepts_closed_portage_provenance(self) -> None:
        document = example_evidence("portage_installed")
        validate_evidence(document)
        validate_schema(document)

    def test_rejects_unknown_provenance(self) -> None:
        with self.assertRaises(DemoError):
            example_evidence("local")

    def test_rejects_unknown_root_field(self) -> None:
        document = example_evidence()
        document["extra"] = True
        self.assert_rejected(document)

    def test_rejects_false_runtime_claim(self) -> None:
        document = example_evidence()
        document["scope"]["secure_session"] = True
        self.assert_rejected(document)

    def test_rejects_unclosed_limitation(self) -> None:
        document = example_evidence()
        document["limitations"].append("later")
        self.assert_rejected(document)

    def test_rejects_partial_profile_sequence(self) -> None:
        document = example_evidence()
        document["settings"]["generation_sequence"] = [1, 2]
        self.assert_rejected(document)

    def test_rejects_unverified_task_action(self) -> None:
        document = example_evidence()
        document["tasks"]["minimized_through_shell"] = True
        self.assert_rejected(document)

    def test_rejects_live_rendering_fallback_claim(self) -> None:
        document = example_evidence()
        document["fallback"]["rendered_opaque"] = True
        self.assert_rejected(document)

    def test_rejects_private_process_identity(self) -> None:
        document = example_evidence()
        document["restart"]["pid"] = 42
        self.assert_rejected(document)

    def test_rejects_private_window_identity(self) -> None:
        document = example_evidence()
        document["startup"]["xid"] = "0x123"
        self.assert_rejected(document)

    def test_rejects_private_artifact_location(self) -> None:
        document = example_evidence()
        document["environment"]["path"] = "/private"
        self.assert_rejected(document)

    def test_rejects_fixture_titles_and_notification_content(self) -> None:
        for value in (
            "Prismdrake Demo App One",
            "Prismdrake Demo App Two",
            "Prismdrake test notification",
        ):
            with self.subTest(value=value):
                document = copy.deepcopy(example_evidence())
                document["environment"]["private"] = value
                self.assert_rejected(document)

    def test_schema_rejects_semantically_invalid_fixture(self) -> None:
        document = example_evidence()
        document["startup"]["dock_type"] = False
        with self.assertRaises(DemoError):
            validate_schema(document)

    def test_serialized_fixture_stays_bounded(self) -> None:
        encoded = json.dumps(example_evidence(), ensure_ascii=True, sort_keys=True)
        self.assertLessEqual(len(encoded.encode("utf-8")), 8192)

    def test_parses_exact_xprop_contracts(self) -> None:
        self.assertEqual(
            parse_cardinals("_NET_WM_STRUT(CARDINAL) = 0, 0, 0, 64\n", "_NET_WM_STRUT"),
            [0, 0, 0, 64],
        )
        self.assertEqual(
            parse_atom_list(
                "_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DOCK\n",
                "_NET_WM_WINDOW_TYPE",
            ),
            ["_NET_WM_WINDOW_TYPE_DOCK"],
        )
        self.assertEqual(
            parse_window_property(
                "_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x20020c\n",
                "_NET_SUPPORTING_WM_CHECK",
            ),
            "0x20020c",
        )
        self.assertEqual(
            parse_utf8_property('_NET_WM_NAME(UTF8_STRING) = "Openbox"\n', "_NET_WM_NAME"),
            "Openbox",
        )

    def test_rejects_loose_xprop_contracts(self) -> None:
        invalid = (
            lambda: parse_cardinals("_NET_WM_STRUT(CARDINAL) = 0, nope\n", "_NET_WM_STRUT"),
            lambda: parse_cardinals("_NET_WM_STRUT(CARDINAL) = 0\nextra\n", "_NET_WM_STRUT"),
            lambda: parse_atom_list(
                "_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DOCK, bad\n",
                "_NET_WM_WINDOW_TYPE",
            ),
            lambda: parse_window_property(
                "_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x0\n",
                "_NET_SUPPORTING_WM_CHECK",
            ),
            lambda: parse_utf8_property('_NET_WM_NAME(STRING) = "Openbox"\n', "_NET_WM_NAME"),
        )
        for operation in invalid:
            with self.subTest(operation=operation):
                with self.assertRaises(DemoError):
                    operation()

    def test_selects_current_desktop_workarea_exactly(self) -> None:
        self.assertEqual(
            select_current_workarea([0, 0, 1280, 720, 0, 0, 1280, 656], [2], [1]),
            [0, 0, 1280, 656],
        )
        for workareas, count, current in (([0, 0, 1280, 656], [2], [0]),
                                           ([0, 0, 1280, 656], [1], [1]),
                                           ([0, 0, 1280, 656], [0], [0])):
            with self.assertRaises(DemoError):
                select_current_workarea(workareas, count, current)

    def test_rejects_safe_mode_marker_at_readiness(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary)
            instance = runtime / "instance"
            instance.mkdir()
            (instance / "ready").touch()
            require_normal_ready(runtime)
            (instance / "safe-mode").touch()
            with self.assertRaises(DemoError):
                require_normal_ready(runtime)

    def test_rejects_safe_mode_snapshot_and_restart_log(self) -> None:
        snapshot = {"theme": {"warnings": ["safe_mode_active"]}}
        with self.assertRaises(DemoError):
            require_normal_snapshot(snapshot)
        restart = ("component=prismdrake-shell severity=error "
                   "event=component_start_failed generation=none profile=none "
                   "recovery=restart_component")
        validate_session_log(restart)
        for invalid in ("", restart + "\n" + restart,
                        restart + "\nevent=fallback_selected"):
            with self.assertRaises(DemoError):
                validate_session_log(invalid)

    def test_pidfd_identity_rejects_reuse_shape(self) -> None:
        identity = capture_process_identity(os.getpid())
        try:
            self.assertTrue(process_identity_alive(identity))
            reused = ProcessIdentity(identity.process_id, identity.start_time + 1, identity.pidfd)
            self.assertFalse(process_identity_alive(reused))
            with self.assertRaises(DemoError):
                require_distinct_process_identity(identity, identity)
            require_distinct_process_identity(identity, reused)
            self.assertEqual(select_sole_owned_panel(["10"], identity.process_id, identity), "10")
            with self.assertRaises(DemoError):
                select_sole_owned_panel(["10", "11"], identity.process_id, identity)
            with self.assertRaises(DemoError):
                select_sole_owned_panel(["10"], identity.process_id + 1, identity)
        finally:
            close_identities([identity])

    def test_cleanup_identity_bounds_fail_closed(self) -> None:
        fixture = ProcessIdentity(2, 1, 0)
        with self.assertRaises(DemoError):
            validate_cleanup_identities([fixture, fixture])
        with self.assertRaises(DemoError):
            validate_cleanup_identities([ProcessIdentity(1, 1, 0)])
        with self.assertRaises(DemoError):
            validate_cleanup_identities([ProcessIdentity(index + 2, 1, 0)
                                         for index in range(17)])
        with self.assertRaises(DemoError):
            validate_process_group_target(40, 41)

    def test_cleanup_environment_marker_is_exact_and_bounded(self) -> None:
        document = b"ONE=1\0PRISMDRAKE_DEMO_TEMPORARY=/private/run\0TWO=2\0"
        marker = b"PRISMDRAKE_DEMO_TEMPORARY=/private/run"
        self.assertTrue(environment_has_marker(document, marker))
        self.assertFalse(environment_has_marker(document, marker + b"-other"))
        with self.assertRaises(DemoError):
            environment_has_marker(b"", marker)
        with self.assertRaises(DemoError):
            environment_has_marker(document, b"bad\0marker")

    def test_output_boundary_rejects_relative_directory_and_symlink(self) -> None:
        with self.assertRaises(DemoError):
            validate_output_target(Path("relative.json"))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(DemoError):
                validate_output_target(root)
            target = root / "evidence.json"
            validate_output_target(target)
            backing = root / "backing.json"
            backing.touch()
            target.symlink_to(backing)
            with self.assertRaises(DemoError):
                validate_output_target(target)

    def test_output_boundary_rejects_existing_temporary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "evidence.json"
            output.with_suffix(".json.tmp").touch()
            with self.assertRaises(DemoError):
                validate_output_target(output)

    def test_output_writer_is_atomic_private_and_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "evidence.json"
            document = example_evidence()
            write_evidence(output, document)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), document)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertFalse(output.with_suffix(".json.tmp").exists())

    def test_wm_identity_requires_self_reference_and_openbox_name(self) -> None:
        validate_wm_identity("0x20", "0x20", "Openbox")
        with self.assertRaises(DemoError):
            validate_wm_identity("0x20", "0x21", "Openbox")
        with self.assertRaises(DemoError):
            validate_wm_identity("0x20", "0x20", "OtherWM")

    def test_openbox_startup_disables_session_management(self) -> None:
        self.assertEqual(openbox_command(Path("/usr/bin/openbox")),
                         ["/usr/bin/openbox", "--sm-disable"])

    def test_openbox_environment_removes_only_private_data_dirs(self) -> None:
        source = {
            "DISPLAY": ":42",
            "HOME": "/isolated-home",
            "XDG_CONFIG_HOME": "/isolated-config",
            "XDG_DATA_DIRS": "/isolated-system-data",
            "XDG_DATA_HOME": "/isolated-user-data",
        }
        expected = dict(source)
        expected.pop("XDG_DATA_DIRS")
        self.assertEqual(openbox_environment(source), expected)
        self.assertIn("XDG_DATA_DIRS", source)

    def test_openbox_environment_accepts_missing_data_dirs_and_rejects_non_strings(self) -> None:
        source = {"DISPLAY": ":42", "XDG_CONFIG_HOME": "/isolated-config"}
        self.assertEqual(openbox_environment(source), source)
        with self.assertRaisesRegex(DemoError, "^openbox_environment_invalid$"):
            openbox_environment({"DISPLAY": 42})  # type: ignore[dict-item]

    def test_search_field_return_is_the_only_live_result_submission_path(self) -> None:
        atspi = object()
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "focused_window_id", return_value="4194305") \
                as window_probe, \
                mock.patch.object(pd1_demo, "focused", return_value=True) as focus_probe, \
                mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            accept_first_result_from_focused_search(atspi, xdotool, "4194305")
        window_probe.assert_called_once_with(xdotool)
        focus_probe.assert_called_once_with(atspi, "Search applications")
        key_sender.assert_called_once_with(xdotool, "Return")

    def test_search_field_submission_rejects_lost_focus_without_sending_key(self) -> None:
        with mock.patch.object(pd1_demo, "focused_window_id", return_value="4194305"), \
                mock.patch.object(pd1_demo, "focused", return_value=False), \
                mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^launcher_search_focus_lost$"):
                accept_first_result_from_focused_search(
                    object(), Path("/usr/bin/xdotool"), "4194305")
        key_sender.assert_not_called()

    def test_search_field_submission_rejects_other_focused_window_without_emission(self) -> None:
        with mock.patch.object(pd1_demo, "focused_window_id", return_value="4194306"), \
                mock.patch.object(pd1_demo, "focused") as focus_probe, \
                mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^launcher_window_focus_lost$"):
                accept_first_result_from_focused_search(
                    object(), Path("/usr/bin/xdotool"), "4194305")
        focus_probe.assert_not_called()
        key_sender.assert_not_called()

    def test_focused_window_identity_parser_is_exact(self) -> None:
        with mock.patch.object(pd1_demo, "run_checked", return_value=
                               mock.Mock(returncode=0, stdout="4194305\n")):
            self.assertEqual(focused_window_id(Path("/usr/bin/xdotool")), "4194305")
        for output in ("", "0\n", "4194305 extra\n"):
            with self.subTest(output=output), \
                    mock.patch.object(pd1_demo, "run_checked", return_value=
                                      mock.Mock(returncode=0, stdout=output)):
                with self.assertRaisesRegex(DemoError, "^keyboard_focus_window_invalid$"):
                    focused_window_id(Path("/usr/bin/xdotool"))

    def test_named_action_nodes_ignore_same_named_noninteractive_descendants(self) -> None:
        button = object()
        label = object()
        with mock.patch.object(pd1_demo, "named_nodes", return_value=[button, label]), \
                mock.patch.object(pd1_demo, "action_names",
                                  side_effect=(("Press",), ())):
            self.assertEqual(named_action_nodes(object(), "Demo", "Press"), [button])

    def test_showing_named_action_nodes_exclude_hidden_surface_duplicates(self) -> None:
        showing = mock.Mock()
        hidden = mock.Mock()
        showing.get_state_set.return_value.contains.side_effect = lambda state: state in (2, 3)
        hidden.get_state_set.return_value.contains.return_value = False
        atspi = mock.Mock(StateType=mock.Mock(SHOWING=2, VISIBLE=3))
        with mock.patch.object(pd1_demo, "named_action_nodes", return_value=[showing, hidden]):
            self.assertEqual(showing_named_action_nodes(atspi, "Demo", "Press"), [showing])

    def test_wait_until_surfaces_last_safe_closed_demo_error(self) -> None:
        with mock.patch.object(pd1_demo.time, "monotonic", side_effect=(0.0, 0.0, 1.0)), \
                mock.patch.object(pd1_demo.time, "sleep"):
            with self.assertRaisesRegex(DemoError, "^x11_property_unavailable$"):
                wait_until(lambda: (_ for _ in ()).throw(
                    DemoError("x11_property_unavailable")), "window_manager_start_timeout",
                    timeout=0.5)

    def test_wait_until_recovers_from_transient_closed_error(self) -> None:
        attempts = iter((DemoError("x11_property_unavailable"), "ready"))

        def predicate() -> str:
            result = next(attempts)
            if isinstance(result, DemoError):
                raise result
            return result

        with mock.patch.object(pd1_demo.time, "sleep"):
            self.assertEqual(wait_until(predicate, "window_manager_start_timeout"), "ready")

    def test_wait_until_does_not_surface_unsafe_error_text(self) -> None:
        with mock.patch.object(pd1_demo.time, "monotonic", side_effect=(0.0, 0.0, 1.0)), \
                mock.patch.object(pd1_demo.time, "sleep"):
            with self.assertRaisesRegex(DemoError, "^window_manager_start_timeout$"):
                wait_until(lambda: (_ for _ in ()).throw(
                    DemoError("private output: /home/user")), "window_manager_start_timeout",
                    timeout=0.5)

    def test_main_reports_closed_demo_error_code(self) -> None:
        error = io.StringIO()
        with mock.patch.object(pd1_demo, "parse_arguments",
                               side_effect=DemoError("workarea_timeout")), \
                contextlib.redirect_stderr(error):
            self.assertEqual(pd1_demo.main(), 2)
        self.assertEqual(
            error.getvalue(),
            "PD1 development demonstration failed closed: workarea_timeout\n",
        )


if __name__ == "__main__":
    unittest.main()
