#pragma once

#include "PanelStrutGeometry.hpp"
#include "Result.hpp"
#include "X11Types.hpp"

#include <QObject>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace prismdrake::x11 {
class X11Connection;
}

namespace prismdrake::shell::window {

enum class X11PropertyOperationKind : std::uint8_t {
    publishDock,
    removeDock,
    publishDesktop,
    removeDesktop,
    publishInitialSkipTaskbar,
};

struct X11PropertyOperation final {
    X11PropertyOperationKind kind;
    x11::WindowId window;
    std::optional<x11::BottomPanelStrut> dockReservation;
};

/// Dedicated reply-bearing X11 property worker for Qt-owned shell windows.
class X11PropertyWorker final {
  public:
    using Completion = std::function<void(std::uint64_t, foundation::Result<void>)>;
    /// Deterministic test boundary. Production callers leave this empty and use the worker-owned
    /// X11 connection and AtomCache.
    using Transaction = std::function<foundation::Result<void>(
        std::stop_token, std::span<const X11PropertyOperation>)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<X11PropertyWorker>>
    create(QObject &owner, std::string display, Completion completion,
           Transaction testTransaction = {});
    ~X11PropertyWorker();
    X11PropertyWorker(const X11PropertyWorker &) = delete;
    X11PropertyWorker &operator=(const X11PropertyWorker &) = delete;

    [[nodiscard]] foundation::Result<void> submit(std::uint64_t token,
                                                  std::vector<X11PropertyOperation> operations);
    /// Publishes one non-rollbackable policy to a bounded set of unpublished native windows.
    /// Retain every target through completion and discard all targets on failure.
    [[nodiscard]] foundation::Result<void>
    submitInitialSkipTaskbar(std::uint64_t token, std::vector<x11::WindowId> windows);

  private:
    struct Job final {
        std::uint64_t token;
        std::vector<X11PropertyOperation> operations;
        bool rollbackOnFailure;
    };

    X11PropertyWorker(QObject &owner, std::string display, Completion completion,
                      Transaction testTransaction);
    [[nodiscard]] foundation::Result<void> enqueue(std::uint64_t token,
                                                   std::vector<X11PropertyOperation> operations,
                                                   bool rollbackOnFailure);
    void run(std::stop_token stopToken);
    void deliver(std::uint64_t token, foundation::Result<void> result);

    QObject &owner_;
    std::string display_;
    Completion completion_;
    Transaction test_transaction_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Job> jobs_;
    bool stopping_{false};
    std::mutex connection_mutex_;
    x11::X11Connection *live_connection_{nullptr};
    std::jthread thread_;
};

} // namespace prismdrake::shell::window
