#pragma once

#include "Result.hpp"
#include "SdBusApi.hpp"
#include "ShellControlService.hpp"

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

class QSocketNotifier;

namespace prismdrake::shell::runtime {

/// Event-driven owner of the Experimental org.prismdrake.Shell1 session-bus name.
/// Dispatch is bounded per event-loop turn and never blocks the GUI thread waiting for messages.
class ShellControlBusOwner final : public QObject {
    Q_OBJECT

  public:
    using FailureHandler = std::function<void(const foundation::Error &)>;

    explicit ShellControlBusOwner(control::ShellPresentationPort &presentationPort,
                                  FailureHandler failureHandler = {}, QObject *parent = nullptr);
    ~ShellControlBusOwner() override;

    ShellControlBusOwner(const ShellControlBusOwner &) = delete;
    ShellControlBusOwner &operator=(const ShellControlBusOwner &) = delete;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept { return static_cast<bool>(bus_); }

  private:
    void dispatch() noexcept;
    void updateEventSources() noexcept;
    void fail() noexcept;

    control::ShellControlService service_;
    FailureHandler failure_handler_;
    ipc::sdbus::Bus bus_;
    std::unique_ptr<QSocketNotifier> read_notifier_;
    std::unique_ptr<QSocketNotifier> write_notifier_;
    QTimer timer_;
    bool dispatch_queued_{false};
};

} // namespace prismdrake::shell::runtime
