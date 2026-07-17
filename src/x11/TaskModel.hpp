#pragma once

#include "EwmhTaskList.hpp"
#include "Result.hpp"
#include "WindowMetadata.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace prismdrake::x11 {

inline constexpr std::size_t maximumTaskWindows = maximumEwmhTaskWindows;
inline constexpr std::size_t maximumTaskApplicationIdBytes = maximumWmClassBytes;
inline constexpr std::size_t maximumTaskFallbackIconNameBytes = 255U;

/// Decoder-assigned identity for one server-observed X window incarnation.
/// It must change when an XID is destroyed and later reused.
class WindowIncarnationId final {
  public:
    using Value = std::uint64_t;

    [[nodiscard]] static foundation::Result<WindowIncarnationId> fromObserved(Value value);
    [[nodiscard]] Value value() const noexcept { return value_; }

    friend bool operator==(const WindowIncarnationId &, const WindowIncarnationId &) = default;

  private:
    explicit WindowIncarnationId(Value value) noexcept : value_(value) {}
    Value value_;
};

/// Stable shell-model identity allocated once per observed window incarnation.
class TaskLifetimeId final {
  public:
    using Value = std::uint64_t;
    [[nodiscard]] Value value() const noexcept { return value_; }

    friend bool operator==(const TaskLifetimeId &, const TaskLifetimeId &) = default;

  private:
    friend class TaskModel;
    explicit TaskLifetimeId(Value value) noexcept : value_(value) {}
    Value value_;
};

/// Monotonic identity for one complete immutable task snapshot.
class TaskModelGeneration final {
  public:
    using Value = std::uint64_t;
    [[nodiscard]] Value value() const noexcept { return value_; }

    friend bool operator==(const TaskModelGeneration &, const TaskModelGeneration &) = default;

  private:
    friend class TaskModel;
    explicit TaskModelGeneration(Value value) noexcept : value_(value) {}
    Value value_;
};

/// One authoritative client tied to a decoder-observed XID incarnation.
/// A null metadata value explicitly excludes one invalid/stale entry while
/// retaining its lifetime tracking. A published record requires a fallback
/// icon name because full icon pixels remain owned by the metadata layer.
struct DecodedTaskObservation final {
    WindowId window;
    WindowIncarnationId incarnation;
    std::optional<WindowMetadata> metadata;
    std::string fallbackIconName;
};

/// One coherent root snapshot plus one complete bounded metadata result for
/// every advertised client. An incomplete property fetch must not be published.
struct TaskModelObservation final {
    EwmhTaskListSnapshot authoritative;
    std::vector<DecodedTaskObservation> windows;
};

class TaskRecord final {
  public:
    [[nodiscard]] WindowId window() const noexcept { return window_; }
    [[nodiscard]] WindowIncarnationId incarnation() const noexcept { return incarnation_; }
    [[nodiscard]] TaskLifetimeId lifetime() const noexcept { return lifetime_; }
    [[nodiscard]] const std::string &title() const noexcept { return title_; }
    [[nodiscard]] const std::string &applicationId() const noexcept { return application_id_; }
    [[nodiscard]] ApplicationIdentitySource applicationIdentitySource() const noexcept {
        return application_identity_source_;
    }
    [[nodiscard]] const std::string &fallbackIconName() const noexcept {
        return fallback_icon_name_;
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool hidden() const noexcept { return hidden_; }
    [[nodiscard]] bool urgent() const noexcept { return urgent_; }
    [[nodiscard]] const std::optional<std::uint32_t> &workspace() const noexcept {
        return workspace_;
    }
    [[nodiscard]] bool onAllWorkspaces() const noexcept { return on_all_workspaces_; }
    [[nodiscard]] WindowType type() const noexcept { return type_; }
    [[nodiscard]] const std::optional<TaskLifetimeId> &transientParent() const noexcept {
        return transient_parent_;
    }
    [[nodiscard]] bool modal() const noexcept { return modal_; }
    [[nodiscard]] TaskModelGeneration lastObservedGeneration() const noexcept {
        return last_observed_generation_;
    }

  private:
    friend class TaskModel;

    TaskRecord(WindowId window, WindowIncarnationId incarnation, TaskLifetimeId lifetime,
               std::string title, std::string applicationId,
               ApplicationIdentitySource identitySource, std::string fallbackIconName, bool active,
               bool hidden, bool urgent, std::optional<std::uint32_t> workspace,
               bool onAllWorkspaces, WindowType type, std::optional<TaskLifetimeId> transientParent,
               bool modal, TaskModelGeneration generation);

    WindowId window_;
    WindowIncarnationId incarnation_;
    TaskLifetimeId lifetime_;
    std::string title_;
    std::string application_id_;
    ApplicationIdentitySource application_identity_source_;
    std::string fallback_icon_name_;
    bool active_;
    bool hidden_;
    bool urgent_;
    std::optional<std::uint32_t> workspace_;
    bool on_all_workspaces_;
    WindowType type_;
    std::optional<TaskLifetimeId> transient_parent_;
    bool modal_;
    TaskModelGeneration last_observed_generation_;
};

struct AuthoritativeTaskClient final {
    WindowId window;
    WindowIncarnationId incarnation;

    friend bool operator==(const AuthoritativeTaskClient &,
                           const AuthoritativeTaskClient &) = default;
};

class TaskModelSnapshot final {
  public:
    [[nodiscard]] TaskModelGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] std::span<const TaskRecord> tasks() const noexcept { return tasks_; }
    [[nodiscard]] std::span<const AuthoritativeTaskClient> authoritativeClients() const noexcept {
        return authoritative_clients_;
    }
    [[nodiscard]] bool authoritativelyContains(WindowId window,
                                               WindowIncarnationId incarnation) const noexcept;

  private:
    friend class TaskModel;
    TaskModelSnapshot(TaskModelGeneration generation, std::vector<TaskRecord> tasks,
                      std::vector<AuthoritativeTaskClient> authoritativeClients);

    TaskModelGeneration generation_;
    std::vector<TaskRecord> tasks_;
    std::vector<AuthoritativeTaskClient> authoritative_clients_;
};

/// Display-free all-or-nothing mirror of authoritative decoded WM snapshots.
/// This mutable publication state has one owner and is not thread-safe.
class TaskModel final {
  public:
    TaskModel() = default;
    TaskModel(const TaskModel &) = delete;
    TaskModel &operator=(const TaskModel &) = delete;
    TaskModel(TaskModel &&) = delete;
    TaskModel &operator=(TaskModel &&) = delete;

    [[nodiscard]] foundation::Result<std::shared_ptr<const TaskModelSnapshot>>
    publish(const TaskModelObservation &observation);

    [[nodiscard]] std::shared_ptr<const TaskModelSnapshot> current() const noexcept {
        return current_;
    }

  private:
    struct TrackedWindow final {
        WindowIncarnationId incarnation;
        std::optional<TaskLifetimeId> lifetime;
    };

    std::map<WindowId::Value, TrackedWindow> tracked_;
    std::uint64_t next_lifetime_{1U};
    std::uint64_t next_generation_{1U};
    std::shared_ptr<const TaskModelSnapshot> current_;
};

} // namespace prismdrake::x11
