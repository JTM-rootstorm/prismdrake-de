#include "EwmhTaskList.hpp"

#include <algorithm>
#include <utility>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename T> [[nodiscard]] Result<T> malformedObservation() {
    return Result<T>::failure(
        {ErrorCode::validation_error, "The authoritative EWMH task list is malformed.",
         "Discard the complete observation and retain the previous validated task list."});
}

template <typename T> [[nodiscard]] Result<T> oversizedObservation() {
    return Result<T>::failure(
        {ErrorCode::too_large, "The authoritative EWMH task list exceeds its window bound.",
         "Discard the complete observation and retain the previous validated task list."});
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

} // namespace

EwmhTaskListSnapshot::EwmhTaskListSnapshot(std::vector<WindowId> clientList,
                                           std::vector<WindowId> stackingOrder,
                                           std::optional<WindowId> activeWindow,
                                           EwmhStackingSource stackingSource) noexcept
    : client_list_(std::move(clientList)), stacking_order_(std::move(stackingOrder)),
      active_window_(activeWindow), stacking_source_(stackingSource) {}

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
    if (observation.clientListStacking) {
        auto stacking = validateList(observation.clientListStacking.value());
        if (!stacking) {
            return Result<EwmhTaskListSnapshot>::failure(stacking.error());
        }
        if (stacking.value().size() != clients.value().size() ||
            !std::all_of(
                stacking.value().begin(), stacking.value().end(),
                [&clients](WindowId window) { return contains(clients.value(), window); })) {
            return malformedObservation<EwmhTaskListSnapshot>();
        }
        stackingOrder = std::move(stacking).value();
        stackingSource = EwmhStackingSource::clientListStacking;
    } else {
        stackingOrder = clients.value();
    }

    std::optional<WindowId> activeWindow;
    if (observation.activeWindow) {
        auto active = WindowId::fromProtocol(observation.activeWindow.value());
        if (!active || !contains(clients.value(), active.value())) {
            return malformedObservation<EwmhTaskListSnapshot>();
        }
        activeWindow = active.value();
    }

    return Result<EwmhTaskListSnapshot>::success(EwmhTaskListSnapshot{
        std::move(clients).value(), std::move(stackingOrder), activeWindow, stackingSource});
}

} // namespace prismdrake::x11
