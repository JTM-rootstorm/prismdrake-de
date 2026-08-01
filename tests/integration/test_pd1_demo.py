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
    APP_TITLES,
    accept_first_result_from_focused_search,
    capture_exact_direct_children,
    current_sole_owned_panel,
    DemoError,
    ProcessIdentity,
    capture_process_identity,
    children,
    close_identities,
    close_test_notification,
    example_evidence,
    environment_has_marker,
    focus_task_from_panel,
    focused_window_id,
    has_showing_focused_node,
    named_action_nodes,
    showing_named_action_nodes,
    openbox_command,
    openbox_environment,
    open_task_context_menu_from_pointer,
    parse_atom_list,
    parse_cardinals,
    parse_direct_child_ids,
    parse_optional_atom_list,
    parse_utf8_property,
    parse_window_property,
    process_identity_alive,
    reactivate_focused_task_from_panel,
    require_distinct_process_identity,
    require_normal_ready,
    require_normal_snapshot,
    send_test_notification,
    send_focused_shell_action_key,
    send_focused_window_action_key,
    select_sole_owned_panel,
    select_current_workarea,
    session_child_command,
    task_screen_center,
    task_reactivation_diagnostic,
    validate_cleanup_identities,
    validate_artifact_provenance,
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

    def test_build_tree_provenance_rejects_installed_attestation(self) -> None:
        arguments = mock.Mock(
            artifact_provenance="build_tree", installed_artifact_attestation=None,
        )
        validate_artifact_provenance(arguments)
        arguments.installed_artifact_attestation = Path("/private/attestation.json")
        with self.assertRaisesRegex(
            DemoError, "^build_tree_attestation_unexpected$",
        ):
            validate_artifact_provenance(arguments)

    def test_installed_provenance_requires_valid_bound_attestation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "attestation.json"
            gentoo_tests = Path(__file__).resolve().parents[1] / "gentoo"
            sys.path.insert(0, str(gentoo_tests))
            try:
                from installed_artifact_attestation import example_attestation_document
            finally:
                sys.path.remove(str(gentoo_tests))
            attestation = example_attestation_document()
            path.write_text(json.dumps(attestation), encoding="utf-8")
            arguments = mock.Mock(
                artifact_provenance="portage_installed",
                installed_artifact_attestation=path,
                session=Path("/usr/bin/prismdrake-session"),
                settingsd=Path("/usr/bin/prismdrake-settingsd"),
                shell=Path("/usr/bin/prismdrake-shell"),
            )
            hashes = {
                "/usr/bin/prismdrake-session": "c" * 64,
                "/usr/bin/prismdrake-settingsd": "d" * 64,
                "/usr/bin/prismdrake-shell": "e" * 64,
                str(Path(pd1_demo.__file__).resolve()): "7" * 64,
            }
            with mock.patch.object(
                pd1_demo, "sha256_regular_file", side_effect=lambda value: hashes[str(value)],
            ):
                validate_artifact_provenance(arguments)

            arguments.shell = Path("/usr/local/bin/prismdrake-shell")
            with self.assertRaisesRegex(DemoError, "^installed_executable_path_mismatch$"):
                validate_artifact_provenance(arguments)

    def test_installed_provenance_rejects_missing_attestation(self) -> None:
        arguments = mock.Mock(
            artifact_provenance="portage_installed",
            installed_artifact_attestation=None,
        )
        with self.assertRaisesRegex(DemoError, "^installed_artifact_attestation_required$"):
            validate_artifact_provenance(arguments)

    def test_installed_attestation_is_forwarded_to_session_child(self) -> None:
        arguments = mock.Mock(
            artifact_provenance="portage_installed",
            installed_artifact_attestation=Path("/private/attestation.json"),
            dbus_run_session=Path("/usr/bin/dbus-run-session"),
            accessible_config=Path("/fixture/config.toml"),
            gdbus=Path("/usr/bin/gdbus"),
            notifyd=None,
            openbox=Path("/usr/bin/openbox"),
            output=Path("/private/demo.json"),
            session=Path("/usr/bin/prismdrake-session"),
            settingsd=Path("/usr/bin/prismdrake-settingsd"),
            shell=Path("/usr/bin/prismdrake-shell"),
            test_app=Path("/fixture/app"),
            xdotool=Path("/usr/bin/xdotool"),
            xprop=Path("/usr/bin/xprop"),
        )
        command = session_child_command(arguments)
        option = command.index("--installed-artifact-attestation")
        self.assertEqual(command[option + 1], "/private/attestation.json")
        notifyd = command.index("--notifyd")
        self.assertEqual(command[notifyd + 1], "/usr/bin/prismdrake-notifyd")

        arguments.notifyd = Path("/fixture/prismdrake-notifyd")
        command = session_child_command(arguments)
        notifyd = command.index("--notifyd")
        self.assertEqual(command[notifyd + 1], "/fixture/prismdrake-notifyd")

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
        document["settings"]["owner_epoch_generations"] = [[1, 2, 3]]
        self.assert_rejected(document)

    def test_rejects_false_or_missing_settings_restart_claims(self) -> None:
        for field in (
            "settingsd_restarted",
            "settings_owner_gap_observed",
            "shell_preserved_during_settings_restart",
            "presentation_epoch_rebuilt",
        ):
            for mutation in ("false", "missing"):
                with self.subTest(field=field, mutation=mutation):
                    document = example_evidence()
                    if mutation == "false":
                        document["restart"][field] = False
                    else:
                        del document["restart"][field]
                    self.assert_rejected(document)
                    with self.assertRaises(DemoError):
                        validate_schema(document)

    def test_rejects_false_or_missing_task_action_claims(self) -> None:
        for field in (
            "minimization_through_shell",
            "reactivation_through_shell",
            "close_through_shell",
        ):
            for mutation in ("false", "missing"):
                with self.subTest(field=field, mutation=mutation):
                    document = example_evidence()
                    if mutation == "false":
                        document["tasks"][field] = False
                    else:
                        del document["tasks"][field]
                    self.assert_rejected(document)

    def test_schema_rejects_false_or_missing_task_action_claims(self) -> None:
        for field in (
            "minimization_through_shell",
            "reactivation_through_shell",
            "close_through_shell",
        ):
            for mutation in ("false", "missing"):
                with self.subTest(field=field, mutation=mutation):
                    document = example_evidence()
                    if mutation == "false":
                        document["tasks"][field] = False
                    else:
                        del document["tasks"][field]
                    with self.assertRaises(DemoError):
                        validate_schema(document)

    def test_rejects_false_or_missing_task_generic_icon_semantics(self) -> None:
        for mutation in ("false", "missing"):
            with self.subTest(mutation=mutation):
                document = example_evidence()
                if mutation == "false":
                    document["fallback"]["task_generic_icon_semantics"] = False
                else:
                    del document["fallback"]["task_generic_icon_semantics"]
                self.assert_rejected(document)

    def test_schema_rejects_false_or_missing_task_generic_icon_semantics(self) -> None:
        for mutation in ("false", "missing"):
            with self.subTest(mutation=mutation):
                document = example_evidence()
                if mutation == "false":
                    document["fallback"]["task_generic_icon_semantics"] = False
                else:
                    del document["fallback"]["task_generic_icon_semantics"]
                with self.assertRaises(DemoError):
                    validate_schema(document)

    def test_rejects_stale_schema_version(self) -> None:
        document = example_evidence()
        document["schema_version"] = 1
        self.assert_rejected(document)
        with self.assertRaises(DemoError):
            validate_schema(document)

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
            parse_optional_atom_list("_NET_WM_STATE(ATOM) =\n", "_NET_WM_STATE"),
            [],
        )
        self.assertEqual(
            parse_optional_atom_list("_NET_WM_STATE(ATOM) = \n", "_NET_WM_STATE"),
            [],
        )
        self.assertEqual(
            parse_optional_atom_list(
                "_NET_WM_STATE(ATOM) = _NET_WM_STATE_HIDDEN, _NET_WM_STATE_ABOVE\n",
                "_NET_WM_STATE",
            ),
            ["_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_ABOVE"],
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
            lambda: parse_optional_atom_list(
                "_NET_WM_STATE(ATOM) = _NET_WM_STATE_HIDDEN, _NET_WM_STATE_HIDDEN\n",
                "_NET_WM_STATE",
            ),
            lambda: parse_optional_atom_list(
                "_NET_WM_STATE(STRING) = _NET_WM_STATE_HIDDEN\n", "_NET_WM_STATE",
            ),
            lambda: parse_optional_atom_list(
                "_NET_WM_STATE(ATOM) =  \n", "_NET_WM_STATE",
            ),
            lambda: parse_optional_atom_list(
                f"_NET_WM_STATE(ATOM) = _{('A' * 128)}\n", "_NET_WM_STATE",
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
        shell_restart = ("component=prismdrake-shell severity=error "
                         "event=component_start_failed generation=none profile=none "
                         "recovery=restart_component")
        settings_restart = ("component=prismdrake-settingsd severity=error "
                            "event=component_start_failed generation=none profile=none "
                            "recovery=restart_component")
        restart_log = shell_restart + "\n" + settings_restart
        validate_session_log(restart_log)
        for invalid in ("", shell_restart, settings_restart,
                        restart_log + "\n" + shell_restart,
                        restart_log + "\n" + settings_restart,
                        restart_log + "\nevent=fallback_selected"):
            with self.assertRaises(DemoError):
                validate_session_log(invalid)

    def test_direct_child_parser_is_exact_and_bounded(self) -> None:
        self.assertEqual(parse_direct_child_ids("42 43 \n"), [42, 43])
        self.assertEqual(parse_direct_child_ids(""), [])
        invalid = (
            "1 ",
            "42 42 ",
            "42,43",
            "42 extra",
            "0 ",
            "2147483648 ",
            " ",
            "\n\n",
            " ".join(str(value) for value in range(2, 19)),
        )
        for document in invalid:
            with self.subTest(document=document), self.assertRaises(DemoError):
                parse_direct_child_ids(document)

    def test_exact_direct_children_bind_expected_executables(self) -> None:
        expected = {
            "settingsd": Path("/expected/prismdrake-settingsd"),
            "shell": Path("/expected/prismdrake-shell"),
        }
        resolved = {
            "/expected/prismdrake-settingsd": Path("/resolved/prismdrake-settingsd"),
            "/expected/prismdrake-shell": Path("/resolved/prismdrake-shell"),
            "/proc/42/exe": Path("/resolved/prismdrake-settingsd"),
            "/proc/43/exe": Path("/resolved/prismdrake-shell"),
        }

        def resolve(path: Path, strict: bool = False) -> Path:
            self.assertTrue(strict)
            return resolved[str(path)]

        identities = {
            42: ProcessIdentity(42, 100, 102),
            43: ProcessIdentity(43, 101, 103),
        }
        with mock.patch.object(Path, "read_text", return_value="42 43 "), \
                mock.patch.object(Path, "resolve", autospec=True, side_effect=resolve), \
                mock.patch.object(pd1_demo, "capture_process_identity",
                                  side_effect=lambda process_id: identities[process_id]):
            observed = capture_exact_direct_children(41, expected)
        self.assertEqual(observed, {
            "settingsd": identities[42],
            "shell": identities[43],
        })

    def test_exact_direct_children_reject_unknown_or_incomplete_set(self) -> None:
        expected = {
            "settingsd": Path("/expected/prismdrake-settingsd"),
            "shell": Path("/expected/prismdrake-shell"),
        }

        def resolve(path: Path, strict: bool = False) -> Path:
            self.assertTrue(strict)
            if str(path).startswith("/expected/"):
                return Path(str(path).replace("/expected/", "/resolved/"))
            return Path("/resolved/unexpected")

        with mock.patch.object(Path, "read_text", return_value="42 "), \
                mock.patch.object(Path, "resolve", autospec=True, side_effect=resolve):
            with self.assertRaisesRegex(DemoError, "^direct_child_executable_invalid$"):
                capture_exact_direct_children(41, expected)

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

    def test_current_panel_rebinds_only_to_live_sole_shell_owner(self) -> None:
        xdotool = Path("/usr/bin/xdotool")
        identity = ProcessIdentity(42, 100, 102)
        valid_result = mock.Mock(returncode=0, stdout="42\n")
        with mock.patch.object(pd1_demo, "window_ids", return_value=["10"]), \
                mock.patch.object(pd1_demo, "run_checked", return_value=valid_result) as runner, \
                mock.patch.object(pd1_demo, "process_identity_alive", return_value=True):
            self.assertEqual(current_sole_owned_panel(xdotool, identity), "10")
        runner.assert_called_once_with([str(xdotool), "getwindowpid", "10"])

        with mock.patch.object(pd1_demo, "window_ids", return_value=["10", "11"]), \
                mock.patch.object(pd1_demo, "run_checked") as runner:
            self.assertIsNone(current_sole_owned_panel(xdotool, identity))
        runner.assert_not_called()

        invalid_results = (
            mock.Mock(returncode=1, stdout=""),
            mock.Mock(returncode=0, stdout="42 extra\n"),
            mock.Mock(returncode=0, stdout="43\n"),
        )
        for result in invalid_results:
            with self.subTest(returncode=result.returncode, stdout=result.stdout), \
                    mock.patch.object(pd1_demo, "window_ids", return_value=["10"]), \
                    mock.patch.object(pd1_demo, "run_checked", return_value=result), \
                    mock.patch.object(pd1_demo, "process_identity_alive", return_value=True):
                self.assertIsNone(current_sole_owned_panel(xdotool, identity))

        with mock.patch.object(pd1_demo, "window_ids", return_value=["10"]), \
                mock.patch.object(pd1_demo, "run_checked", return_value=valid_result), \
                mock.patch.object(pd1_demo, "process_identity_alive", return_value=False):
            self.assertIsNone(current_sole_owned_panel(xdotool, identity))

    def test_keyboard_task_navigation_rebinds_replaced_panel(self) -> None:
        atspi = object()
        xdotool = Path("/usr/bin/xdotool")

        def wait(predicate: object, code: str, **_kwargs: object) -> object:
            if code == "launcher_window_timeout":
                return "20"
            if code in {
                    "launcher_search_focus_timeout", "launcher_dismiss_timeout",
                    "task_keyboard_focus_timeout"}:
                return True
            if code in {"panel_keyboard_return_timeout", "shell_panel_owner_timeout"}:
                return predicate()
            self.fail(f"unexpected wait code: {code}")

        with mock.patch.object(pd1_demo, "current_shell_panel",
                               side_effect=("10", "30", "30")) as panel_probe, \
                mock.patch.object(pd1_demo, "press", return_value=True), \
                mock.patch.object(pd1_demo, "wait_until", side_effect=wait), \
                mock.patch.object(pd1_demo, "focused_window_id", side_effect=("20", "30")), \
                mock.patch.object(pd1_demo, "send_focused_key"), \
                mock.patch.object(pd1_demo, "has_showing_focused_action", return_value=True), \
                mock.patch.object(pd1_demo, "send_focused_window_action_key") as key_sender:
            observed = focus_task_from_panel(
                atspi, xdotool, "10", ProcessIdentity(42, 100, 102), APP_TITLES[0],
            )

        self.assertEqual(observed, "30")
        self.assertEqual(panel_probe.call_count, 3)
        key_sender.assert_called_once_with(
            atspi, xdotool, "30", ProcessIdentity(42, 100, 102),
            "Open applications", "Tab",
        )

    def test_reactivation_reuses_restored_task_focus_without_launcher_churn(self) -> None:
        atspi = object()
        xdotool = Path("/usr/bin/xdotool")
        identity = ProcessIdentity(42, 100, 102)

        def wait(predicate: object, code: str, **_kwargs: object) -> object:
            self.assertEqual(code, "task_reactivation_focus_timeout")
            return predicate()

        with mock.patch.object(pd1_demo, "current_shell_panel", return_value="30"), \
                mock.patch.object(pd1_demo, "focused_window_id", return_value="30"), \
                mock.patch.object(pd1_demo, "has_showing_focused_action", return_value=True), \
                mock.patch.object(pd1_demo, "wait_until", side_effect=wait), \
                mock.patch.object(pd1_demo, "send_focused_window_action_key") as key_sender, \
                mock.patch.object(pd1_demo, "press") as launcher_press:
            observed = reactivate_focused_task_from_panel(
                atspi, xdotool, "10", identity, APP_TITLES[0],
            )

        self.assertEqual(observed, "30")
        key_sender.assert_called_once_with(
            atspi, xdotool, "30", identity, APP_TITLES[0], "Return",
        )
        launcher_press.assert_not_called()

    def test_reactivation_failure_diagnostic_is_redacted_and_categorized(self) -> None:
        log = "\n".join((
            "The checked task request path is not current.",
            "The checked task request belongs to a stale refresh token.",
            "The checked task request has no current validated refresh epoch.",
            "The checked task request belongs to a future refresh token.",
            "The checked task request target is stale on the X11 worker.",
            "The checked task request target is absent from the latest complete observation.",
            "The window manager rejected a current task request.",
        ))
        diagnostic = task_reactivation_diagnostic(True, False, True, True, False, log)
        self.assertEqual(
            diagnostic,
            "PD1 task reactivation diagnostic: active_target=1 hidden_target=0 "
            "focused_panel=1 focused_task=1 minimized_semantic=0 path_rejected=1 "
            "token_stale=1 epoch_invalid=1 token_future=1 target_stale=1 "
            "target_absent=1 wm_rejected=1",
        )
        for private_field in (
                "display=", "bus_address=", "process_id=", "window_id=", "title="):
            self.assertNotIn(private_field, diagnostic)

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

    def test_focused_window_action_rejects_wrong_x_focus_without_emission(self) -> None:
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "require_shell_owner"), \
                mock.patch.object(pd1_demo, "focused_window_id", return_value="4194306"), \
                mock.patch.object(pd1_demo, "showing_focused_action_node") as focus_probe, \
                mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^action_window_focus_lost$"):
                send_focused_window_action_key(
                    object(), xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    APP_TITLES[0], "shift+F10",
                )
        focus_probe.assert_not_called()
        key_sender.assert_not_called()

    def test_focused_window_action_rejects_lost_atspi_focus_without_emission(self) -> None:
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "require_shell_owner"), \
                mock.patch.object(pd1_demo, "focused_window_id", return_value="4194305"), \
                mock.patch.object(
                    pd1_demo, "showing_focused_action_node",
                    side_effect=DemoError("atspi_focused_control_identity"),
                ), mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^atspi_focused_control_identity$"):
                send_focused_window_action_key(
                    object(), xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    APP_TITLES[0], "shift+F10",
                )
        key_sender.assert_not_called()

    def test_focused_window_action_rejects_atspi_owner_drift_without_emission(self) -> None:
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "window_process_id", return_value=100), \
                mock.patch.object(pd1_demo, "process_identity_alive", return_value=True), \
                mock.patch.object(pd1_demo, "process_application_id", return_value=99), \
                mock.patch.object(pd1_demo, "focused_window_id") as focus_probe, \
                mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^atspi_owner_identity_invalid$"):
                send_focused_window_action_key(
                    object(), xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    APP_TITLES[0], "shift+F10",
                )
        focus_probe.assert_not_called()
        key_sender.assert_not_called()

    def test_focused_shell_action_rejects_wrong_owner_without_emission(self) -> None:
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "window_process_id", return_value=99), \
                mock.patch.object(pd1_demo, "window_ids", return_value=["4194305"]), \
                mock.patch.object(pd1_demo, "wait_until",
                                  side_effect=lambda predicate, *_args, **_kwargs: predicate()), \
                mock.patch.object(pd1_demo, "showing_focused_action_node") as focus_probe, \
                mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^panel_owner_identity_invalid$"):
                send_focused_shell_action_key(
                    object(), xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    f"Minimize {APP_TITLES[0]}", "Return",
                )
        focus_probe.assert_not_called()
        key_sender.assert_not_called()

    def test_focused_shell_action_rejects_lost_atspi_focus_without_emission(self) -> None:
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "require_shell_owner"), \
                mock.patch.object(pd1_demo, "window_ids", return_value=["4194305"]), \
                mock.patch.object(pd1_demo, "wait_until",
                                  side_effect=lambda predicate, *_args, **_kwargs: predicate()), \
                mock.patch.object(pd1_demo, "focused_window_id", return_value="4194305"), \
                mock.patch.object(
                    pd1_demo, "showing_focused_action_node",
                    side_effect=DemoError("atspi_focused_control_identity"),
                ), mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            with self.assertRaisesRegex(DemoError, "^atspi_focused_control_identity$"):
                send_focused_shell_action_key(
                    object(), xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    f"Minimize {APP_TITLES[0]}", "Return",
                )
        key_sender.assert_not_called()

    def test_notification_fixture_uses_typed_standard_dbus_calls(self) -> None:
        connection = mock.Mock()
        connection.call_sync.side_effect = (
            mock.Mock(unpack=mock.Mock(return_value=(17,))),
            mock.Mock(unpack=mock.Mock(return_value=())),
        )
        gio = mock.Mock(DBusCallFlags=mock.Mock(NO_AUTO_START=4))
        glib = mock.Mock()
        glib.Variant.side_effect = lambda signature, value: (signature, value)
        glib.VariantType.new.side_effect = lambda signature: signature

        notification_id = send_test_notification(connection, gio, glib)
        close_test_notification(connection, gio, glib, notification_id)
        self.assertEqual(notification_id, 17)
        notify = connection.call_sync.call_args_list[0].args
        self.assertEqual(notify[0:4], (
            "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "Notify",
        ))
        self.assertEqual(notify[5:8], ("(u)", 4, 2_000))
        close = connection.call_sync.call_args_list[1].args
        self.assertEqual(close[0:4], (
            "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "CloseNotification",
        ))
        self.assertEqual(close[4], ("(u)", (17,)))
        self.assertEqual(close[5:8], ("()", 4, 2_000))

    def test_notification_fixture_rejects_malformed_identities_before_close(self) -> None:
        connection = mock.Mock()
        connection.call_sync.return_value.unpack.return_value = (0,)
        gio = mock.Mock(DBusCallFlags=mock.Mock(NO_AUTO_START=4))
        glib = mock.Mock()
        with self.assertRaisesRegex(DemoError, "^notification_identity_invalid$"):
            send_test_notification(connection, gio, glib)
        for value in (False, 0, -1, 0x100000000):
            connection.reset_mock()
            with self.subTest(value=value):
                with self.assertRaisesRegex(DemoError, "^notification_identity_invalid$"):
                    close_test_notification(connection, gio, glib, value)
                connection.call_sync.assert_not_called()

    def test_task_pointer_center_is_bounded_to_panel_extents(self) -> None:
        rectangle = mock.Mock(x=100, y=660, width=200, height=48)
        component = mock.Mock()
        component.get_extents.return_value = rectangle
        node = mock.Mock()
        node.get_component_iface.return_value = component
        atspi = mock.Mock(CoordType=mock.Mock(SCREEN=7))
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "showing_named_action_nodes", return_value=[node]), \
                mock.patch.object(pd1_demo, "geometry", return_value=[0, 656, 1280, 64]):
            self.assertEqual(
                task_screen_center(atspi, xdotool, "4194305", APP_TITLES[1]),
                (200, 684),
            )
        component.get_extents.assert_called_once_with(7)

    def test_task_pointer_center_rejects_outside_panel_before_emission(self) -> None:
        rectangle = mock.Mock(x=100, y=650, width=200, height=48)
        component = mock.Mock()
        component.get_extents.return_value = rectangle
        node = mock.Mock()
        node.get_component_iface.return_value = component
        atspi = mock.Mock(CoordType=mock.Mock(SCREEN=7))
        with mock.patch.object(pd1_demo, "showing_named_action_nodes", return_value=[node]), \
                mock.patch.object(pd1_demo, "geometry", return_value=[0, 656, 1280, 64]), \
                mock.patch.object(pd1_demo, "run_checked") as pointer_sender:
            with self.assertRaisesRegex(DemoError, "^task_extents_outside_panel$"):
                task_screen_center(
                    atspi, Path("/usr/bin/xdotool"), "4194305", APP_TITLES[1],
                )
        pointer_sender.assert_not_called()

    def test_task_pointer_center_rejects_coordinates_outside_fixed_display(self) -> None:
        rectangle = mock.Mock(x=-5, y=660, width=200, height=48)
        component = mock.Mock()
        component.get_extents.return_value = rectangle
        node = mock.Mock()
        node.get_component_iface.return_value = component
        atspi = mock.Mock(CoordType=mock.Mock(SCREEN=7))
        with mock.patch.object(pd1_demo, "showing_named_action_nodes", return_value=[node]), \
                mock.patch.object(pd1_demo, "geometry", return_value=[-10, 656, 1290, 64]), \
                mock.patch.object(pd1_demo, "run_checked") as pointer_sender:
            with self.assertRaisesRegex(DemoError, "^atspi_extents_invalid$"):
                task_screen_center(
                    atspi, Path("/usr/bin/xdotool"), "4194305", APP_TITLES[1],
                )
        pointer_sender.assert_not_called()

    def test_pointer_context_open_uses_exact_center_and_validates_focus(self) -> None:
        atspi = object()
        xdotool = Path("/usr/bin/xdotool")
        result = mock.Mock(returncode=0)
        with mock.patch.object(pd1_demo, "window_process_id", return_value=100), \
                mock.patch.object(pd1_demo, "process_identity_alive", return_value=True), \
                mock.patch.object(pd1_demo, "process_application_id", return_value=100), \
                mock.patch.object(pd1_demo, "window_ids", return_value=["4194305"]), \
                mock.patch.object(pd1_demo, "wait_until",
                                  side_effect=lambda predicate, *_args, **_kwargs: predicate()), \
                mock.patch.object(pd1_demo, "task_screen_center", return_value=(200, 684)), \
                mock.patch.object(pd1_demo, "run_checked", return_value=result) as pointer_sender, \
                mock.patch.object(pd1_demo, "require_task_context_menu_focus") as focus_probe:
            open_task_context_menu_from_pointer(
                atspi, xdotool, "4194305", ProcessIdentity(100, 1, 0), APP_TITLES[1],
            )
        pointer_sender.assert_called_once_with([
            str(xdotool), "mousemove", "--sync", "200", "684", "click", "3",
        ])
        focus_probe.assert_called_once_with(
            atspi, xdotool, "4194305", ProcessIdentity(100, 1, 0),
            APP_TITLES[1], False,
        )

    def test_keyboard_context_walks_grouped_application_actions_to_minimize(self) -> None:
        atspi = object()
        xdotool = Path("/usr/bin/xdotool")
        title = APP_TITLES[0]
        focused_actions = iter((
            f"Open a new {title} window",
            f"Pin {title}",
            f"Minimize {title}",
        ))
        with mock.patch.object(pd1_demo, "current_shell_panel", return_value="4194305"), \
                mock.patch.object(
                    pd1_demo, "wait_until",
                    side_effect=lambda predicate, *_args, **_kwargs: predicate(),
                ), mock.patch.object(
                    pd1_demo, "focused_task_context_action",
                    side_effect=lambda *_args: next(focused_actions),
                ), mock.patch.object(
                    pd1_demo, "focused_window_id", return_value="4194305",
                ), mock.patch.object(pd1_demo, "send_focused_key") as key_sender:
            self.assertEqual(
                pd1_demo.require_task_context_menu_focus(
                    atspi, xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    title, True,
                ),
                "4194305",
            )
        self.assertEqual(
            key_sender.call_args_list,
            [mock.call(xdotool, "Tab"), mock.call(xdotool, "Tab")],
        )

    def test_pointer_context_requires_visible_action_without_keyboard_navigation(self) -> None:
        atspi = object()
        xdotool = Path("/usr/bin/xdotool")
        with mock.patch.object(pd1_demo, "current_shell_panel", return_value="4194305"), \
                mock.patch.object(
                    pd1_demo, "wait_until",
                    side_effect=lambda predicate, *_args, **_kwargs: predicate(),
                ), mock.patch.object(
                    pd1_demo, "showing_named_action_nodes", return_value=[object()],
                ), mock.patch.object(
                    pd1_demo, "focused_task_context_action",
                ) as focus_probe, mock.patch.object(
                    pd1_demo, "send_focused_key",
                ) as key_sender:
            self.assertEqual(
                pd1_demo.require_task_context_menu_focus(
                    atspi, xdotool, "4194305", ProcessIdentity(100, 1, 0),
                    APP_TITLES[1], False,
                ),
                "4194305",
            )
        focus_probe.assert_not_called()
        key_sender.assert_not_called()

    def test_task_context_diagnostic_is_bounded_and_redacts_atspi_text(self) -> None:
        title = APP_TITLES[0]
        action_name = f"Minimize {title}"
        state_type = mock.Mock(FOCUSED=1, SHOWING=2, VISIBLE=3)
        atspi = mock.Mock(StateType=state_type)

        target = mock.Mock()
        target.get_state_set.return_value.contains.side_effect = (
            lambda state: state in (state_type.SHOWING, state_type.VISIBLE)
        )
        task_focus = mock.Mock()
        task_focus.get_name.return_value = title
        task_focus.get_state_set.return_value.contains.side_effect = (
            lambda state: state == state_type.FOCUSED
        )
        private_focus = mock.Mock()
        private_focus.get_name.return_value = "private focused accessible name"
        private_focus.get_role_name.side_effect = AssertionError("role must not be read")
        private_focus.get_state_set.return_value.contains.side_effect = (
            lambda state: state == state_type.FOCUSED
        )

        def named(_atspi: object, name: str) -> list[object]:
            if name == action_name:
                return [target]
            if name == "Minimize":
                return [object()]
            return []

        def showing(_atspi: object, name: str, _action: str) -> list[object]:
            return [target] if name == action_name else []

        with mock.patch.object(pd1_demo, "named_nodes", side_effect=named), \
                mock.patch.object(pd1_demo, "named_action_nodes", return_value=[target]), \
                mock.patch.object(pd1_demo, "showing_named_action_nodes",
                                  side_effect=showing), \
                mock.patch.object(pd1_demo, "application", return_value=object()), \
                mock.patch.object(pd1_demo, "walk",
                                  return_value=[task_focus, private_focus]), \
                mock.patch.object(pd1_demo, "action_names",
                                  return_value=["Press", "private action name"]), \
                mock.patch.object(pd1_demo, "task_accessible_description",
                                  return_value="Window actions shown. private description"):
            diagnostic = pd1_demo.task_context_menu_diagnostic(
                atspi, title, action_name,
            )

        self.assertEqual(
            diagnostic,
            "PD1 task-menu diagnostic: named_count=1 interactive_count=1 "
            "showing_count=1 visible_count=1 focused_count=0 bare_counts=1,0 "
            "menu_marker=1 reviewed_states=0/0,0/0,0/0,1/0,0/0 "
            "focus_buckets=2,1,0,0,1 action_profile=2,1",
        )
        self.assertLessEqual(len(diagnostic), 512)
        for private_text in (
                "private focused accessible name", "private action name",
                "private description"):
            self.assertNotIn(private_text, diagnostic)
        private_focus.get_role_name.assert_not_called()

    def test_task_context_diagnostic_degrades_failed_probes_to_closed_markers(self) -> None:
        atspi = mock.Mock()
        with mock.patch.object(pd1_demo, "named_nodes",
                               side_effect=RuntimeError("private path")), \
                mock.patch.object(pd1_demo, "named_action_nodes",
                                  side_effect=RuntimeError("private action")), \
                mock.patch.object(pd1_demo, "showing_named_action_nodes",
                                  side_effect=RuntimeError("private state")), \
                mock.patch.object(pd1_demo, "application",
                                  side_effect=RuntimeError("private owner")), \
                mock.patch.object(pd1_demo, "task_accessible_description",
                                  side_effect=RuntimeError("private description")):
            diagnostic = pd1_demo.task_context_menu_diagnostic(
                atspi, APP_TITLES[0], f"Minimize {APP_TITLES[0]}",
            )

        self.assertEqual(
            diagnostic,
            "PD1 task-menu diagnostic: named_count=? interactive_count=? "
            "showing_count=? visible_count=? focused_count=? bare_counts=?,? "
            "menu_marker=? reviewed_states=?/?,?/?,?/?,?/?,?/? "
            "focus_buckets=?,?,?,?,? action_profile=?,?",
        )
        self.assertNotIn("private", diagnostic)

    def test_context_diagnostic_failure_does_not_mask_original_demo_error(self) -> None:
        error = io.StringIO()
        with mock.patch.object(
                    pd1_demo, "wait_until",
                    side_effect=("4194305", DemoError("task_context_menu_focus_timeout")),
                ), mock.patch.object(
                    pd1_demo, "task_context_menu_diagnostic",
                    side_effect=RuntimeError("private diagnostic failure"),
                ), contextlib.redirect_stderr(error):
            with self.assertRaisesRegex(
                    DemoError, "^task_context_menu_focus_timeout$"):
                pd1_demo.require_task_context_menu_focus(
                    object(), Path("/usr/bin/xdotool"), "4194305",
                    ProcessIdentity(100, 1, 0), APP_TITLES[0], True,
                )

        self.assertEqual(
            error.getvalue(),
            "PD1 task-menu diagnostic: unavailable=1\n",
        )

    def test_pointer_context_open_rejects_invalid_shell_identity_before_emission(self) -> None:
        with mock.patch.object(pd1_demo, "window_process_id") as owner_probe, \
                mock.patch.object(pd1_demo, "window_ids", return_value=["4194305"]), \
                mock.patch.object(pd1_demo, "wait_until",
                                  side_effect=lambda predicate, *_args, **_kwargs: predicate()), \
                mock.patch.object(pd1_demo, "task_screen_center") as center_probe, \
                mock.patch.object(pd1_demo, "run_checked") as pointer_sender:
            with self.assertRaisesRegex(DemoError, "^shell_process_identity_invalid$"):
                open_task_context_menu_from_pointer(
                    object(), Path("/usr/bin/xdotool"), "4194305",
                    ProcessIdentity(1, 1, 0), APP_TITLES[1],
                )
        owner_probe.assert_not_called()
        center_probe.assert_not_called()
        pointer_sender.assert_not_called()

    def test_pointer_context_open_rejects_stale_panel_before_emission(self) -> None:
        with mock.patch.object(pd1_demo, "window_process_id", return_value=99), \
                mock.patch.object(pd1_demo, "window_ids", return_value=["4194305"]), \
                mock.patch.object(pd1_demo, "wait_until",
                                  side_effect=lambda predicate, *_args, **_kwargs: predicate()), \
                mock.patch.object(pd1_demo, "task_screen_center") as center_probe, \
                mock.patch.object(pd1_demo, "run_checked") as pointer_sender:
            with self.assertRaisesRegex(DemoError, "^panel_owner_identity_invalid$"):
                open_task_context_menu_from_pointer(
                    object(), Path("/usr/bin/xdotool"), "4194305",
                    ProcessIdentity(100, 1, 0), APP_TITLES[1],
                )
        center_probe.assert_not_called()
        pointer_sender.assert_not_called()

    def test_named_action_nodes_ignore_same_named_noninteractive_descendants(self) -> None:
        button = object()
        label = object()
        with mock.patch.object(pd1_demo, "named_nodes", return_value=[button, label]), \
                mock.patch.object(pd1_demo, "action_names",
                                  side_effect=(("Press",), ())):
            self.assertEqual(named_action_nodes(object(), "Demo", "Press"), [button])

    def test_atspi_children_skip_transient_null_entries(self) -> None:
        child = object()
        node = mock.Mock()
        node.get_child_count.return_value = 2
        node.get_child_at_index.side_effect = (child, None)
        self.assertEqual(children(node), [child])

    def test_showing_named_action_nodes_exclude_hidden_surface_duplicates(self) -> None:
        showing = mock.Mock()
        hidden = mock.Mock()
        showing.get_state_set.return_value.contains.side_effect = lambda state: state in (2, 3)
        hidden.get_state_set.return_value.contains.return_value = False
        atspi = mock.Mock(StateType=mock.Mock(SHOWING=2, VISIBLE=3))
        with mock.patch.object(pd1_demo, "named_action_nodes", return_value=[showing, hidden]):
            self.assertEqual(showing_named_action_nodes(atspi, "Demo", "Press"), [showing])

    def test_showing_focused_node_excludes_hidden_surface_duplicates(self) -> None:
        showing = mock.Mock()
        hidden = mock.Mock()
        showing.get_state_set.return_value.contains.side_effect = lambda state: state in (1, 2, 3)
        hidden.get_state_set.return_value.contains.return_value = False
        atspi = mock.Mock(StateType=mock.Mock(FOCUSED=1, SHOWING=2, VISIBLE=3))
        with mock.patch.object(pd1_demo, "named_nodes", return_value=[showing, hidden]):
            self.assertTrue(has_showing_focused_node(atspi, "Demo"))

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
