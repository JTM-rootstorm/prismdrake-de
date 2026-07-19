#include "NotificationSurfaceIntents.hpp"

#include <algorithm>

namespace prismdrake::shell::notificationcenter {
namespace {

[[nodiscard]] foundation::Error invalidIntent() {
    return {foundation::ErrorCode::invalid_argument, "The notification surface intent is invalid.",
            "Use a bounded output identity and a current typed focus origin."};
}

[[nodiscard]] bool validOutputName(const QString &value, bool allowEmpty) {
    if ((!allowEmpty && value.isEmpty()) || value.size() > 128) {
        return false;
    }
    return std::ranges::none_of(value, [](QChar character) {
        const auto code = character.unicode();
        return code == 0U || code < 0x20U || (code >= 0x7FU && code <= 0x9FU);
    });
}

} // namespace

foundation::Result<void>
NotificationSurfaceIntentSource::requestCenter(QString outputName,
                                               NotificationFocusOrigin focusOrigin) {
    if (!validOutputName(outputName, true) || focusOrigin.value == 0U) {
        return foundation::Result<void>::failure(invalidIntent());
    }
    emit placementRequested(
        {NotificationSurfaceKind::center, std::move(outputName), focusOrigin, 0U});
    return foundation::Result<void>::success();
}

foundation::Result<void>
NotificationSurfaceIntentSource::requestTransientStack(QString primaryOutputName,
                                                       std::uint64_t topologyGeneration) {
    if (!validOutputName(primaryOutputName, false) || topologyGeneration == 0U) {
        return foundation::Result<void>::failure(invalidIntent());
    }
    emit placementRequested({NotificationSurfaceKind::transientStack, std::move(primaryOutputName),
                             std::nullopt, topologyGeneration});
    return foundation::Result<void>::success();
}

foundation::Result<void>
NotificationSurfaceIntentSource::requestFocusReturn(NotificationFocusOrigin focusOrigin) {
    if (focusOrigin.value == 0U) {
        return foundation::Result<void>::failure(invalidIntent());
    }
    emit focusReturnRequested(focusOrigin);
    return foundation::Result<void>::success();
}

} // namespace prismdrake::shell::notificationcenter
