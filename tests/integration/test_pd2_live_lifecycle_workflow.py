#!/usr/bin/env python3
"""Hermetic contracts for the PD2 installed lifecycle workflow."""

from __future__ import annotations

import unittest

from pd2_live_lifecycle_workflow import (
    MAX_SHUTDOWN_MILLISECONDS,
    SHUTDOWN_ORDER,
    STEP_CATEGORIES,
    CleanupObservation,
    ComponentObservation,
    InstalledLifecycleWorkflow,
    LifecycleWorkflowError,
    LogoutObservation,
    NotificationEpochObservation,
    OutputIdentity,
    ProcessIdentity,
    RestartObservation,
    ReviewedSurfaceObservation,
    SettingsEpochObservation,
    ShutdownEvent,
    ShutdownObservation,
    SurfacePlacement,
    SurfacePlacementObservation,
    TopologyObservation,
    WindowAuthorityObservation,
    WorkflowResult,
    validate_workflow_result,
)


SURFACES = ("desktop_primary", "panel_primary", "desktop_secondary", "panel_secondary")


def identity(role: str, serial: int) -> ProcessIdentity:
    return ProcessIdentity(role, 1_000 + serial, 10_000 + serial)


class FakeInstalledPorts:
    def __init__(self) -> None:
        self.first = OutputIdentity("DUMMY-0", 1, True, True)
        self.second = OutputIdentity("DUMMY-1", 1, True, False)
        self.topology_generation = 1
        self.outputs = (self.first, self.second)
        self.placements = (self.first, self.first, self.second, self.second)
        self.components_value = ComponentObservation(
            identity("prismdrake-settingsd", 1),
            identity("prismdrake-notifyd", 2),
            identity("prismdrake-shell", 3),
        )
        self.window_value = WindowAuthorityObservation(
            identity("window-manager", 10),
            (identity("application", 11), identity("application", 12)),
            (0x1001, 0x1002),
        )
        self.notification_fingerprint = b"n" * 32
        self.notification_dependency_available = True
        self.notification_generation = 9
        self.notification_record_count = 2
        self.notification_persistent = True
        self.notification_safe_degradation = True
        self.settings_generation = 7
        self.last_restart: str | None = None
        self.restart_returns_stale_before = False
        self.restart_replaces_sibling = False
        self.restart_invalidated_override: bool | None = None
        self.surface_unsettled = False
        self.surface_inaccessible = False
        self.migration_incomplete = False
        self.surface_offscreen = False
        self.readd_stale_epoch = False
        self.readd_stale_generation = False
        self.notification_fingerprint_after_restart = self.notification_fingerprint
        self.preserve_windows = True
        self.stale_transaction_rejected = True
        self.shutdown_roles = SHUTDOWN_ORDER
        self.shutdown_total = 150
        self.shutdown_session_stopped = True
        self.shutdown_tied_elapsed = False
        self.unrelated_alive = True
        self.cleanup_value = CleanupObservation(0, False, False)
        self.transaction = 40
        self.confirmed_transaction: int | None = None
        self.shutdown_components: ComponentObservation | None = None
        self.raise_private_error = False
        self.calls: list[str] = []

    def topology(self) -> TopologyObservation:
        self.calls.append("topology")
        if self.raise_private_error:
            raise RuntimeError("/home/private/token")
        return TopologyObservation(self.topology_generation, self.outputs)

    def surface_placements(self) -> SurfacePlacementObservation:
        self.calls.append("surface_placements")
        placements = self.placements
        if self.migration_incomplete and not self.first.active:
            placements = (self.first, self.second, self.second, self.second)
        return SurfacePlacementObservation(
            self.topology_generation,
            tuple(
                SurfacePlacement(
                    surface,
                    output,
                    True,
                    not (self.surface_offscreen and index == 0),
                )
                for index, (surface, output) in enumerate(
                    zip(SURFACES, placements, strict=True)
                )
            ),
        )

    def remove_output(self, output: OutputIdentity, topology_generation: int) -> None:
        self.calls.append("remove_output")
        if output != self.first or topology_generation != self.topology_generation:
            raise RuntimeError("stale output accepted")
        self.topology_generation += 1
        self.first = OutputIdentity("DUMMY-0", 1, False, False)
        self.second = OutputIdentity("DUMMY-1", 1, True, True)
        self.outputs = (self.first, self.second)
        self.placements = (self.second,) * len(SURFACES)

    def readd_output(self, output: OutputIdentity, topology_generation: int) -> None:
        self.calls.append("readd_output")
        if output.name != self.first.name or topology_generation != self.topology_generation:
            raise RuntimeError("stale topology accepted")
        if not self.readd_stale_generation:
            self.topology_generation += 1
        epoch = output.connection_epoch if self.readd_stale_epoch else output.connection_epoch + 1
        self.first = OutputIdentity("DUMMY-0", epoch, True, False)
        self.outputs = (self.first, self.second)
        self.placements = (self.first, self.first, self.second, self.second)

    def components(self) -> ComponentObservation:
        self.calls.append("components")
        return self.components_value

    def prepare_survival_fixture(self) -> WindowAuthorityObservation:
        self.calls.append("prepare_survival_fixture")
        return self.window_value

    def restart_exact(self, expected: ProcessIdentity) -> RestartObservation:
        self.calls.append(f"restart:{expected.role}")
        current = self.components_value.for_role(expected.role)
        before = identity(expected.role, 99) if self.restart_returns_stale_before else current
        new = ProcessIdentity(expected.role, current.pid + 100, current.start_time + 100)
        settingsd = (
            new
            if expected.role == "prismdrake-settingsd"
            else self.components_value.settingsd
        )
        notifyd = new if expected.role == "prismdrake-notifyd" else self.components_value.notifyd
        shell = new if expected.role == "prismdrake-shell" else self.components_value.shell
        if self.restart_replaces_sibling:
            sibling = (
                "prismdrake-notifyd"
                if expected.role != "prismdrake-notifyd"
                else "prismdrake-settingsd"
            )
            if sibling == "prismdrake-notifyd":
                notifyd = ProcessIdentity(
                    sibling, notifyd.pid + 300, notifyd.start_time + 300
                )
            else:
                settingsd = ProcessIdentity(
                    sibling, settingsd.pid + 300, settingsd.start_time + 300
                )
        self.components_value = ComponentObservation(settingsd, notifyd, shell)
        invalidated = expected.role in {"prismdrake-settingsd", "prismdrake-notifyd"}
        if self.restart_invalidated_override is not None:
            invalidated = self.restart_invalidated_override
        if expected.role == "prismdrake-notifyd":
            # The restart observation carries the already-observed invalidation
            # transition. The next state read represents reacquisition.
            self.notification_dependency_available = True
            self.notification_generation = 1
            self.notification_fingerprint = self.notification_fingerprint_after_restart
            self.notification_safe_degradation = True
        elif expected.role == "prismdrake-shell":
            self.notification_dependency_available = True
        self.last_restart = expected.role
        return RestartObservation(
            expected.role, before, new, self.components_value, invalidated
        )

    def settings_epoch(self) -> SettingsEpochObservation:
        self.calls.append("settings_epoch")
        observations = tuple(
            ReviewedSurfaceObservation(
                surface,
                bytes([index + 1]) * 32,
                (
                    bytes([index + 2]) * 32
                    if self.surface_unsettled and index == 1
                    else bytes([index + 1]) * 32
                ),
                not (self.surface_inaccessible and index == 1),
            )
            for index, surface in enumerate(SURFACES)
        )
        return SettingsEpochObservation(
            self.components_value.settingsd,
            self.components_value.shell,
            self.settings_generation,
            True,
            observations,
        )

    def notification_epoch(self) -> NotificationEpochObservation:
        self.calls.append("notification_epoch")
        return NotificationEpochObservation(
            self.components_value.notifyd,
            self.components_value.shell,
            self.notification_generation,
            self.notification_fingerprint,
            self.notification_record_count,
            self.notification_persistent,
            self.notification_safe_degradation,
            self.notification_dependency_available,
        )

    def window_authority(self) -> WindowAuthorityObservation:
        self.calls.append("window_authority")
        if self.preserve_windows:
            return self.window_value
        return WindowAuthorityObservation(
            self.window_value.window_manager,
            self.window_value.application_processes,
            (0x1001,),
        )

    def request_through_shellctl(self) -> LogoutObservation:
        self.calls.append("request_logout")
        self.transaction += 1
        return LogoutObservation(self.transaction, "confirmation_pending")

    def cancel(self, transaction_id: int) -> LogoutObservation:
        self.calls.append("cancel_logout")
        return LogoutObservation(transaction_id, "cancelled")

    def reject_stale_transaction(self, transaction_id: int) -> bool:
        self.calls.append("reject_stale_logout")
        return self.stale_transaction_rejected

    def confirm(self, transaction_id: int) -> LogoutObservation:
        self.calls.append("confirm_logout")
        self.confirmed_transaction = transaction_id
        self.shutdown_components = self.components_value
        return LogoutObservation(transaction_id, "stopping_shell")

    def shutdown_observation(self, transaction_id: int) -> ShutdownObservation:
        self.calls.append("shutdown_observation")
        assert self.shutdown_components is not None
        events = tuple(
            ShutdownEvent(
                role,
                self.shutdown_components.for_role(role),
                25 if self.shutdown_tied_elapsed else (index + 1) * 25,
            )
            for index, role in enumerate(self.shutdown_roles)
        )
        return ShutdownObservation(
            transaction_id,
            events,
            self.shutdown_total,
            self.shutdown_session_stopped,
        )

    def identities_alive(self, identities: tuple[ProcessIdentity, ...]) -> bool:
        self.calls.append("identities_alive")
        expected = (
            self.window_value.window_manager,
            *self.window_value.application_processes,
        )
        return identities == expected and self.unrelated_alive

    def observe_exact_environment_cleanup(self) -> CleanupObservation:
        self.calls.append("observe_cleanup")
        return self.cleanup_value


def workflow(ports: FakeInstalledPorts) -> InstalledLifecycleWorkflow:
    return InstalledLifecycleWorkflow(ports, ports, ports, ports, SURFACES)


def run_to(workflow_value: InstalledLifecycleWorkflow, final_step: str) -> list[WorkflowResult]:
    results: list[WorkflowResult] = []
    for step in STEP_CATEGORIES:
        results.append(workflow_value.run_step(step))
        if step == final_step:
            break
    return results


class InstalledLifecycleWorkflowTests(unittest.TestCase):
    def test_runs_exact_steps_with_only_categorical_evidence(self) -> None:
        ports = FakeInstalledPorts()
        results = run_to(workflow(ports), tuple(STEP_CATEGORIES)[-1])
        self.assertEqual([item.category for item in results], list(STEP_CATEGORIES.values()))
        for result in results:
            validate_workflow_result(result)
            self.assertEqual(set(result.evidence()), {"category", "status"})
        self.assertEqual(
            [call for call in ports.calls if call.startswith("restart:")],
            [
                "restart:prismdrake-settingsd",
                "restart:prismdrake-notifyd",
                "restart:prismdrake-shell",
            ],
        )

    def test_requires_nonempty_notification_and_application_survival_baseline(self) -> None:
        ports = FakeInstalledPorts()
        ports.notification_record_count = 0
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_notification_baseline_invalid"
        ):
            workflow(ports).run_step("remove_current_primary_output")

        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_window_state_invalid"):
            WindowAuthorityObservation(identity("window-manager", 2), (), (1,))

        ports = FakeInstalledPorts()
        ports.notification_persistent = False
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_notification_baseline_invalid"
        ):
            workflow(ports).run_step("remove_current_primary_output")

        ports = FakeInstalledPorts()
        ports.notification_safe_degradation = False
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_notification_baseline_invalid"
        ):
            workflow(ports).run_step("remove_current_primary_output")

    def test_rejects_unknown_out_of_order_and_post_failure_steps(self) -> None:
        instance = workflow(FakeInstalledPorts())
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_step_not_reviewed"):
            instance.run_step("capture_everything")
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_step_order_invalid"):
            instance.run_step("verify_surface_migration")
        ports = FakeInstalledPorts()
        ports.raise_private_error = True
        failed = workflow(ports)
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_observer_failed") as caught:
            failed.run_step("remove_current_primary_output")
        self.assertNotIn("private", str(caught.exception))
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_step_order_invalid"):
            failed.run_step("remove_current_primary_output")

    def test_requires_migration_off_removed_output(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        instance.run_step("remove_current_primary_output")
        ports.migration_incomplete = True
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_surface_placement_invalid"
        ):
            instance.run_step("verify_surface_migration")

    def test_rejects_offscreen_surface_after_migration(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        instance.run_step("remove_current_primary_output")
        ports.surface_offscreen = True
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_surface_placement_invalid"
        ):
            instance.run_step("verify_surface_migration")

    def test_rejects_stale_output_generation_and_connection_epoch(self) -> None:
        for attribute, category in (
            ("readd_stale_generation", "lifecycle_output_readd_invalid"),
            ("readd_stale_epoch", "lifecycle_output_readd_invalid"),
        ):
            with self.subTest(attribute=attribute):
                ports = FakeInstalledPorts()
                instance = workflow(ports)
                run_to(instance, "verify_surface_migration")
                setattr(ports, attribute, True)
                with self.assertRaisesRegex(LifecycleWorkflowError, category):
                    instance.run_step("readd_removed_output")

    def test_rejects_stale_process_identity_and_sibling_restart(self) -> None:
        for attribute, category in (
            ("restart_returns_stale_before", "lifecycle_stale_process_identity_accepted"),
            ("restart_replaces_sibling", "lifecycle_unrelated_component_restarted"),
        ):
            with self.subTest(attribute=attribute):
                ports = FakeInstalledPorts()
                instance = workflow(ports)
                run_to(instance, "readd_removed_output")
                setattr(ports, attribute, True)
                with self.assertRaisesRegex(LifecycleWorkflowError, category):
                    instance.run_step("restart_settingsd")

    def test_requires_complete_settings_generation(self) -> None:
        for attribute in ("surface_unsettled", "surface_inaccessible"):
            with self.subTest(attribute=attribute):
                ports = FakeInstalledPorts()
                instance = workflow(ports)
                run_to(instance, "restart_settingsd")
                setattr(ports, attribute, True)
                with self.assertRaisesRegex(
                    LifecycleWorkflowError, "lifecycle_settings_epoch_incomplete"
                ):
                    instance.run_step("verify_complete_settings_epoch_rebuild")

    def test_requires_notification_invalidation_and_durable_state_reacquisition(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "verify_complete_settings_epoch_rebuild")
        ports.restart_invalidated_override = False
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_dependent_invalidation_invalid"
        ):
            instance.run_step("restart_notifyd")

        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "verify_complete_settings_epoch_rebuild")
        ports.notification_fingerprint_after_restart = b"x" * 32
        instance.run_step("restart_notifyd")
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_notification_reacquisition_invalid"
        ):
            instance.run_step("verify_notification_state_reacquisition")

        for attribute in (
            "notification_persistent",
            "notification_safe_degradation",
        ):
            with self.subTest(attribute=attribute):
                ports = FakeInstalledPorts()
                instance = workflow(ports)
                run_to(instance, "verify_complete_settings_epoch_rebuild")
                instance.run_step("restart_notifyd")
                setattr(ports, attribute, False)
                with self.assertRaisesRegex(
                    LifecycleWorkflowError,
                    "lifecycle_notification_reacquisition_invalid",
                ):
                    instance.run_step("verify_notification_state_reacquisition")

        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "verify_complete_settings_epoch_rebuild")
        instance.run_step("restart_notifyd")
        ports.notification_record_count += 1
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_notification_reacquisition_invalid"
        ):
            instance.run_step("verify_notification_state_reacquisition")

    def test_requires_external_windows_after_shell_restart(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "restart_shell")
        ports.preserve_windows = False
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_external_state_not_preserved"
        ):
            instance.run_step("verify_windows_and_notification_state_survive")

    def test_cancelled_transaction_must_be_stale(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "request_logout_through_shellctl")
        ports.stale_transaction_rejected = False
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_logout_cancel_invalid"):
            instance.run_step("cancel_logout_once")

    def test_requires_exact_bounded_reverse_shutdown(self) -> None:
        for configure in (
            lambda ports: setattr(ports, "shutdown_roles", tuple(reversed(SHUTDOWN_ORDER))),
            lambda ports: setattr(
                ports, "shutdown_total", MAX_SHUTDOWN_MILLISECONDS + 1
            ),
            lambda ports: setattr(ports, "shutdown_session_stopped", False),
        ):
            ports = FakeInstalledPorts()
            instance = workflow(ports)
            run_to(instance, "request_and_confirm_logout")
            configure(ports)
            with self.assertRaisesRegex(
                LifecycleWorkflowError, "lifecycle_shutdown_not_bounded_reverse_order"
            ):
                instance.run_step("verify_reverse_order_prismdrake_shutdown")

    def test_accepts_equal_shutdown_timestamps_when_observed_order_is_exact(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "request_and_confirm_logout")
        ports.shutdown_tied_elapsed = True
        result = instance.run_step("verify_reverse_order_prismdrake_shutdown")
        self.assertEqual(result.category, "bounded_reverse_shutdown_observed")

    def test_requires_unrelated_survival_and_complete_cleanup(self) -> None:
        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "verify_reverse_order_prismdrake_shutdown")
        ports.unrelated_alive = False
        with self.assertRaisesRegex(
            LifecycleWorkflowError, "lifecycle_unrelated_process_terminated"
        ):
            instance.run_step("verify_unrelated_processes_survive")

        ports = FakeInstalledPorts()
        instance = workflow(ports)
        run_to(instance, "verify_unrelated_processes_survive")
        ports.cleanup_value = CleanupObservation(1, False, False)
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_cleanup_incomplete"):
            instance.run_step("verify_clean_test_environment_cleanup")

    def test_rejects_bool_and_duplicate_identity_fields(self) -> None:
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_process_identity_invalid"):
            ProcessIdentity("application", True, 4)
        duplicate = identity("application", 1)
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_window_state_invalid"):
            WindowAuthorityObservation(identity("window-manager", 2), (duplicate, duplicate), (1,))
        with self.assertRaisesRegex(LifecycleWorkflowError, "lifecycle_result_invalid"):
            validate_workflow_result(WorkflowResult("pid-1001"))

    def test_exact_identities_are_not_represented_or_serialized(self) -> None:
        process = identity("application", 55)
        self.assertNotIn(str(process.pid), repr(process))
        result = WorkflowResult("unrelated_processes_survived")
        self.assertNotIn(str(process.pid), str(result.evidence()))


if __name__ == "__main__":
    unittest.main()
