#pragma once

#include "Result.hpp"

#include <QObject>

#include <csignal>
#include <functional>
#include <memory>

class QSocketNotifier;

namespace prismdrake::shell::runtime {

using SignalAction = struct sigaction;

/// Converts SIGINT and SIGTERM into one owner-thread callback through a non-blocking pipe.
///
/// The signal handler performs only an async-signal-safe write. Qt object teardown and shell
/// shutdown remain on the application thread.
class TerminationSignalBridge final : public QObject {
  public:
    using Callback = std::function<void()>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<TerminationSignalBridge>>
    create(Callback callback);
    ~TerminationSignalBridge() override;

    TerminationSignalBridge(const TerminationSignalBridge &) = delete;
    TerminationSignalBridge &operator=(const TerminationSignalBridge &) = delete;

  private:
    explicit TerminationSignalBridge(Callback callback);

    [[nodiscard]] foundation::Result<void> initialize();
    void drainSignalPipe();

    Callback callback_;
    std::unique_ptr<QSocketNotifier> notifier_;
    int read_fd_{-1};
    int write_fd_{-1};
    SignalAction previous_interrupt_{};
    SignalAction previous_terminate_{};
    bool interrupt_installed_{false};
    bool terminate_installed_{false};
    bool callback_requested_{false};
};

} // namespace prismdrake::shell::runtime
