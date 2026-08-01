#include "SingletonTaskPolicyGate.hpp"

namespace prismdrake::shell::runtime {
namespace {

[[nodiscard]] foundation::Result<void> invalidTransition() {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument,
         "The singleton task-policy publication transition is invalid.",
         "Keep one non-zero pre-map publication in flight at a time."});
}

} // namespace

foundation::Result<void> SingletonTaskPolicyGate::begin(std::uint64_t token) {
    if (token == 0U || token_) {
        return invalidTransition();
    }
    token_ = token;
    invalidated_ = false;
    ready_ = false;
    return foundation::Result<void>::success();
}

void SingletonTaskPolicyGate::invalidate() noexcept {
    invalidated_ = pending();
    ready_ = false;
}

void SingletonTaskPolicyGate::revalidate() noexcept {
    if (pending()) {
        invalidated_ = false;
    }
}

SingletonTaskPolicyGate::CompletionDisposition
SingletonTaskPolicyGate::complete(std::uint64_t token, bool success,
                                  bool settingsOwnerAvailable) noexcept {
    if (!token_ || *token_ != token) {
        return CompletionDisposition::stale;
    }
    token_.reset();
    ready_ = false;
    if (invalidated_ && !settingsOwnerAvailable) {
        invalidated_ = false;
        return CompletionDisposition::discard;
    }
    invalidated_ = false;
    if (!success) {
        return CompletionDisposition::fatal;
    }
    ready_ = true;
    return CompletionDisposition::ready;
}

} // namespace prismdrake::shell::runtime
