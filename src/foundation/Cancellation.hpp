#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace prismdrake::foundation {

namespace detail {

struct CancellationState final {
    std::atomic_bool requested{false};
};

} // namespace detail

/// Copyable observer for one explicitly owned cancellation state.
class CancellationToken final {
  public:
    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return state_->requested.load(std::memory_order_acquire);
    }

  private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<const detail::CancellationState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<const detail::CancellationState> state_;
};

/// Sole cancellation initiator for tokens sharing one atomic state.
class CancellationSource final {
  public:
    CancellationSource() : state_(std::make_shared<detail::CancellationState>()) {}

    CancellationSource(const CancellationSource &) = delete;
    CancellationSource &operator=(const CancellationSource &) = delete;
    CancellationSource(CancellationSource &&) = delete;
    CancellationSource &operator=(CancellationSource &&) = delete;

    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }

    /// Returns true only for the call that changes the state to requested.
    [[nodiscard]] bool requestCancellation() noexcept {
        bool expected = false;
        return state_->requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                         std::memory_order_acquire);
    }

  private:
    std::shared_ptr<detail::CancellationState> state_;
};

} // namespace prismdrake::foundation
