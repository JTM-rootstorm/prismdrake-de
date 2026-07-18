#!/usr/bin/env python3
"""Negative and schema-source tests for the live AT-SPI evidence contract."""

from __future__ import annotations

import copy
import json
import subprocess
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import live_atspi
from live_atspi import (
    EvidenceError,
    _grab_focus,
    example_evidence_document,
    validate_evidence_document,
    validate_evidence_schema,
)


class AccessibilityEvidenceContractTests(unittest.TestCase):
    def test_probe_converts_command_failures_to_categorical_status(self) -> None:
        with mock.patch.object(
            live_atspi,
            "_run_checked",
            side_effect=subprocess.TimeoutExpired(["private-command"], 0.5),
        ):
            completed = live_atspi._run_probe(["private-command"], timeout=0.5)
        self.assertEqual(completed.returncode, 124)
        self.assertEqual(completed.stdout, "")

    def test_launcher_probe_is_bounded_categorical_and_redacted(self) -> None:
        node = mock.Mock()
        node.get_process_id.return_value = 42
        node.get_state_set.return_value.contains.return_value = True
        node.get_component_iface.return_value.get_extents.return_value = SimpleNamespace(
            x=20, y=70, width=300, height=48
        )
        atspi = SimpleNamespace(
            CoordType=SimpleNamespace(SCREEN=1),
            StateType=SimpleNamespace(FOCUSED=2),
        )
        title = SimpleNamespace(returncode=0, stdout="17\n")
        owner = SimpleNamespace(returncode=0, stdout="42\n")
        shell_windows = SimpleNamespace(returncode=0, stdout="11\n17\n19\n")
        visible = SimpleNamespace(returncode=0, stdout="11\n17\n")
        focused = SimpleNamespace(returncode=0, stdout="17\n")
        window_class = SimpleNamespace(returncode=0, stdout="prismdrake-shell\n")
        with mock.patch.object(
            live_atspi,
            "_run_checked",
            side_effect=[
                title,
                owner,
                shell_windows,
                owner,
                owner,
                visible,
                owner,
                focused,
                owner,
                window_class,
            ],
        ):
            summary = live_atspi._launcher_probe_summary(
                atspi, Path("/usr/bin/xdotool"), node, "11", 42
            )
        self.assertEqual(
            summary,
            "launcher_probe accessible_owner=1 accessible_focused=1 component=1 "
            "extents_positive=1 title_query=1 title_shape=1 title_count=1 "
            "title_non_panel_count=1 title_owned_count=1 title_owner_complete=1 "
            "class_query=1 class_shape=1 class_count=3 class_non_panel_count=2 "
            "class_owned_count=2 class_owner_complete=1 visible_query=1 "
            "visible_shape=1 visible_count=2 visible_non_panel_count=1 "
            "visible_owned_count=1 visible_owner_complete=1 focus_shape=1 "
            "focus_non_panel=1 focus_owned=1 focus_class=1",
        )
        for private_value in ("11", "17", "19", "42", "/usr/bin", "Prismdrake Launcher"):
            self.assertNotIn(private_value, summary)

    def test_launcher_probe_caps_counts_and_owner_queries(self) -> None:
        node = mock.Mock()
        node.get_process_id.return_value = 41
        node.get_state_set.return_value.contains.return_value = False
        node.get_component_iface.return_value = None
        atspi = SimpleNamespace(
            CoordType=SimpleNamespace(SCREEN=1),
            StateType=SimpleNamespace(FOCUSED=2),
        )
        oversized = SimpleNamespace(
            returncode=0, stdout="\n".join(str(value) for value in range(32)) + "\n"
        )
        unavailable = SimpleNamespace(returncode=1, stdout="")
        with mock.patch.object(
            live_atspi,
            "_run_checked",
            side_effect=[oversized, oversized, unavailable, unavailable],
        ) as run_checked:
            summary = live_atspi._launcher_probe_summary(
                atspi, Path("/usr/bin/xdotool"), node, "99", 42
            )
        self.assertIn("title_count=17", summary)
        self.assertIn("title_shape=0", summary)
        self.assertIn("class_count=17", summary)
        self.assertIn("class_shape=0", summary)
        self.assertIn("visible_count=0", summary)
        self.assertIn("focus_shape=0", summary)
        self.assertEqual(run_checked.call_count, 4)

    def test_selects_focused_surface_for_exact_accessible_and_x11_owner(self) -> None:
        focused = SimpleNamespace(returncode=0, stdout="17\n")
        owner = SimpleNamespace(returncode=0, stdout="42\n")
        window_class = SimpleNamespace(returncode=0, stdout="prismdrake-shell\n")
        node = mock.Mock()
        node.get_process_id.return_value = 42
        node.get_state_set.return_value.contains.return_value = True
        atspi = SimpleNamespace(StateType=SimpleNamespace(FOCUSED=1))
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[focused, owner, window_class]
        ) as run_checked:
            self.assertEqual(
                live_atspi._focused_window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                ),
                "17",
            )
        self.assertEqual(
            run_checked.call_args_list,
            [
                mock.call(["/usr/bin/xdotool", "getwindowfocus"]),
                mock.call(["/usr/bin/xdotool", "getwindowpid", "17"]),
                mock.call(["/usr/bin/xdotool", "getwindowclassname", "17"]),
            ],
        )

    def test_focused_surface_rejects_unfocused_or_foreign_accessible(self) -> None:
        atspi = SimpleNamespace(StateType=SimpleNamespace(FOCUSED=1))
        node = mock.Mock()
        node.get_process_id.return_value = 41
        with mock.patch.object(live_atspi, "_run_checked") as run_checked:
            self.assertIsNone(
                live_atspi._focused_window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                )
            )
        run_checked.assert_not_called()

        node.get_process_id.return_value = 42
        node.get_state_set.return_value.contains.return_value = False
        with mock.patch.object(live_atspi, "_run_checked") as run_checked:
            self.assertIsNone(
                live_atspi._focused_window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                )
            )
        run_checked.assert_not_called()

    def test_focused_surface_rejects_panel_owner_or_class_mismatch(self) -> None:
        node = mock.Mock()
        node.get_process_id.return_value = 42
        node.get_state_set.return_value.contains.return_value = True
        atspi = SimpleNamespace(StateType=SimpleNamespace(FOCUSED=1))

        panel = SimpleNamespace(returncode=0, stdout="11\n")
        with mock.patch.object(live_atspi, "_run_checked", return_value=panel):
            self.assertIsNone(
                live_atspi._focused_window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                )
            )

        focused = SimpleNamespace(returncode=0, stdout="17\n")
        foreign_owner = SimpleNamespace(returncode=0, stdout="41\n")
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[focused, foreign_owner]
        ):
            self.assertIsNone(
                live_atspi._focused_window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                )
            )

        owner = SimpleNamespace(returncode=0, stdout="42\n")
        foreign_class = SimpleNamespace(returncode=0, stdout="other-shell\n")
        with mock.patch.object(
            live_atspi,
            "_run_checked",
            side_effect=[focused, owner, foreign_class],
        ):
            self.assertIsNone(
                live_atspi._focused_window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                )
            )

    def test_selects_sole_visible_non_panel_surface_with_exact_owner(self) -> None:
        search = SimpleNamespace(returncode=0, stdout="11\n17\n")
        owner = SimpleNamespace(returncode=0, stdout="42\n")
        node = mock.Mock()
        node.get_process_id.return_value = 42
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[search, owner]
        ) as run_checked:
            self.assertEqual(
                live_atspi._visible_window_for_accessible(
                    Path("/usr/bin/xdotool"), node, "11", 42
                ),
                "17",
            )
        self.assertEqual(
            run_checked.call_args_list,
            [
                mock.call(
                    [
                        "/usr/bin/xdotool",
                        "search",
                        "--all",
                        "--onlyvisible",
                        "--class",
                        "^prismdrake-shell$",
                    ]
                ),
                mock.call(["/usr/bin/xdotool", "getwindowpid", "17"]),
            ],
        )

    def test_selects_exact_titled_surface_across_all_desktops(self) -> None:
        search = SimpleNamespace(returncode=0, stdout="17\n")
        owner = SimpleNamespace(returncode=0, stdout="42\n")
        node = mock.Mock()
        node.get_process_id.return_value = 42
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[search, owner]
        ) as run_checked:
            self.assertEqual(
                live_atspi._titled_window_for_accessible(
                    Path("/usr/bin/xdotool"), node, "11", 42
                ),
                "17",
            )
        self.assertEqual(
            run_checked.call_args_list,
            [
                mock.call(
                    [
                        "/usr/bin/xdotool",
                        "search",
                        "--all",
                        "--name",
                        "^Prismdrake Launcher$",
                    ]
                ),
                mock.call(["/usr/bin/xdotool", "getwindowpid", "17"]),
            ],
        )

    def test_titled_surface_rejects_foreign_ambiguous_and_malformed_results(self) -> None:
        node = mock.Mock()
        node.get_process_id.return_value = 42
        xdotool = Path("/usr/bin/xdotool")

        foreign_node = mock.Mock()
        foreign_node.get_process_id.return_value = 41
        with mock.patch.object(live_atspi, "_run_checked") as run_checked:
            self.assertIsNone(
                live_atspi._titled_window_for_accessible(
                    xdotool, foreign_node, "11", 42
                )
            )
        run_checked.assert_not_called()

        for output in ("", "17\n19\n", "private-window\n", "11\n"):
            completed = SimpleNamespace(returncode=0, stdout=output)
            with mock.patch.object(live_atspi, "_run_checked", return_value=completed):
                self.assertIsNone(
                    live_atspi._titled_window_for_accessible(xdotool, node, "11", 42)
                )

        search = SimpleNamespace(returncode=0, stdout="17\n")
        foreign_owner = SimpleNamespace(returncode=0, stdout="41\n")
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[search, foreign_owner]
        ):
            self.assertIsNone(
                live_atspi._titled_window_for_accessible(xdotool, node, "11", 42)
            )

    def test_visible_surface_rejects_foreign_ambiguous_and_malformed_results(self) -> None:
        node = mock.Mock()
        node.get_process_id.return_value = 42
        xdotool = Path("/usr/bin/xdotool")

        foreign_node = mock.Mock()
        foreign_node.get_process_id.return_value = 41
        with mock.patch.object(live_atspi, "_run_checked") as run_checked:
            self.assertIsNone(
                live_atspi._visible_window_for_accessible(
                    xdotool, foreign_node, "11", 42
                )
            )
        run_checked.assert_not_called()

        malformed = SimpleNamespace(returncode=0, stdout="11\nprivate-window\n")
        with mock.patch.object(live_atspi, "_run_checked", return_value=malformed):
            self.assertIsNone(
                live_atspi._visible_window_for_accessible(xdotool, node, "11", 42)
            )

        ambiguous = SimpleNamespace(returncode=0, stdout="11\n17\n19\n")
        owner = SimpleNamespace(returncode=0, stdout="42\n")
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[ambiguous, owner, owner]
        ):
            self.assertIsNone(
                live_atspi._visible_window_for_accessible(xdotool, node, "11", 42)
            )

        visible = SimpleNamespace(returncode=0, stdout="11\n17\n")
        foreign_owner = SimpleNamespace(returncode=0, stdout="41\n")
        with mock.patch.object(
            live_atspi, "_run_checked", side_effect=[visible, foreign_owner]
        ):
            self.assertIsNone(
                live_atspi._visible_window_for_accessible(xdotool, node, "11", 42)
            )

        oversized = SimpleNamespace(
            returncode=0, stdout="\n".join(str(value) for value in range(17)) + "\n"
        )
        with mock.patch.object(live_atspi, "_run_checked", return_value=oversized):
            with self.assertRaisesRegex(EvidenceError, "visible shell X11 window count"):
                live_atspi._visible_window_for_accessible(xdotool, node, "99", 42)

    def test_selects_exact_class_surface_containing_accessible_center(self) -> None:
        search = SimpleNamespace(returncode=0, stdout="11\n17\n19\n")
        launcher = SimpleNamespace(
            returncode=0,
            stdout="WINDOW=17\nX=0\nY=52\nWIDTH=560\nHEIGHT=620\nSCREEN=0\n",
        )
        launcher_owner = SimpleNamespace(returncode=0, stdout="42\n")
        other = SimpleNamespace(
            returncode=0,
            stdout="WINDOW=19\nX=800\nY=40\nWIDTH=300\nHEIGHT=200\nSCREEN=0\n",
        )
        other_owner = SimpleNamespace(returncode=0, stdout="42\n")
        node = mock.Mock()
        node.get_process_id.return_value = 42
        node.get_component_iface.return_value.get_extents.return_value = SimpleNamespace(
            x=20, y=70, width=300, height=48
        )
        atspi = SimpleNamespace(CoordType=SimpleNamespace(SCREEN=1))
        with mock.patch.object(
            live_atspi,
            "_run_checked",
            side_effect=[search, launcher_owner, launcher, other_owner, other],
        ):
            self.assertEqual(
                live_atspi._window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                ),
                "17",
            )

    def test_accessible_surface_rejects_process_identity_mismatch(self) -> None:
        node = mock.Mock()
        node.get_process_id.return_value = 41
        atspi = SimpleNamespace(CoordType=SimpleNamespace(SCREEN=1))
        with mock.patch.object(live_atspi, "_run_checked") as run_checked:
            self.assertIsNone(
                live_atspi._window_for_accessible(
                    atspi, Path("/usr/bin/xdotool"), node, "11", 42
                )
            )
        run_checked.assert_not_called()

    def test_grab_focus_rejects_missing_component_interface(self) -> None:
        class NodeWithoutComponent:
            @staticmethod
            def get_component_iface() -> None:
                return None

        self.assertFalse(_grab_focus(NodeWithoutComponent()))

    def test_grab_focus_returns_component_result(self) -> None:
        class AcceptingComponent:
            @staticmethod
            def grab_focus() -> bool:
                return True

        class NodeWithComponent:
            @staticmethod
            def get_component_iface() -> AcceptingComponent:
                return AcceptingComponent()

        self.assertTrue(_grab_focus(NodeWithComponent()))

    def test_example_satisfies_semantic_contract(self) -> None:
        evidence = example_evidence_document()
        validate_evidence_schema(evidence)
        validate_evidence_document(evidence)

    def test_schema_source_is_strict_version_one_json(self) -> None:
        schema_path = Path(__file__).with_name("atspi-evidence.schema.json")
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        self.assertEqual(schema["properties"]["phases"]["minItems"], 7)
        self.assertEqual(schema["properties"]["phases"]["maxItems"], 7)
        self.assertFalse(schema["$defs"]["phase"]["additionalProperties"])
        self.assertFalse(schema["$defs"]["control"]["additionalProperties"])

    def test_rejects_missing_phase(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"].pop()
        with self.assertRaisesRegex(EvidenceError, "wrong phase count"):
            validate_evidence_document(evidence)

    def test_rejects_reordered_focus_evidence(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][1]["focused_control"] = "Prismdrake Evidence App"
        with self.assertRaisesRegex(EvidenceError, "unexpected focused control"):
            validate_evidence_document(evidence)

    def test_rejects_duplicate_control(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][1] = copy.deepcopy(
            evidence["phases"][0]["controls"][0]
        )
        with self.assertRaisesRegex(EvidenceError, "missing or reordered"):
            validate_evidence_document(evidence)

    def test_rejects_private_runtime_data(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][0]["description"] = "unix:path=/tmp/private"
        with self.assertRaisesRegex(EvidenceError, "private runtime data"):
            validate_evidence_document(evidence)

    def test_rejects_non_fixture_description(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][0]["description"] = "Unexpected user content"
        with self.assertRaisesRegex(EvidenceError, "outside the redacted fixture allow-list"):
            validate_evidence_document(evidence)

    def test_rejects_missing_button_action(self) -> None:
        evidence = example_evidence_document()
        evidence["phases"][0]["controls"][0]["actions"] = []
        with self.assertRaisesRegex(EvidenceError, "button has no Press action"):
            validate_evidence_document(evidence)


if __name__ == "__main__":
    unittest.main()
