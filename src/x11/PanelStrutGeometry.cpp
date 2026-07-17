#include "PanelStrutGeometry.hpp"

#include <cstdint>
#include <limits>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<BottomPanelStrut> invalidGeometry() {
    return Result<BottomPanelStrut>::failure(
        {ErrorCode::validation_error, "The bottom-panel geometry is invalid.",
         "Use nonzero contained root and output geometry with a bounded panel height."});
}

[[nodiscard]] bool fitsCardinal(std::uint64_t value) noexcept {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

} // namespace

Result<BottomPanelStrut> calculateBottomPanelStrut(RootGeometry root, OutputGeometry output,
                                                   std::uint32_t panelHeight) {
    if (panelHeight == 0U || panelHeight > output.heightPx()) {
        return invalidGeometry();
    }

    const auto outputX = static_cast<std::uint64_t>(output.xPx());
    const auto outputY = static_cast<std::uint64_t>(output.yPx());
    const auto outputRightExclusive = outputX + output.widthPx();
    const auto outputBottomExclusive = outputY + output.heightPx();
    if (!fitsCardinal(outputRightExclusive) || !fitsCardinal(outputBottomExclusive) ||
        outputRightExclusive > root.widthPx() || outputBottomExclusive > root.heightPx()) {
        return invalidGeometry();
    }

    const std::uint64_t panelY = outputBottomExclusive - panelHeight;
    const std::uint64_t bottom = static_cast<std::uint64_t>(root.heightPx()) - panelY;
    const std::uint64_t bottomStartX = outputX;
    const std::uint64_t bottomEndX = outputRightExclusive - 1U;
    if (!fitsCardinal(panelY) || !fitsCardinal(bottom) || !fitsCardinal(bottomStartX) ||
        !fitsCardinal(bottomEndX)) {
        return invalidGeometry();
    }

    const auto panel =
        BottomPanelGeometry{static_cast<std::uint32_t>(outputX), static_cast<std::uint32_t>(panelY),
                            output.widthPx(), panelHeight};
    const auto bottomCardinal = static_cast<std::uint32_t>(bottom);
    const auto startCardinal = static_cast<std::uint32_t>(bottomStartX);
    const auto endCardinal = static_cast<std::uint32_t>(bottomEndX);
    return Result<BottomPanelStrut>::success(BottomPanelStrut{
        panel,
        {0U, 0U, 0U, bottomCardinal},
        {0U, 0U, 0U, bottomCardinal, 0U, 0U, 0U, 0U, 0U, 0U, startCardinal, endCardinal}});
}

} // namespace prismdrake::x11
