#!/usr/bin/env python3
"""Bounded negative and parser tests for the external evidence collectors."""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import collect_idle_wakeups
import collect_live_session_performance
import collect_startup_to_panel
import validate_live_startup
from collector_common import CollectorError, environment_id, revision, summarize
from external_evidence_semantics import semantic_error


class CommonContractTests(unittest.TestCase):
    def test_rejects_unredacted_identity_values(self) -> None:
        with self.assertRaisesRegex(CollectorError, "invalid_source_revision"):
            revision("not-a-revision")
        with self.assertRaisesRegex(CollectorError, "invalid_environment_id"):
            environment_id("Host Name")

    def test_summary_is_integer_and_deterministic(self) -> None:
        self.assertEqual(
            summarize([40, 10, 30, 20]),
            {
                "sample_count": 4,
                "minimum_ns": 10,
                "median_ns": 20,
                "p95_ns": 40,
                "maximum_ns": 40,
                "samples_ns": [40, 10, 30, 20],
            },
        )


class StartupCollectorTests(unittest.TestCase):
    def test_startup_deadline_is_exactly_the_full_supervisor_readiness_bound(self) -> None:
        arguments = [
            "collect_startup_to_panel.py",
            "--session",
            "/bin/true",
            "--settingsd",
            "/bin/true",
            "--shell",
            "/bin/true",
            "--xev",
            "/bin/true",
            "--xprop",
            "/bin/true",
            "--stdbuf",
            "/bin/true",
            "--revision",
            "0" * 40,
            "--environment-id",
            "contract-test",
        ]
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(
                collect_startup_to_panel.parse_arguments().timeout_seconds, 10
            )
        with (
            mock.patch.object(sys, "argv", [*arguments, "--timeout-seconds", "5"]),
            self.assertRaisesRegex(CollectorError, "invalid_timeout"),
        ):
            collect_startup_to_panel.parse_arguments()

    def test_session_shutdown_accepts_only_success_or_cancelled_exit(self) -> None:
        self.assertEqual(collect_startup_to_panel.SESSION_CLEAN_EXIT_CODES, (0, 7))

    def test_inotify_tree_rejects_multiple_session_instances(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            root = runtime / "prismdrake"
            root.mkdir()
            (root / "session-1-0").mkdir()
            (root / "session-2-0").mkdir()
            observer = collect_startup_to_panel.InotifyRuntimeTree(runtime)
            self.addCleanup(observer.close)
            with self.assertRaisesRegex(CollectorError, "multiple_session_instances"):
                observer.state()

    def test_dock_properties_are_exactly_parsed_in_one_bounded_read(self) -> None:
        with mock.patch.object(
            collect_startup_to_panel,
            "bounded_xprop_capture",
            return_value=(
                0,
                "_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DOCK\n"
                "_NET_WM_STRUT(CARDINAL) = 0, 0, 0, 48\n"
                "_NET_WM_STRUT_PARTIAL(CARDINAL) = 0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023\n"
                "_NET_WM_PID(CARDINAL) = 123\n"
                "WM_STATE(WM_STATE):\n"
                "\t\twindow state: Normal\n"
                "\t\ticon window: 0x0\n",
            ),
        ) as capture:
            self.assertEqual(
                collect_startup_to_panel.inspect_mapped_dock(
                    Path("/bin/true"), {}, "0x1"
                ),
                collect_startup_to_panel.DockProperties(
                    123,
                    (0, 0, 0, 48),
                    (0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023),
                ),
            )
            self.assertEqual(capture.call_count, 1)

    def test_xprop_capture_rejects_output_above_fixed_bound(self) -> None:
        with self.assertRaisesRegex(CollectorError, "dock_property_output_too_large"):
            collect_startup_to_panel.bounded_xprop_capture(
                [
                    sys.executable,
                    "-c",
                    "import sys; sys.stdout.write('x' * 70000)",
                ],
                os.environ.copy(),
            )

    def test_inventory_enumerates_all_queued_mapped_docks(self) -> None:
        first = collect_startup_to_panel.DockProperties(
            123, (0, 0, 0, 48), (0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023)
        )
        second = collect_startup_to_panel.DockProperties(
            124, (0, 0, 48, 0), (0, 0, 48, 0, 0, 0, 0, 0, 0, 1023, 0, 0)
        )
        with (
            mock.patch.object(
                collect_startup_to_panel,
                "bounded_xprop_capture",
                return_value=(
                    0,
                    "_NET_CLIENT_LIST_STACKING(WINDOW): window id # 0x1, 0x2\n",
                ),
            ),
            mock.patch.object(
                collect_startup_to_panel,
                "inspect_mapped_dock",
                side_effect=[first, second],
            ),
        ):
            self.assertEqual(
                collect_startup_to_panel.enumerate_mapped_docks(
                    Path("/bin/true"), {}
                ),
                [first, second],
            )
        with self.assertRaisesRegex(CollectorError, "foreign_mapped_dock"):
            collect_startup_to_panel.validate_mapped_dock_inventory(
                [first, second], 123
            )
        with self.assertRaisesRegex(CollectorError, "duplicate_mapped_dock"):
            collect_startup_to_panel.validate_mapped_dock_inventory(
                [first, first], 123
            )

    def test_reparented_root_map_resolves_dock_through_managed_inventory(self) -> None:
        event = (
            b"MapNotify event, serial 19, synthetic NO, window 0x21f,\n"
            b"    event 0x21f, window 0x200065, override NO\n"
        )
        dock = collect_startup_to_panel.DockProperties(
            123, (0, 0, 0, 48), (0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023)
        )
        self.assertTrue(collect_startup_to_panel.mapped_inventory_event(event))
        self.assertTrue(collect_startup_to_panel.root_map_event(event))
        self.assertFalse(
            collect_startup_to_panel.root_map_event(
                b"PropertyNotify event, serial 18, synthetic NO, window 0x21f,\n"
                b"    atom 0xfa (_NET_CLIENT_LIST_STACKING), state PropertyNewValue\n"
            )
        )
        with (
            mock.patch.object(
                collect_startup_to_panel,
                "enumerate_mapped_docks",
                return_value=[dock],
            ),
            mock.patch.object(
                collect_startup_to_panel, "bind_dock_to_shell", return_value=77
            ) as bind,
        ):
            self.assertEqual(
                collect_startup_to_panel.bind_inventory_dock(
                    100,
                    Path("/bin/true"),
                    Path("/bin/true"),
                    Path("/bin/true"),
                    {},
                ),
                (123, 77),
            )
        self.assertEqual(bind.call_args.args[:2], (100, dock))

    def test_dock_must_have_normal_icccm_state_to_count_as_mapped(self) -> None:
        output = (
            "_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DOCK\n"
            "_NET_WM_STRUT(CARDINAL) = 0, 0, 0, 48\n"
            "_NET_WM_STRUT_PARTIAL(CARDINAL) = 0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023\n"
            "_NET_WM_PID(CARDINAL) = 123\n"
            "WM_STATE(WM_STATE):\n"
            "\t\twindow state: Iconic\n"
            "\t\ticon window: 0x0\n"
        )
        self.assertIsNone(collect_startup_to_panel.parse_dock_properties(output))
        with self.assertRaisesRegex(CollectorError, "invalid_mapped_dock_contract"):
            collect_startup_to_panel.parse_dock_properties(
                output.replace("window state: Iconic", "window state: Private")
            )

    def test_dock_parser_rejects_substring_and_malformed_strut_tricks(self) -> None:
        self.assertIsNone(
            collect_startup_to_panel.parse_dock_properties(
                "_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_NORMAL\n"
                "UNRELATED = _NET_WM_WINDOW_TYPE_DOCK\n"
            )
        )
        with self.assertRaisesRegex(CollectorError, "invalid_mapped_dock_contract"):
            collect_startup_to_panel.parse_dock_properties(
                "_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DOCK\n"
                "_NET_WM_STRUT(CARDINAL) = 0, 0, 0, 48 trailing\n"
                "_NET_WM_STRUT_PARTIAL(CARDINAL) = 0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023\n"
                "_NET_WM_PID(CARDINAL) = 123\n"
                "WM_STATE(WM_STATE):\n"
                "\t\twindow state: Normal\n"
                "\t\ticon window: 0x0\n"
            )

    def test_dock_binding_rejects_a_foreign_or_ambiguous_shell_child(self) -> None:
        dock = collect_startup_to_panel.DockProperties(
            123, (0, 0, 0, 48), (0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 1023)
        )
        with (
            mock.patch.object(
                collect_startup_to_panel, "direct_child_pids", return_value={123, 124}
            ),
            mock.patch.object(
                collect_startup_to_panel.os.path, "samefile", return_value=True
            ),
        ):
            with self.assertRaisesRegex(
                CollectorError, "mapped_dock_not_supervised_shell"
            ):
                collect_startup_to_panel.bind_dock_to_shell(
                    100, dock, Path("/bin/true")
                )

    def test_observer_readiness_requires_exact_property_notify(self) -> None:
        self.assertFalse(
            collect_startup_to_panel.observer_ready_event(
                b"PropertyNotify event,\n    atom 0x1 (_UNRELATED_PROPERTY)\n"
            )
        )

    def test_observer_handshake_sets_and_removes_probe_before_return(self) -> None:
        xev = mock.Mock()
        xev.poll.return_value = None
        xev.stdout.fileno.return_value = 7
        selector = mock.Mock()
        selector.select.return_value = [(SimpleNamespace(data="x11"), None)]
        observer = mock.Mock()
        event = (
            b"PropertyNotify event,\n"
            b"    atom 0x2 (_PRISMDRAKE_PD1_OBSERVER_READY), state PropertyNewValue\n"
        )
        with (
            mock.patch.object(collect_startup_to_panel.os, "read", return_value=event),
            mock.patch.object(collect_startup_to_panel, "_change_root_probe") as change,
        ):
            collect_startup_to_panel.wait_for_x11_observer(
                Path("/bin/true"), {}, xev, selector, observer
            )
        self.assertEqual(change.call_args_list[0].args[2], "0")
        self.assertIsNone(change.call_args_list[-1].args[2])
        self.assertTrue(
            collect_startup_to_panel.observer_ready_event(
                b"PropertyNotify event,\n"
                b"    atom 0x2 (_PRISMDRAKE_PD1_OBSERVER_READY), state PropertyNewValue\n"
            )
        )

    def test_restart_observation_uses_structured_recovery_field(self) -> None:
        with tempfile.TemporaryFile() as diagnostics:
            diagnostics.write(
                b"component=prismdrake-shell severity=error event=component_start_failed "
                b"generation=none profile=none recovery=restart_component\n"
            )
            diagnostics.flush()
            self.assertEqual(
                collect_startup_to_panel.session_diagnostic_state(
                    diagnostics, diagnostics.tell()
                ),
                (True, True),
            )


class IdleCollectorTests(unittest.TestCase):
    def test_parses_both_filtered_scheduler_tracepoints(self) -> None:
        self.assertEqual(
            collect_idle_wakeups.parse_perf(
                "7;;sched:sched_wakeup;100.00;\n2;;sched:sched_wakeup_new;100.00;\n"
            ),
            (7, 2),
        )

    def test_rejects_unsupported_tracepoint(self) -> None:
        with self.assertRaisesRegex(CollectorError, "scheduler_tracepoint_unavailable"):
            collect_idle_wakeups.parse_perf(
                "<not supported>;;sched:sched_wakeup;0.00;\n"
            )

    def test_filter_targets_only_supplied_thread_ids(self) -> None:
        self.assertEqual(
            collect_idle_wakeups.event_filter((12, 34)), "pid == 12 || pid == 34"
        )

    def test_perf_command_is_system_wide_and_exactly_filtered(self) -> None:
        command = collect_idle_wakeups.perf_command(self._options(), (12, 34))
        self.assertEqual(command[:4], ["/bin/true", "stat", "-a", "--no-big-num"])
        self.assertEqual(command.count("-a"), 1)
        self.assertEqual(command.count("pid == 12 || pid == 34"), 2)

    def test_pidfd_guard_rejects_identity_loss_at_a_boundary(self) -> None:
        descriptors = {"session": 20, "settingsd": 21, "shell": 22}
        with (
            mock.patch.object(
                collect_idle_wakeups, "pidfd_alive", side_effect=[True, False]
            ),
            mock.patch.object(collect_idle_wakeups, "verify_components"),
        ):
            with self.assertRaisesRegex(CollectorError, "component_identity_changed"):
                collect_idle_wakeups.verify_identity_boundary(
                    self._options(), descriptors
                )

    def test_endpoint_rejects_non_direct_or_extra_supervised_children(self) -> None:
        options = self._options()
        options.runtime_directory = Path("/tmp")
        options.xprop = Path("/bin/true")
        with (
            mock.patch.dict(
                os.environ,
                {"DISPLAY": ":99", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/test"},
            ),
            mock.patch.object(
                collect_idle_wakeups,
                "direct_child_pids",
                return_value={11, 12, 13},
            ),
        ):
            with self.assertRaisesRegex(
                CollectorError, "supervised_component_tree_invalid"
            ):
                collect_idle_wakeups.verify_supervised_startup_endpoint(options)

    def test_partial_launch_failure_terminates_and_reaps_started_perf(self) -> None:
        process = mock.Mock()
        process.poll.return_value = None
        terminate = mock.Mock(return_value=True)
        with (
            mock.patch.object(
                collect_idle_wakeups,
                "open_component_pidfds",
                return_value={"session": 20, "settingsd": 21, "shell": 22},
            ),
            mock.patch.object(collect_idle_wakeups, "verify_identity_boundary"),
            mock.patch.object(
                collect_idle_wakeups, "verify_supervised_startup_endpoint"
            ),
            mock.patch.object(collect_idle_wakeups, "close_pidfds"),
            mock.patch.object(collect_idle_wakeups, "enable_child_subreaper"),
            mock.patch.object(collect_idle_wakeups, "terminate_and_reap", terminate),
            mock.patch.object(collect_idle_wakeups.time, "sleep"),
            mock.patch.object(collect_idle_wakeups, "thread_ids", return_value=(1,)),
            mock.patch.object(
                collect_idle_wakeups.subprocess,
                "Popen",
                side_effect=[process, OSError("launch failed")],
            ),
        ):
            with self.assertRaises(OSError):
                collect_idle_wakeups.collect(self._options())
        self.assertEqual(terminate.call_args.args[0], [process])

    def test_communicate_timeout_terminates_and_reaps_every_perf_child(self) -> None:
        processes = [mock.Mock() for _role in collect_idle_wakeups.ROLES]
        processes[0].communicate.side_effect = subprocess.TimeoutExpired("perf", 1)
        for process in processes:
            process.poll.return_value = None
        terminate = mock.Mock(return_value=True)
        with (
            mock.patch.object(
                collect_idle_wakeups,
                "open_component_pidfds",
                return_value={"session": 20, "settingsd": 21, "shell": 22},
            ),
            mock.patch.object(collect_idle_wakeups, "verify_identity_boundary"),
            mock.patch.object(
                collect_idle_wakeups, "verify_supervised_startup_endpoint"
            ),
            mock.patch.object(collect_idle_wakeups, "close_pidfds"),
            mock.patch.object(collect_idle_wakeups, "enable_child_subreaper"),
            mock.patch.object(collect_idle_wakeups, "terminate_and_reap", terminate),
            mock.patch.object(collect_idle_wakeups.time, "sleep"),
            mock.patch.object(collect_idle_wakeups, "thread_ids", return_value=(1,)),
            mock.patch.object(
                collect_idle_wakeups.subprocess, "Popen", side_effect=processes
            ),
        ):
            with self.assertRaisesRegex(CollectorError, "perf_collection_unbounded"):
                collect_idle_wakeups.collect(self._options())
        self.assertEqual(terminate.call_args.args[0], processes)

    def test_parse_failure_still_reaps_every_perf_child(self) -> None:
        processes = [mock.Mock() for _role in collect_idle_wakeups.ROLES]
        for process in processes:
            process.communicate.return_value = (None, "invalid output\n")
            process.returncode = 0
            process.poll.return_value = 0
        terminate = mock.Mock(return_value=True)
        with (
            mock.patch.object(
                collect_idle_wakeups,
                "open_component_pidfds",
                return_value={"session": 20, "settingsd": 21, "shell": 22},
            ),
            mock.patch.object(collect_idle_wakeups, "verify_identity_boundary"),
            mock.patch.object(
                collect_idle_wakeups, "verify_supervised_startup_endpoint"
            ),
            mock.patch.object(collect_idle_wakeups, "close_pidfds"),
            mock.patch.object(collect_idle_wakeups, "enable_child_subreaper"),
            mock.patch.object(collect_idle_wakeups, "terminate_and_reap", terminate),
            mock.patch.object(collect_idle_wakeups.time, "sleep"),
            mock.patch.object(collect_idle_wakeups, "thread_ids", return_value=(1,)),
            mock.patch.object(
                collect_idle_wakeups.subprocess, "Popen", side_effect=processes
            ),
        ):
            with self.assertRaisesRegex(CollectorError, "invalid_perf_output"):
                collect_idle_wakeups.collect(self._options())
        self.assertEqual(terminate.call_args.args[0], processes)

    @staticmethod
    def _options() -> SimpleNamespace:
        return SimpleNamespace(
            duration_seconds=1,
            perf=Path("/bin/true"),
            sleep=Path("/bin/true"),
            session_pid=10,
            settingsd_pid=11,
            shell_pid=12,
            session_executable=Path("/bin/true"),
            settingsd_executable=Path("/bin/true"),
            shell_executable=Path("/bin/true"),
            revision="0" * 40,
            environment_id="contract-test",
        )

    def test_collection_holds_pidfds_and_isolates_perf_process_groups(self) -> None:
        processes = []
        for _role in collect_idle_wakeups.ROLES:
            process = mock.Mock()
            process.communicate.return_value = (
                None,
                "1;;sched:sched_wakeup;100.00;\n"
                "2;;sched:sched_wakeup_new;100.00;\n",
            )
            process.returncode = 0
            process.poll.return_value = 0
            processes.append(process)
        terminate = mock.Mock(return_value=True)
        with (
            mock.patch.object(
                collect_idle_wakeups,
                "open_component_pidfds",
                return_value={"session": 20, "settingsd": 21, "shell": 22},
            ),
            mock.patch.object(
                collect_idle_wakeups, "verify_identity_boundary"
            ) as verify,
            mock.patch.object(
                collect_idle_wakeups, "verify_supervised_startup_endpoint"
            ) as endpoint,
            mock.patch.object(collect_idle_wakeups, "close_pidfds") as close_pidfds,
            mock.patch.object(collect_idle_wakeups, "enable_child_subreaper"),
            mock.patch.object(collect_idle_wakeups, "terminate_and_reap", terminate),
            mock.patch.object(collect_idle_wakeups.time, "sleep"),
            mock.patch.object(collect_idle_wakeups, "thread_ids", return_value=(1,)),
            mock.patch.object(
                collect_idle_wakeups.subprocess, "Popen", side_effect=processes
            ) as popen,
        ):
            document = collect_idle_wakeups.collect(self._options())
        self.assertEqual(verify.call_count, 5)
        self.assertEqual(endpoint.call_count, 5)
        close_pidfds.assert_called_once_with(
            {"session": 20, "settingsd": 21, "shell": 22}
        )
        self.assertTrue(all(call.kwargs["start_new_session"] for call in popen.call_args_list))
        self.assertFalse(document["method"]["contract_eligible"])

    def test_cleanup_signals_and_reaps_each_isolated_process_group(self) -> None:
        processes = [mock.Mock(pid=101), mock.Mock(pid=202)]
        for process in processes:
            process.wait.return_value = 0
        with (
            mock.patch.object(
                collect_idle_wakeups, "_signal_process_groups", return_value=True
            ) as signal_groups,
            mock.patch.object(
                collect_idle_wakeups, "_reap_adopted_group", return_value=True
            ) as reap_group,
        ):
            self.assertTrue(collect_idle_wakeups.terminate_and_reap(processes))
        self.assertEqual(
            [call.args[1] for call in signal_groups.call_args_list],
            [collect_idle_wakeups.signal.SIGTERM, collect_idle_wakeups.signal.SIGKILL],
        )
        self.assertEqual(
            [call.args[0] for call in reap_group.call_args_list], [101, 202]
        )

    def test_adopted_sleep_grandchild_is_reaped_by_process_group(self) -> None:
        with (
            mock.patch.object(
                collect_idle_wakeups.os,
                "waitpid",
                side_effect=[(303, 0), ChildProcessError()],
            ) as waitpid,
            mock.patch.object(
                collect_idle_wakeups, "process_group_exists", return_value=False
            ),
        ):
            self.assertTrue(
                collect_idle_wakeups._reap_adopted_group(101, float("inf"))
            )
        self.assertTrue(all(call.args[0] == -101 for call in waitpid.call_args_list))


class SemanticContractTests(unittest.TestCase):
    def test_rejects_semantically_inconsistent_startup_and_idle_artifacts(self) -> None:
        startup = {
            "evidence_kind": "startup_to_mapped_panel",
            "method": {"deadline_ns": 10_000_000_000},
            "result": {
                "duration_ns": 1,
                "ready_marker_ns": 2,
                "mapped_dock_ns": 3,
                "safe_mode": False,
                "child_restart_observed": False,
                "mapped_dock_count": 1,
                "foreign_dock_observed": False,
                "duplicate_dock_observed": False,
            },
        }
        self.assertEqual(
            semantic_error(startup), "startup_endpoint_semantics_invalid"
        )
        idle = {
            "evidence_kind": "idle_scheduler_wakeups",
            "method": {
                "requested_interval_ns": 60_000_000_000,
                "observed_interval_ns": 60_000_000_000,
                "collection_scope": "system_wide",
                "process_identity": "pidfd_and_proc_executable_at_boundaries",
                "contract_eligible": True,
                "live_tree_ownership": "exact_session_direct_children",
                "startup_endpoint": "ready_marker_and_single_owned_mapped_dock",
            },
            "results": [
                {
                    "component": role,
                    "thread_count_start": 1,
                    "thread_count_end": 1,
                    "sched_wakeup_count": 1,
                    "sched_wakeup_new_count": 1,
                    "received_wakeup_count": 3 if role == "shell" else 2,
                }
                for role in collect_idle_wakeups.ROLES
            ],
        }
        self.assertEqual(semantic_error(idle), "idle_wakeup_sum_invalid")


class LiveWorkflowTests(unittest.TestCase):
    def test_idle_collection_uses_the_startup_callback_live_tree(self) -> None:
        options = SimpleNamespace(
            session=Path("/bin/true"),
            settingsd=Path("/bin/true"),
            shell=Path("/bin/true"),
            xev=Path("/bin/true"),
            xprop=Path("/bin/true"),
            stdbuf=Path("/bin/true"),
            perf=Path("/bin/true"),
            sleep=Path("/bin/true"),
            revision="0" * 40,
            environment_id="contract-test",
            startup_output=Path("/tmp/startup.json"),
            idle_output=Path("/tmp/idle.json"),
        )
        live = collect_startup_to_panel.LiveSession(
            10, 11, 12, Path("/tmp/runtime"), {"DISPLAY": ":99"}
        )

        startup_options = []

        def startup_collect(options: object, callback: object) -> object:
            startup_options.append(options)
            return {"evidence_kind": "startup_to_mapped_panel"}, callback(live)

        with (
            mock.patch.object(
                collect_live_session_performance.collect_startup_to_panel,
                "collect",
                side_effect=startup_collect,
            ),
            mock.patch.object(
                collect_live_session_performance.collect_idle_wakeups,
                "collect",
                return_value={"evidence_kind": "idle_scheduler_wakeups"},
            ) as idle_collect,
            mock.patch.object(
                collect_live_session_performance, "write_artifact"
            ) as write_artifact,
        ):
            collect_live_session_performance.run(options)
        idle_options = idle_collect.call_args.args[0]
        self.assertEqual(
            (idle_options.session_pid, idle_options.settingsd_pid, idle_options.shell_pid),
            (10, 11, 12),
        )
        self.assertEqual(idle_options.runtime_directory, Path("/tmp/runtime"))
        self.assertEqual(startup_options[0].timeout_seconds, 10)
        self.assertEqual(write_artifact.call_count, 2)


class LiveStartupValidatorTests(unittest.TestCase):
    def test_preserves_one_exact_closed_collector_identifier(self) -> None:
        completed = subprocess.CompletedProcess(
            [],
            2,
            stdout="",
            stderr="collect_startup_to_panel: startup_deadline_exceeded\n",
        )
        self.assertEqual(
            validate_live_startup.collector_failure_identifier(completed),
            "startup_deadline_exceeded",
        )
        diagnostics = io.StringIO()
        with (
            mock.patch.object(
                sys,
                "argv",
                ["validate_live_startup.py", "schema", "collector", "--"],
            ),
            mock.patch.object(
                validate_live_startup.subprocess, "run", return_value=completed
            ),
            contextlib.redirect_stderr(diagnostics),
        ):
            self.assertEqual(validate_live_startup.main(), 1)
        self.assertEqual(
            diagnostics.getvalue(),
            "validate_live_startup: startup_deadline_exceeded\n",
        )

    def test_replaces_malformed_or_unknown_stderr_without_leaking_it(self) -> None:
        private_values = [
            "collect_startup_to_panel: /home/private/session failed\n",
            "collect_startup_to_panel: startup_deadline_exceeded\nprivate detail\n",
            "collect_startup_to_panel: unknown_private_identifier\n",
            "startup_deadline_exceeded\n",
        ]
        for private_value in private_values:
            with self.subTest(private_value=private_value):
                completed = subprocess.CompletedProcess(
                    [], 2, stdout="", stderr=private_value
                )
                identifier = validate_live_startup.collector_failure_identifier(
                    completed
                )
                self.assertEqual(identifier, "collector_failed")
                self.assertNotIn(private_value, identifier)
                diagnostics = io.StringIO()
                with (
                    mock.patch.object(
                        sys,
                        "argv",
                        [
                            "validate_live_startup.py",
                            "schema",
                            "collector",
                            "--",
                        ],
                    ),
                    mock.patch.object(
                        validate_live_startup.subprocess,
                        "run",
                        return_value=completed,
                    ),
                    contextlib.redirect_stderr(diagnostics),
                ):
                    self.assertEqual(validate_live_startup.main(), 1)
                self.assertEqual(
                    diagnostics.getvalue(),
                    "validate_live_startup: collector_failed\n",
                )
                self.assertNotIn(private_value, diagnostics.getvalue())


if __name__ == "__main__":
    os.environ["PRISMDRAKE_PERFORMANCE_TESTING"] = "1"
    unittest.main()
