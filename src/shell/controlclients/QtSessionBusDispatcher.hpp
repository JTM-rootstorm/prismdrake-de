#pragma once

#include "Result.hpp"
#include "SdBusApi.hpp"

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

class QSocketNotifier;

namespace prismdrake::shell::controlclients::detail {

/// Qt event-source adapter for one private asynchronous session-bus connection.
class QtSessionBusDispatcher final : public QObject {
  public:
    using FailureCallback = std::function<void()>;

    explicit QtSessionBusDispatcher(FailureCallback failureCallback);
    ~QtSessionBusDispatcher() override;

    QtSessionBusDispatcher(const QtSessionBusDispatcher &) = delete;
    QtSessionBusDispatcher &operator=(const QtSessionBusDispatcher &) = delete;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;
    void refresh() noexcept;

    [[nodiscard]] sd_bus *get() const noexcept { return bus_.get(); }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(bus_); }

  private:
    void dispatch() noexcept;
    void fail() noexcept;

    FailureCallback failure_callback_;
    ipc::sdbus::Bus bus_;
    std::unique_ptr<QSocketNotifier> read_notifier_;
    std::unique_ptr<QSocketNotifier> write_notifier_;
    QTimer timer_;
    bool dispatch_queued_{false};
    bool failing_{false};
};

} // namespace prismdrake::shell::controlclients::detail
