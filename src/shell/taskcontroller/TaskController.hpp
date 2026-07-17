#pragma once

#include "EwmhTaskSource.hpp"
#include "EwmhWindowRequests.hpp"
#include "Result.hpp"
#include "RootEventStream.hpp"
#include "TaskControllerCore.hpp"
#include "X11Connection.hpp"

#include <QMetaObject>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

class QSocketNotifier;
class QObject;

namespace prismdrake::shell::taskcontroller {

/// Event-driven standards-only X11 task controller for the development shell.
///
/// This object owns one X11 connection and its sole RootEventStream, mirrors EWMH state through
/// EwmhTaskSource, and sends only checked EWMH/ICCCM requests. It never polls or claims WM state.
/// The supplied presentation must outlive the controller.
class TaskController final {
  public:
    using ConnectionLostCallback = std::function<void(const foundation::Error &)>;
    using FailureCallback = TaskControllerCore::FailureCallback;
    using OutcomeCallback = TaskControllerCore::OutcomeCallback;

    [[nodiscard]] static foundation::Result<std::unique_ptr<TaskController>>
    create(tasks::TaskPresentationModel &presentation, std::string_view display,
           ConnectionLostCallback connectionLost, FailureCallback recoverableFailure = {},
           OutcomeCallback outcome = {});

    ~TaskController();
    TaskController(const TaskController &) = delete;
    TaskController &operator=(const TaskController &) = delete;

    [[nodiscard]] bool terminated() const noexcept { return terminated_; }
    [[nodiscard]] std::shared_ptr<const x11::TaskModelSnapshot> currentSnapshot() const noexcept {
        return core_.currentSnapshot();
    }
    [[nodiscard]] std::size_t pendingRequestCount() const noexcept {
        return core_.pendingRequestCount();
    }

  private:
    TaskController(tasks::TaskPresentationModel &presentation, x11::X11Connection connection,
                   x11::RootEventStream events, x11::EwmhTaskSource source,
                   std::optional<x11::EwmhWindowRequests> requests,
                   ConnectionLostCallback connectionLost, FailureCallback recoverableFailure,
                   OutcomeCallback outcome);

    [[nodiscard]] foundation::Result<void> refreshTasks();
    [[nodiscard]] foundation::Result<void>
    dispatchCheckedRequest(const x11::TaskRequestState &request);
    void drainEvents();
    void scheduleDrain();
    void reportRecoverable(const foundation::Error &error) const;
    void terminateForConnectionLoss(const foundation::Error &error);

    tasks::TaskPresentationModel &presentation_;
    ConnectionLostCallback connection_lost_;
    FailureCallback recoverable_failure_;
    TaskControllerCore core_;
    x11::X11Connection connection_;
    x11::RootEventStream events_;
    x11::EwmhTaskSource source_;
    std::optional<x11::EwmhWindowRequests> requests_;
    std::unique_ptr<QObject> event_context_;
    std::unique_ptr<QSocketNotifier> event_notifier_;
    bool drain_scheduled_{false};
    bool terminated_{false};
};

} // namespace prismdrake::shell::taskcontroller
