#!/usr/bin/env python3
"""Fail-closed installed lifecycle workflow for PD2 demonstration steps 29-43."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Protocol, TypeVar


UINT64_MAX = (1 << 64) - 1
MAX_NAME_BYTES = 64
MAX_OUTPUTS = 8
MAX_SURFACES = 16
MAX_APPLICATIONS = 16
MAX_WINDOWS = 64
MAX_SHUTDOWN_MILLISECONDS = 10_000
RESTARTABLE_ROLES = frozenset(
    {"prismdrake-settingsd", "prismdrake-notifyd", "prismdrake-shell"}
)
SHUTDOWN_ORDER = (
    "prismdrake-shell",
    "prismdrake-notifyd",
    "prismdrake-settingsd",
)
STEP_CATEGORIES = {
    "remove_current_primary_output": "primary_output_removed",
    "verify_surface_migration": "shell_surfaces_migrated",
    "readd_removed_output": "removed_output_readded",
    "restart_settingsd": "settingsd_exact_child_restarted",
    "verify_complete_settings_epoch_rebuild": "settings_epoch_rebuilt",
    "restart_notifyd": "notifyd_exact_child_restarted",
    "verify_notification_state_reacquisition": "notification_state_reacquired",
    "restart_shell": "shell_exact_child_restarted",
    "verify_windows_and_notification_state_survive": "external_state_survived",
    "request_logout_through_shellctl": "logout_confirmation_requested",
    "cancel_logout_once": "logout_cancelled",
    "request_and_confirm_logout": "logout_confirmed",
    "verify_reverse_order_prismdrake_shutdown": "bounded_reverse_shutdown_observed",
    "verify_unrelated_processes_survive": "unrelated_processes_survived",
    "verify_clean_test_environment_cleanup": "test_environment_cleaned",
}
CLOSED_ERROR_CATEGORIES = frozenset(
    {
        "lifecycle_cleanup_incomplete",
        "lifecycle_cleanup_invalid",
        "lifecycle_component_identity_invalid",
        "lifecycle_component_role_invalid",
        "lifecycle_dependent_invalidation_invalid",
        "lifecycle_external_state_not_preserved",
        "lifecycle_logout_cancel_invalid",
        "lifecycle_logout_confirmation_invalid",
        "lifecycle_logout_request_invalid",
        "lifecycle_logout_state_invalid",
        "lifecycle_logout_transaction_reused",
        "lifecycle_notification_baseline_invalid",
        "lifecycle_notification_epoch_invalid",
        "lifecycle_notification_invalidation_invalid",
        "lifecycle_notification_reacquisition_invalid",
        "lifecycle_observer_failed",
        "lifecycle_output_generation_stale",
        "lifecycle_output_identity_invalid",
        "lifecycle_output_readd_invalid",
        "lifecycle_primary_output_removal_invalid",
        "lifecycle_process_identity_invalid",
        "lifecycle_restart_invalid",
        "lifecycle_result_invalid",
        "lifecycle_settings_epoch_incomplete",
        "lifecycle_settings_epoch_invalid",
        "lifecycle_shutdown_not_bounded_reverse_order",
        "lifecycle_shutdown_timeout_invalid",
        "lifecycle_shutdown_trace_invalid",
        "lifecycle_stale_process_identity_accepted",
        "lifecycle_step_not_reviewed",
        "lifecycle_step_order_invalid",
        "lifecycle_surface_migration_incomplete",
        "lifecycle_surface_placement_invalid",
        "lifecycle_surface_set_changed",
        "lifecycle_surface_set_invalid",
        "lifecycle_surface_topology_stale",
        "lifecycle_topology_invalid",
        "lifecycle_two_output_baseline_missing",
        "lifecycle_unrelated_component_restarted",
        "lifecycle_unrelated_process_terminated",
        "lifecycle_window_state_invalid",
    }
)


class LifecycleWorkflowError(RuntimeError):
    """A fixed categorical lifecycle failure with no captured private state."""


def _fail(category: str) -> None:
    raise LifecycleWorkflowError(category) from None


def _uint(value: Any, category: str, *, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if type(value) is not int or not minimum <= value <= UINT64_MAX:
        _fail(category)
    return value


def _name(value: Any, category: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > MAX_NAME_BYTES
        or not value.isascii()
        or not value.replace("-", "").replace("_", "").isalnum()
    ):
        _fail(category)
    return value


def _strict_tuple(value: Any, maximum: int, category: str) -> tuple[Any, ...]:
    if type(value) is not tuple or not value or len(value) > maximum:
        _fail(category)
    return value


@dataclass(frozen=True, slots=True, repr=False)
class ProcessIdentity:
    """Exact process identity. It is intentionally absent from result evidence."""

    role: str
    pid: int
    start_time: int

    def __post_init__(self) -> None:
        _name(self.role, "lifecycle_process_identity_invalid")
        _uint(self.pid, "lifecycle_process_identity_invalid")
        _uint(self.start_time, "lifecycle_process_identity_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class OutputIdentity:
    name: str
    connection_epoch: int
    active: bool
    primary: bool

    def __post_init__(self) -> None:
        _name(self.name, "lifecycle_output_identity_invalid")
        _uint(self.connection_epoch, "lifecycle_output_identity_invalid")
        if type(self.active) is not bool or type(self.primary) is not bool:
            _fail("lifecycle_output_identity_invalid")
        if self.primary and not self.active:
            _fail("lifecycle_output_identity_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class TopologyObservation:
    generation: int
    outputs: tuple[OutputIdentity, ...]

    def __post_init__(self) -> None:
        _uint(self.generation, "lifecycle_topology_invalid")
        outputs = _strict_tuple(self.outputs, MAX_OUTPUTS, "lifecycle_topology_invalid")
        if any(type(item) is not OutputIdentity for item in outputs):
            _fail("lifecycle_topology_invalid")
        if len({item.name for item in outputs}) != len(outputs):
            _fail("lifecycle_topology_invalid")
        primary = tuple(item for item in outputs if item.primary)
        if len(primary) != 1:
            _fail("lifecycle_topology_invalid")

    @property
    def primary(self) -> OutputIdentity:
        return next(item for item in self.outputs if item.primary)


@dataclass(frozen=True, slots=True, repr=False)
class SurfacePlacement:
    surface: str
    output: OutputIdentity
    visible: bool
    fully_within_output: bool

    def __post_init__(self) -> None:
        _name(self.surface, "lifecycle_surface_placement_invalid")
        if (
            type(self.output) is not OutputIdentity
            or not self.output.active
            or type(self.visible) is not bool
            or type(self.fully_within_output) is not bool
        ):
            _fail("lifecycle_surface_placement_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class SurfacePlacementObservation:
    topology_generation: int
    placements: tuple[SurfacePlacement, ...]

    def __post_init__(self) -> None:
        _uint(self.topology_generation, "lifecycle_surface_placement_invalid")
        placements = _strict_tuple(
            self.placements, MAX_SURFACES, "lifecycle_surface_placement_invalid"
        )
        if any(type(item) is not SurfacePlacement for item in placements):
            _fail("lifecycle_surface_placement_invalid")
        if len({item.surface for item in placements}) != len(placements):
            _fail("lifecycle_surface_placement_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class ComponentObservation:
    settingsd: ProcessIdentity
    notifyd: ProcessIdentity
    shell: ProcessIdentity

    def __post_init__(self) -> None:
        identities = (self.settingsd, self.notifyd, self.shell)
        if any(type(item) is not ProcessIdentity for item in identities):
            _fail("lifecycle_component_identity_invalid")
        if tuple(item.role for item in identities) != (
            "prismdrake-settingsd",
            "prismdrake-notifyd",
            "prismdrake-shell",
        ) or len({(item.pid, item.start_time) for item in identities}) != 3:
            _fail("lifecycle_component_identity_invalid")

    def for_role(self, role: str) -> ProcessIdentity:
        return {
            "prismdrake-settingsd": self.settingsd,
            "prismdrake-notifyd": self.notifyd,
            "prismdrake-shell": self.shell,
        }.get(role) or _fail("lifecycle_component_role_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class RestartObservation:
    """Exact replacement plus an observed dependent-model invalidation edge."""

    role: str
    before: ProcessIdentity
    after: ProcessIdentity
    components: ComponentObservation
    dependent_invalidated: bool

    def __post_init__(self) -> None:
        _name(self.role, "lifecycle_restart_invalid")
        if self.role not in RESTARTABLE_ROLES:
            _fail("lifecycle_restart_invalid")
        if (
            type(self.before) is not ProcessIdentity
            or type(self.after) is not ProcessIdentity
            or type(self.components) is not ComponentObservation
            or type(self.dependent_invalidated) is not bool
            or self.before.role != self.role
            or self.after.role != self.role
            or self.before == self.after
            or self.components.for_role(self.role) != self.after
        ):
            _fail("lifecycle_restart_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class ReviewedSurfaceObservation:
    """Stable reviewed pixels plus AT-SPI recovery for one required surface."""

    surface: str
    first_fingerprint: bytes
    second_fingerprint: bytes
    accessible: bool

    def __post_init__(self) -> None:
        _name(self.surface, "lifecycle_settings_epoch_invalid")
        if (
            not isinstance(self.first_fingerprint, bytes)
            or len(self.first_fingerprint) != 32
            or not isinstance(self.second_fingerprint, bytes)
            or len(self.second_fingerprint) != 32
            or type(self.accessible) is not bool
        ):
            _fail("lifecycle_settings_epoch_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class SettingsEpochObservation:
    authority: ProcessIdentity
    shell: ProcessIdentity
    generation: int
    complete_typed_snapshot: bool
    reviewed_surfaces: tuple[ReviewedSurfaceObservation, ...]

    def __post_init__(self) -> None:
        if (
            type(self.authority) is not ProcessIdentity
            or self.authority.role != "prismdrake-settingsd"
            or type(self.shell) is not ProcessIdentity
            or self.shell.role != "prismdrake-shell"
            or type(self.complete_typed_snapshot) is not bool
        ):
            _fail("lifecycle_settings_epoch_invalid")
        _uint(self.generation, "lifecycle_settings_epoch_invalid")
        surfaces = _strict_tuple(
            self.reviewed_surfaces, MAX_SURFACES, "lifecycle_settings_epoch_invalid"
        )
        if (
            any(type(item) is not ReviewedSurfaceObservation for item in surfaces)
            or len({item.surface for item in surfaces}) != len(surfaces)
        ):
            _fail("lifecycle_settings_epoch_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class NotificationEpochObservation:
    """Authority plus durable and safely degraded notification projections.

    The fingerprint covers only durable record identities, generations,
    urgency classes, and action identifiers. It excludes live notification
    identifiers, transient presentation, action availability, titles, bodies,
    paths, application arguments, and all other captured content.
    """

    authority: ProcessIdentity
    shell: ProcessIdentity
    state_generation: int
    durable_fingerprint: bytes
    durable_record_count: int
    persistent_history: bool
    safe_runtime_degradation: bool
    shell_dependency_available: bool

    def __post_init__(self) -> None:
        if (
            type(self.authority) is not ProcessIdentity
            or self.authority.role != "prismdrake-notifyd"
            or type(self.shell) is not ProcessIdentity
            or self.shell.role != "prismdrake-shell"
            or not isinstance(self.durable_fingerprint, bytes)
            or len(self.durable_fingerprint) != 32
            or type(self.persistent_history) is not bool
            or type(self.safe_runtime_degradation) is not bool
            or type(self.shell_dependency_available) is not bool
        ):
            _fail("lifecycle_notification_epoch_invalid")
        _uint(self.state_generation, "lifecycle_notification_epoch_invalid")
        _uint(
            self.durable_record_count,
            "lifecycle_notification_epoch_invalid",
            allow_zero=True,
        )


@dataclass(frozen=True, slots=True, repr=False)
class WindowAuthorityObservation:
    window_manager: ProcessIdentity
    application_processes: tuple[ProcessIdentity, ...]
    window_tokens: tuple[int, ...]

    def __post_init__(self) -> None:
        if (
            type(self.window_manager) is not ProcessIdentity
            or self.window_manager.role != "window-manager"
        ):
            _fail("lifecycle_window_state_invalid")
        applications = _strict_tuple(
            self.application_processes, MAX_APPLICATIONS, "lifecycle_window_state_invalid"
        )
        windows = _strict_tuple(
            self.window_tokens, MAX_WINDOWS, "lifecycle_window_state_invalid"
        )
        if (
            any(type(item) is not ProcessIdentity for item in applications)
            or any(item.role != "application" for item in applications)
            or len(set(applications)) != len(applications)
            or len(
                {
                    (item.pid, item.start_time)
                    for item in (self.window_manager, *applications)
                }
            )
            != len(applications) + 1
            or any(type(item) is not int or not 1 <= item <= UINT64_MAX for item in windows)
            or len(set(windows)) != len(windows)
        ):
            _fail("lifecycle_window_state_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class LogoutObservation:
    transaction_id: int
    state: str

    def __post_init__(self) -> None:
        _uint(self.transaction_id, "lifecycle_logout_state_invalid")
        if self.state not in {"confirmation_pending", "cancelled", "stopping_shell"}:
            _fail("lifecycle_logout_state_invalid")


@dataclass(frozen=True, slots=True, repr=False)
class ShutdownEvent:
    role: str
    identity: ProcessIdentity
    elapsed_milliseconds: int

    def __post_init__(self) -> None:
        if self.role not in SHUTDOWN_ORDER or self.identity.role != self.role:
            _fail("lifecycle_shutdown_trace_invalid")
        _uint(
            self.elapsed_milliseconds,
            "lifecycle_shutdown_trace_invalid",
            allow_zero=True,
        )


@dataclass(frozen=True, slots=True, repr=False)
class ShutdownObservation:
    transaction_id: int
    events: tuple[ShutdownEvent, ...]
    total_milliseconds: int
    session_stopped: bool

    def __post_init__(self) -> None:
        _uint(self.transaction_id, "lifecycle_shutdown_trace_invalid")
        events = _strict_tuple(self.events, 3, "lifecycle_shutdown_trace_invalid")
        if any(type(item) is not ShutdownEvent for item in events):
            _fail("lifecycle_shutdown_trace_invalid")
        _uint(
            self.total_milliseconds,
            "lifecycle_shutdown_trace_invalid",
            allow_zero=True,
        )
        if type(self.session_stopped) is not bool:
            _fail("lifecycle_shutdown_trace_invalid")


@dataclass(frozen=True, slots=True)
class CleanupObservation:
    exact_test_processes_remaining: int
    private_runtime_present: bool
    private_bus_present: bool

    def __post_init__(self) -> None:
        _uint(
            self.exact_test_processes_remaining,
            "lifecycle_cleanup_invalid",
            allow_zero=True,
        )
        if (
            type(self.private_runtime_present) is not bool
            or type(self.private_bus_present) is not bool
        ):
            _fail("lifecycle_cleanup_invalid")


@dataclass(frozen=True, slots=True)
class WorkflowResult:
    category: str
    status: str = "passed"

    def evidence(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


class OutputPort(Protocol):
    def topology(self) -> TopologyObservation: ...

    def surface_placements(self) -> SurfacePlacementObservation: ...

    def remove_output(self, output: OutputIdentity, topology_generation: int) -> None: ...

    def readd_output(self, output: OutputIdentity, topology_generation: int) -> None: ...


class ServicePort(Protocol):
    def prepare_survival_fixture(self) -> WindowAuthorityObservation: ...

    def components(self) -> ComponentObservation: ...

    def restart_exact(self, identity: ProcessIdentity) -> RestartObservation: ...

    def settings_epoch(self) -> SettingsEpochObservation: ...

    def notification_epoch(self) -> NotificationEpochObservation: ...

    def window_authority(self) -> WindowAuthorityObservation: ...


class LogoutPort(Protocol):
    def request_through_shellctl(self) -> LogoutObservation: ...

    def cancel(self, transaction_id: int) -> LogoutObservation: ...

    def reject_stale_transaction(self, transaction_id: int) -> bool: ...

    def confirm(self, transaction_id: int) -> LogoutObservation: ...

    def shutdown_observation(self, transaction_id: int) -> ShutdownObservation: ...

    def identities_alive(self, identities: tuple[ProcessIdentity, ...]) -> bool: ...


class CleanupPort(Protocol):
    def observe_exact_environment_cleanup(self) -> CleanupObservation: ...


class _Stage(IntEnum):
    initial = 0
    output_removed = 1
    surfaces_migrated = 2
    output_readded = 3
    settingsd_restarted = 4
    settings_rebuilt = 5
    notifyd_restarted = 6
    notifications_reacquired = 7
    shell_restarted = 8
    external_state_verified = 9
    logout_requested = 10
    logout_cancelled = 11
    logout_confirmed = 12
    shutdown_verified = 13
    unrelated_verified = 14
    complete = 15
    failed = 16


T = TypeVar("T")


class InstalledLifecycleWorkflow:
    """Exact-order, privacy-minimized workflow for installed steps 29-43."""

    def __init__(
        self,
        outputs: OutputPort,
        services: ServicePort,
        logout: LogoutPort,
        cleanup: CleanupPort,
        reviewed_surfaces: tuple[str, ...],
        *,
        shutdown_timeout_milliseconds: int = MAX_SHUTDOWN_MILLISECONDS,
    ) -> None:
        surfaces = _strict_tuple(
            reviewed_surfaces, MAX_SURFACES, "lifecycle_surface_set_invalid"
        )
        if (
            any(_name(item, "lifecycle_surface_set_invalid") != item for item in surfaces)
            or len(set(surfaces)) != len(surfaces)
        ):
            _fail("lifecycle_surface_set_invalid")
        if (
            type(shutdown_timeout_milliseconds) is not int
            or not 1 <= shutdown_timeout_milliseconds <= MAX_SHUTDOWN_MILLISECONDS
        ):
            _fail("lifecycle_shutdown_timeout_invalid")
        self._outputs = outputs
        self._services = services
        self._logout = logout
        self._cleanup = cleanup
        self._reviewed_surfaces = surfaces
        self._shutdown_timeout = shutdown_timeout_milliseconds
        self._stage = _Stage.initial
        self._removed_output: OutputIdentity | None = None
        self._removed_topology: TopologyObservation | None = None
        self._components: ComponentObservation | None = None
        self._notification: NotificationEpochObservation | None = None
        self._windows: WindowAuthorityObservation | None = None
        self._logout_transaction: int | None = None
        self._shutdown_components: ComponentObservation | None = None
        self._unrelated: tuple[ProcessIdentity, ...] | None = None

    def run_step(self, identifier: str) -> WorkflowResult:
        identifiers = tuple(STEP_CATEGORIES)
        if identifier not in STEP_CATEGORIES:
            self._raise_clean("lifecycle_step_not_reviewed")
        index = int(self._stage)
        if index >= len(identifiers) or identifier != identifiers[index]:
            self._raise_clean("lifecycle_step_order_invalid")
        try:
            self._run_reviewed(identifier)
        except LifecycleWorkflowError as error:
            self._stage = _Stage.failed
            self._raise_clean(str(error))
        except Exception:
            self._stage = _Stage.failed
            self._raise_clean("lifecycle_observer_failed")
        self._stage = _Stage(self._stage + 1)
        return WorkflowResult(STEP_CATEGORIES[identifier])

    @staticmethod
    def _raise_clean(category: str) -> None:
        if category not in CLOSED_ERROR_CATEGORIES:
            category = "lifecycle_observer_failed"
        error = LifecycleWorkflowError(category)
        error.__cause__ = None
        error.__context__ = None
        raise error from None

    def _run_reviewed(self, identifier: str) -> None:
        operations: dict[str, Callable[[], None]] = {
            "remove_current_primary_output": self._remove_primary,
            "verify_surface_migration": self._verify_migration,
            "readd_removed_output": self._readd_output,
            "restart_settingsd": self._restart_settingsd,
            "verify_complete_settings_epoch_rebuild": self._verify_settings,
            "restart_notifyd": self._restart_notifyd,
            "verify_notification_state_reacquisition": self._verify_notifications,
            "restart_shell": self._restart_shell,
            "verify_windows_and_notification_state_survive": self._verify_external_state,
            "request_logout_through_shellctl": self._request_logout,
            "cancel_logout_once": self._cancel_logout,
            "request_and_confirm_logout": self._confirm_logout,
            "verify_reverse_order_prismdrake_shutdown": self._verify_shutdown,
            "verify_unrelated_processes_survive": self._verify_unrelated,
            "verify_clean_test_environment_cleanup": self._verify_cleanup,
        }
        operations[identifier]()

    def _remove_primary(self) -> None:
        topology = self._outputs.topology()
        active = tuple(item for item in topology.outputs if item.active)
        if len(active) < 2:
            _fail("lifecycle_two_output_baseline_missing")
        placements = self._outputs.surface_placements()
        self._validate_placements(placements, topology)
        components = self._services.components()
        notification = self._services.notification_epoch()
        # Steps 15-16 intentionally closed the earlier controlled windows.
        # The live adapter must create a dedicated nonempty fixture here; that
        # exact WM/application/window baseline is reused for steps 37 and 42.
        windows = self._services.prepare_survival_fixture()
        if (
            notification.authority != components.notifyd
            or notification.shell != components.shell
            or not notification.shell_dependency_available
            or not notification.persistent_history
            or not notification.safe_runtime_degradation
            or notification.durable_record_count == 0
        ):
            _fail("lifecycle_notification_baseline_invalid")
        self._outputs.remove_output(topology.primary, topology.generation)
        after = self._outputs.topology()
        if after.generation <= topology.generation:
            _fail("lifecycle_output_generation_stale")
        old = {item.name: item for item in topology.outputs}
        current = {item.name: item for item in after.outputs}
        removed = current.get(topology.primary.name)
        if (
            removed is None
            or removed.active
            or removed.primary
            or removed.connection_epoch != topology.primary.connection_epoch
            or any(
                name != topology.primary.name
                and (
                    current.get(name) is None
                    or current[name].connection_epoch != identity.connection_epoch
                    or current[name].active != identity.active
                )
                for name, identity in old.items()
            )
        ):
            _fail("lifecycle_primary_output_removal_invalid")
        self._removed_output = topology.primary
        self._removed_topology = after
        self._components = components
        self._notification = notification
        self._windows = windows
        self._unrelated = (windows.window_manager, *windows.application_processes)

    def _validate_placements(
        self, observation: SurfacePlacementObservation, topology: TopologyObservation
    ) -> None:
        if type(observation) is not SurfacePlacementObservation:
            _fail("lifecycle_surface_placement_invalid")
        if observation.topology_generation != topology.generation:
            _fail("lifecycle_surface_topology_stale")
        if tuple(item.surface for item in observation.placements) != self._reviewed_surfaces:
            _fail("lifecycle_surface_set_changed")
        active = {item for item in topology.outputs if item.active}
        if any(
            item.output not in active or not item.visible or not item.fully_within_output
            for item in observation.placements
        ):
            _fail("lifecycle_surface_placement_invalid")

    def _verify_migration(self) -> None:
        topology = self._required(self._removed_topology, "lifecycle_step_order_invalid")
        placements = self._outputs.surface_placements()
        self._validate_placements(placements, topology)
        removed = self._required(self._removed_output, "lifecycle_step_order_invalid")
        if any(item.output.name == removed.name for item in placements.placements):
            _fail("lifecycle_surface_migration_incomplete")

    def _readd_output(self) -> None:
        removed = self._required(self._removed_output, "lifecycle_step_order_invalid")
        topology = self._required(self._removed_topology, "lifecycle_step_order_invalid")
        self._outputs.readd_output(removed, topology.generation)
        after = self._outputs.topology()
        restored = next((item for item in after.outputs if item.name == removed.name), None)
        if (
            after.generation <= topology.generation
            or restored is None
            or not restored.active
            or restored.connection_epoch <= removed.connection_epoch
        ):
            _fail("lifecycle_output_readd_invalid")
        self._validate_placements(self._outputs.surface_placements(), after)

    def _restart(
        self, role: str, *, require_invalidation: bool
    ) -> RestartObservation:
        before = self._required(self._components, "lifecycle_step_order_invalid")
        identity = before.for_role(role)
        restart = self._services.restart_exact(identity)
        if type(restart) is not RestartObservation or restart.before != identity:
            _fail("lifecycle_stale_process_identity_accepted")
        if restart.dependent_invalidated is not require_invalidation:
            _fail("lifecycle_dependent_invalidation_invalid")
        for sibling in RESTARTABLE_ROLES - {role}:
            if restart.components.for_role(sibling) != before.for_role(sibling):
                _fail("lifecycle_unrelated_component_restarted")
        self._components = restart.components
        return restart

    def _restart_settingsd(self) -> None:
        self._restart("prismdrake-settingsd", require_invalidation=True)

    def _verify_settings(self) -> None:
        components = self._required(self._components, "lifecycle_step_order_invalid")
        epoch = self._services.settings_epoch()
        if (
            type(epoch) is not SettingsEpochObservation
            or epoch.authority != components.settingsd
            or epoch.shell != components.shell
            or not epoch.complete_typed_snapshot
            or tuple(item.surface for item in epoch.reviewed_surfaces)
            != self._reviewed_surfaces
            or any(
                not item.accessible
                or item.first_fingerprint != item.second_fingerprint
                for item in epoch.reviewed_surfaces
            )
        ):
            _fail("lifecycle_settings_epoch_incomplete")

    def _restart_notifyd(self) -> None:
        # ``restart_exact`` must observe the shell's invalidation transition;
        # it is not valid to race a later snapshot and infer that a gap existed.
        self._restart("prismdrake-notifyd", require_invalidation=True)

    def _verify_notifications(self) -> None:
        components = self._required(self._components, "lifecycle_step_order_invalid")
        prior = self._required(self._notification, "lifecycle_step_order_invalid")
        current = self._services.notification_epoch()
        if (
            current.authority != components.notifyd
            or current.shell != components.shell
            or not current.shell_dependency_available
            or not current.persistent_history
            or not current.safe_runtime_degradation
            or current.durable_fingerprint != prior.durable_fingerprint
            or current.durable_record_count != prior.durable_record_count
        ):
            _fail("lifecycle_notification_reacquisition_invalid")
        self._notification = current

    def _restart_shell(self) -> None:
        self._restart("prismdrake-shell", require_invalidation=False)

    def _verify_external_state(self) -> None:
        components = self._required(self._components, "lifecycle_step_order_invalid")
        notification = self._required(self._notification, "lifecycle_step_order_invalid")
        windows = self._required(self._windows, "lifecycle_step_order_invalid")
        current_notification = self._services.notification_epoch()
        current_windows = self._services.window_authority()
        if (
            current_notification.authority != components.notifyd
            or current_notification.shell != components.shell
            or not current_notification.shell_dependency_available
            or not current_notification.persistent_history
            or not current_notification.safe_runtime_degradation
            or current_notification.durable_fingerprint
            != notification.durable_fingerprint
            or current_notification.durable_record_count
            != notification.durable_record_count
            or current_windows != windows
        ):
            _fail("lifecycle_external_state_not_preserved")

    def _request_logout(self) -> None:
        request = self._logout.request_through_shellctl()
        if type(request) is not LogoutObservation or request.state != "confirmation_pending":
            _fail("lifecycle_logout_request_invalid")
        self._logout_transaction = request.transaction_id

    def _cancel_logout(self) -> None:
        transaction = self._required(
            self._logout_transaction, "lifecycle_step_order_invalid"
        )
        cancelled = self._logout.cancel(transaction)
        if (
            type(cancelled) is not LogoutObservation
            or cancelled.transaction_id != transaction
            or cancelled.state != "cancelled"
            or self._logout.reject_stale_transaction(transaction) is not True
        ):
            _fail("lifecycle_logout_cancel_invalid")

    def _confirm_logout(self) -> None:
        previous = self._required(self._logout_transaction, "lifecycle_step_order_invalid")
        request = self._logout.request_through_shellctl()
        if (
            type(request) is not LogoutObservation
            or request.state != "confirmation_pending"
            or request.transaction_id == previous
        ):
            _fail("lifecycle_logout_transaction_reused")
        components = self._required(self._components, "lifecycle_step_order_invalid")
        confirmed = self._logout.confirm(request.transaction_id)
        if (
            type(confirmed) is not LogoutObservation
            or confirmed.transaction_id != request.transaction_id
            or confirmed.state != "stopping_shell"
        ):
            _fail("lifecycle_logout_confirmation_invalid")
        self._logout_transaction = request.transaction_id
        self._shutdown_components = components

    def _verify_shutdown(self) -> None:
        transaction = self._required(
            self._logout_transaction, "lifecycle_step_order_invalid"
        )
        components = self._required(
            self._shutdown_components, "lifecycle_step_order_invalid"
        )
        observation = self._logout.shutdown_observation(transaction)
        if type(observation) is not ShutdownObservation:
            _fail("lifecycle_shutdown_trace_invalid")
        elapsed = tuple(event.elapsed_milliseconds for event in observation.events)
        if (
            observation.transaction_id != transaction
            or tuple(event.role for event in observation.events) != SHUTDOWN_ORDER
            or tuple(event.identity for event in observation.events)
            != tuple(components.for_role(role) for role in SHUTDOWN_ORDER)
            or any(
                event.elapsed_milliseconds > self._shutdown_timeout
                for event in observation.events
            )
            or elapsed != tuple(sorted(elapsed))
            or elapsed[-1] > observation.total_milliseconds
            or observation.total_milliseconds > self._shutdown_timeout
            or not observation.session_stopped
        ):
            _fail("lifecycle_shutdown_not_bounded_reverse_order")

    def _verify_unrelated(self) -> None:
        identities = self._required(self._unrelated, "lifecycle_step_order_invalid")
        if self._logout.identities_alive(identities) is not True:
            _fail("lifecycle_unrelated_process_terminated")

    def _verify_cleanup(self) -> None:
        # ``pd2_demo.run_harness`` delegates cleanup to the live port rather
        # than dispatching step 43. The adapter performs teardown once, then
        # invokes this final workflow step with a post-teardown observer.
        cleanup = self._cleanup.observe_exact_environment_cleanup()
        if (
            type(cleanup) is not CleanupObservation
            or cleanup.exact_test_processes_remaining != 0
            or cleanup.private_runtime_present
            or cleanup.private_bus_present
        ):
            _fail("lifecycle_cleanup_incomplete")

    @staticmethod
    def _required(value: T | None, category: str) -> T:
        if value is None:
            _fail(category)
        return value


def validate_workflow_result(result: Any) -> None:
    """Reject evidence carrying anything except a reviewed categorical result."""

    if (
        type(result) is not WorkflowResult
        or result.category not in STEP_CATEGORIES.values()
        or result.status != "passed"
        or result.evidence() != {"category": result.category, "status": "passed"}
    ):
        _fail("lifecycle_result_invalid")
