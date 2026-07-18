#include "EwmhTaskList.hpp"

#include <algorithm>
#include <utility>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename T> [[nodiscard]] Result<T> malformedObservation() {
    return Result<T>::failure(
        {ErrorCode::validation_error, "The mandatory EWMH client list is malformed.",
         "Discard the complete observation and retain the previous validated task list."});
}

template <typename T> [[nodiscard]] Result<T> oversizedObservation() {
    return Result<T>::failure(
        {ErrorCode::too_large, "The mandatory EWMH client list exceeds its window bound.",
         "Discard the complete observation and retain the previous validated task list."});
}

template <typename T> [[nodiscard]] Result<T> malformedActiveObservation() {
    return Result<T>::failure(
        {ErrorCode::validation_error, "The optional EWMH active-window property is malformed.",
         "Ignore the untrusted property and retain the previous validated task list."});
}

template <typename T> [[nodiscard]] Result<T> malformedStackingObservation() {
    return Result<T>::failure(
        {ErrorCode::validation_error, "The optional EWMH stacking list is malformed.",
         "Ignore the untrusted property and retain the previous validated task list."});
}

template <typename T> [[nodiscard]] Result<T> oversizedStackingObservation() {
    return Result<T>::failure(
        {ErrorCode::too_large, "The optional EWMH stacking list exceeds its window bound.",
         "Ignore the oversized property and retain the previous validated task list."});
}

[[nodiscard]] bool contains(std::span<const WindowId> windows, WindowId window) noexcept {
    return std::find(windows.begin(), windows.end(), window) != windows.end();
}

[[nodiscard]] Result<std::vector<WindowId>>
validateList(const std::vector<WindowId::Value> &rawWindows) {
    if (rawWindows.size() > maximumEwmhTaskWindows) {
        return oversizedObservation<std::vector<WindowId>>();
    }

    std::vector<WindowId> windows;
    windows.reserve(rawWindows.size());
    for (const auto rawWindow : rawWindows) {
        const auto window = WindowId::fromProtocol(rawWindow);
        if (!window || contains(windows, window.value())) {
            return malformedObservation<std::vector<WindowId>>();
        }
        windows.push_back(window.value());
    }
    return Result<std::vector<WindowId>>::success(std::move(windows));
}

[[nodiscard]] Result<std::vector<WindowId>>
validateStackingList(const std::vector<WindowId::Value> &rawWindows) {
    if (rawWindows.size() > maximumEwmhTaskWindows) {
        return oversizedStackingObservation<std::vector<WindowId>>();
    }
    auto stacking = validateList(rawWindows);
    if (!stacking) {
        return malformedStackingObservation<std::vector<WindowId>>();
    }
    return stacking;
}

} // namespace

EwmhTaskListSnapshot::EwmhTaskListSnapshot(std::vector<WindowId> clientList,
                                           std::vector<WindowId> stackingOrder,
                                           std::optional<WindowId> activeWindow,
                                           EwmhStackingSource stackingSource,
                                           bool stackingSetDisagreed,
                                           bool staleActiveWindowCleared) noexcept
    : client_list_(std::move(clientList)), stacking_order_(std::move(stackingOrder)),
      active_window_(activeWindow), stacking_source_(stackingSource),
      stacking_set_disagreed_(stackingSetDisagreed),
      stale_active_window_cleared_(staleActiveWindowCleared) {}

bool EwmhTaskListSnapshot::contains(WindowId window) const noexcept {
    return x11::contains(client_list_, window);
}

Result<EwmhTaskListSnapshot> buildEwmhTaskListSnapshot(const EwmhTaskListObservation &observation) {
    if (!observation.clientList) {
        return malformedObservation<EwmhTaskListSnapshot>();
    }

    auto clients = validateList(observation.clientList.value());
    if (!clients) {
        return Result<EwmhTaskListSnapshot>::failure(clients.error());
    }

    std::vector<WindowId> stackingOrder;
    EwmhStackingSource stackingSource = EwmhStackingSource::clientListFallback;
    bool stackingSetDisagreed = false;
    if (observation.clientListStacking) {
        auto stacking = validateStackingList(observation.clientListStacking.value());
        if (!stacking) {
            return Result<EwmhTaskListSnapshot>::failure(stacking.error());
        }
        if (stacking.value().size() == clients.value().size() &&
            std::all_of(
                stacking.value().begin(), stacking.value().end(),
                [&clients](WindowId window) { return contains(clients.value(), window); })) {
            stackingOrder = std::move(stacking).value();
            stackingSource = EwmhStackingSource::clientListStacking;
        } else {
            stackingOrder = clients.value();
            stackingSetDisagreed = true;
        }
    } else {
        stackingOrder = clients.value();
    }

    std::optional<WindowId> activeWindow;
    bool staleActiveWindowCleared = false;
    if (observation.activeWindow) {
        auto active = WindowId::fromProtocol(observation.activeWindow.value());
        if (!active) {
            return malformedActiveObservation<EwmhTaskListSnapshot>();
        }
        if (contains(clients.value(), active.value())) {
            activeWindow = active.value();
        } else {
            staleActiveWindowCleared = true;
        }
    }

    return Result<EwmhTaskListSnapshot>::success(
        EwmhTaskListSnapshot{std::move(clients).value(), std::move(stackingOrder), activeWindow,
                             stackingSource, stackingSetDisagreed, staleActiveWindowCleared});
}

bool sameEwmhTaskMembership(const EwmhTaskListSnapshot &left,
                            const EwmhTaskListSnapshot &right) noexcept {
    return std::ranges::equal(left.clientList(), right.clientList());
}

} // namespace prismdrake::x11
