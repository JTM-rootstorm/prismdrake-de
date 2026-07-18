#include "ShellThemeSnapshotAdapter.hpp"

#include <QByteArray>
#include <QStringDecoder>
#include <QThread>

#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace prismdrake::shell::theme {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

class ApplyingSnapshotGuard final {
  public:
    explicit ApplyingSnapshotGuard(bool &applying) noexcept : applying_(&applying) {
        applying = true;
    }
    ~ApplyingSnapshotGuard() {
        if (applying_ != nullptr) {
            *applying_ = false;
        }
    }

    ApplyingSnapshotGuard(const ApplyingSnapshotGuard &) = delete;
    ApplyingSnapshotGuard &operator=(const ApplyingSnapshotGuard &) = delete;

  private:
    bool *applying_;
};

[[nodiscard]] Error invalidSnapshot(std::string_view field) {
    return {ErrorCode::validation_error,
            "shell theme snapshot contains an invalid " + std::string(field),
            "retain the prior presentation and publish a complete validated settings generation"};
}

[[nodiscard]] bool finiteNonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool finitePositive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool validUtf8(std::string_view value) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    (void)decoder.decode(
        QByteArray::fromRawData(value.data(), static_cast<qsizetype>(value.size())));
    return !decoder.hasError();
}

[[nodiscard]] QString utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QColor color(prismdrake::theme::Color value) {
    const auto packed = value.rgba;
    return QColor::fromRgb(
        static_cast<int>((packed >> 24U) & 0xffU), static_cast<int>((packed >> 16U) & 0xffU),
        static_cast<int>((packed >> 8U) & 0xffU), static_cast<int>(packed & 0xffU));
}

[[nodiscard]] QColor materialColor(const prismdrake::theme::ResolvedMaterial &material) {
    const auto packed = material.color.rgba;
    const double alpha = (static_cast<double>(packed & 0xffU) / 255.0) * material.opacity;
    return QColor::fromRgbF(static_cast<double>((packed >> 24U) & 0xffU) / 255.0,
                            static_cast<double>((packed >> 16U) & 0xffU) / 255.0,
                            static_cast<double>((packed >> 8U) & 0xffU) / 255.0, alpha);
}

[[nodiscard]] Result<void> validateMaterial(const prismdrake::theme::ResolvedMaterial &material,
                                            std::string_view name) {
    if (!std::isfinite(material.opacity) || material.opacity < 0.0 || material.opacity > 1.0 ||
        !finiteNonnegative(material.blurRadiusPx) || !finiteNonnegative(material.saturation) ||
        (material.usedFallback && material.blurRequested) ||
        (!material.blurRequested && material.blurRadiusPx != 0.0)) {
        return Result<void>::failure(invalidSnapshot(name));
    }
    return Result<void>::success();
}

[[nodiscard]] bool validComponent(const prismdrake::theme::ComponentStyle &component) noexcept {
    return finiteNonnegative(component.radiusPx) && finiteNonnegative(component.paddingPx) &&
           finiteNonnegative(component.borderWidthPx);
}

[[nodiscard]] Result<double> requiredPositive(const prismdrake::theme::NumberMap &values,
                                              std::string_view key,
                                              std::string_view diagnosticName) {
    const auto value = values.find(key);
    if (value == values.end() || !finitePositive(value->second)) {
        return Result<double>::failure(invalidSnapshot(diagnosticName));
    }
    return Result<double>::success(value->second);
}

[[nodiscard]] Result<void>
validateSnapshot(const prismdrake::settings::SettingsSnapshot &snapshot) {
    if (snapshot.snapshotSchemaVersion != prismdrake::settings::runtimeSnapshotSchemaVersion ||
        snapshot.generation.value() == foundation::Generation::unpublishedValue) {
        return Result<void>::failure(invalidSnapshot("schema or generation"));
    }

    const auto &configuration = snapshot.candidate.configuration;
    const auto &resolved = snapshot.candidate.theme;
    std::string_view expectedDisplayName;
    prismdrake::theme::Profile expectedThemeProfile;
    switch (configuration.profile) {
    case config::Profile::lustre:
        expectedThemeProfile = prismdrake::theme::Profile::lustre;
        expectedDisplayName = "Prismdrake Lustre";
        break;
    case config::Profile::forge:
        expectedThemeProfile = prismdrake::theme::Profile::forge;
        expectedDisplayName = "Prismdrake Forge";
        break;
    default:
        return Result<void>::failure(invalidSnapshot("profile identity"));
    }
    if (resolved.profile != expectedThemeProfile ||
        resolved.profileDisplayName != expectedDisplayName ||
        !validUtf8(resolved.profileDisplayName) || resolved.schemaVersion != 1U) {
        return Result<void>::failure(invalidSnapshot("profile identity"));
    }
    if (configuration.panel.sizePx == 0U) {
        return Result<void>::failure(invalidSnapshot("configured panel size"));
    }

    const auto &accessibility = resolved.accessibility;
    if (!finitePositive(accessibility.textScale) ||
        !finiteNonnegative(accessibility.animationScale) ||
        !finitePositive(accessibility.focusWidthPx) ||
        !finitePositive(accessibility.minimumTargetSizePx) ||
        !finitePositive(accessibility.minimumContrastRatio)) {
        return Result<void>::failure(invalidSnapshot("effective accessibility values"));
    }

    const auto &semantic = resolved.semantic;
    if (!validUtf8(semantic.typography.bodyFamily) || semantic.typography.bodyFamily.empty() ||
        !finitePositive(semantic.typography.bodySizePx) ||
        !finitePositive(semantic.typography.titleSizePx) ||
        !finitePositive(semantic.border.thicknessPx) ||
        !validComponent(resolved.component.taskButton) ||
        !validComponent(resolved.component.launcherTile) ||
        !validComponent(resolved.component.notificationCard) ||
        !validComponent(resolved.component.menuItem)) {
        return Result<void>::failure(invalidSnapshot("presentation token value"));
    }

    auto panelHeight = requiredPositive(semantic.panel, "height_px", "panel height token");
    if (!panelHeight) {
        return Result<void>::failure(panelHeight.error());
    }
    auto iconSize = requiredPositive(semantic.panel, "icon_size_px", "panel icon-size token");
    if (!iconSize) {
        return Result<void>::failure(iconSize.error());
    }
    auto panelMaterial = validateMaterial(resolved.materials.panel, "resolved panel material");
    if (!panelMaterial) {
        return panelMaterial;
    }
    auto launcherMaterial =
        validateMaterial(resolved.materials.launcher, "resolved launcher material");
    if (!launcherMaterial) {
        return launcherMaterial;
    }
    auto notificationMaterial =
        validateMaterial(resolved.materials.notification, "resolved notification material");
    if (!notificationMaterial) {
        return notificationMaterial;
    }
    return Result<void>::success();
}

[[nodiscard]] QString resolvedProfileId(prismdrake::theme::Profile profile) {
    return profile == prismdrake::theme::Profile::lustre ? QStringLiteral("lustre")
                                                         : QStringLiteral("forge");
}

} // namespace

PanelThemeTokens::PanelThemeTokens(const prismdrake::settings::SettingsSnapshot &snapshot,
                                   QObject *parent)
    : QObject(parent), surface_color_(materialColor(snapshot.candidate.theme.materials.panel)),
      blur_requested_(snapshot.candidate.theme.materials.panel.blurRequested),
      fallback_active_(snapshot.candidate.theme.materials.panel.usedFallback),
      text_primary_color_(color(snapshot.candidate.theme.semantic.colors.textPrimary)),
      text_muted_color_(color(snapshot.candidate.theme.semantic.colors.textMuted)),
      focus_color_(color(snapshot.candidate.theme.semantic.colors.focusRing)),
      selection_color_(color(snapshot.candidate.theme.semantic.colors.selection)),
      active_border_color_(color(snapshot.candidate.theme.semantic.colors.borderActive)),
      inactive_border_color_(color(snapshot.candidate.theme.semantic.colors.borderInactive)),
      danger_color_(color(snapshot.candidate.theme.semantic.colors.danger)),
      warning_color_(color(snapshot.candidate.theme.semantic.colors.warning)),
      success_color_(color(snapshot.candidate.theme.semantic.colors.success)),
      border_width_(snapshot.candidate.theme.semantic.border.thicknessPx),
      focus_width_(snapshot.candidate.theme.accessibility.focusWidthPx),
      panel_height_(snapshot.candidate.configuration.panel.sizePx),
      icon_size_(snapshot.candidate.theme.semantic.panel.at("icon_size_px")),
      minimum_target_size_(snapshot.candidate.theme.accessibility.minimumTargetSizePx),
      body_font_family_(utf8(snapshot.candidate.theme.semantic.typography.bodyFamily)),
      body_font_pixels_(snapshot.candidate.theme.semantic.typography.bodySizePx),
      title_font_pixels_(snapshot.candidate.theme.semantic.typography.titleSizePx),
      fast_motion_ms_(snapshot.candidate.theme.semantic.motion.fastMs),
      normal_motion_ms_(snapshot.candidate.theme.semantic.motion.normalMs),
      task_radius_(snapshot.candidate.theme.component.taskButton.radiusPx),
      task_padding_(snapshot.candidate.theme.component.taskButton.paddingPx),
      task_border_width_(snapshot.candidate.theme.component.taskButton.borderWidthPx),
      launcher_radius_(snapshot.candidate.theme.component.launcherTile.radiusPx),
      launcher_padding_(snapshot.candidate.theme.component.launcherTile.paddingPx),
      launcher_border_width_(snapshot.candidate.theme.component.launcherTile.borderWidthPx),
      menu_item_radius_(snapshot.candidate.theme.component.menuItem.radiusPx),
      menu_item_padding_(snapshot.candidate.theme.component.menuItem.paddingPx),
      menu_item_border_width_(snapshot.candidate.theme.component.menuItem.borderWidthPx) {}

LauncherThemeTokens::LauncherThemeTokens(const prismdrake::settings::SettingsSnapshot &snapshot,
                                         QObject *parent)
    : QObject(parent), surface_color_(materialColor(snapshot.candidate.theme.materials.launcher)),
      blur_requested_(snapshot.candidate.theme.materials.launcher.blurRequested),
      fallback_active_(snapshot.candidate.theme.materials.launcher.usedFallback),
      border_color_(color(snapshot.candidate.theme.semantic.colors.borderInactive)),
      text_primary_color_(color(snapshot.candidate.theme.semantic.colors.textPrimary)),
      text_muted_color_(color(snapshot.candidate.theme.semantic.colors.textMuted)),
      focus_color_(color(snapshot.candidate.theme.semantic.colors.focusRing)),
      selection_color_(color(snapshot.candidate.theme.semantic.colors.selection)),
      danger_color_(color(snapshot.candidate.theme.semantic.colors.danger)),
      control_color_(color(snapshot.candidate.theme.semantic.colors.elevatedSurface)),
      tile_radius_(snapshot.candidate.theme.component.launcherTile.radiusPx),
      tile_padding_(snapshot.candidate.theme.component.launcherTile.paddingPx),
      tile_border_width_(snapshot.candidate.theme.component.launcherTile.borderWidthPx),
      border_width_(snapshot.candidate.theme.semantic.border.thicknessPx),
      focus_width_(snapshot.candidate.theme.accessibility.focusWidthPx),
      minimum_target_size_(snapshot.candidate.theme.accessibility.minimumTargetSizePx),
      body_font_family_(utf8(snapshot.candidate.theme.semantic.typography.bodyFamily)),
      body_font_pixels_(snapshot.candidate.theme.semantic.typography.bodySizePx),
      title_font_pixels_(snapshot.candidate.theme.semantic.typography.titleSizePx),
      fast_motion_ms_(snapshot.candidate.theme.semantic.motion.fastMs),
      normal_motion_ms_(snapshot.candidate.theme.semantic.motion.normalMs),
      reduced_motion_(snapshot.candidate.theme.accessibility.reducedMotion),
      high_contrast_(snapshot.candidate.theme.accessibility.highContrast),
      transparency_disabled_(snapshot.candidate.theme.accessibility.transparencyDisabled) {}

NotificationThemeTokens::NotificationThemeTokens(
    const prismdrake::settings::SettingsSnapshot &snapshot, QObject *parent)
    : QObject(parent),
      surface_color_(materialColor(snapshot.candidate.theme.materials.notification)),
      blur_requested_(snapshot.candidate.theme.materials.notification.blurRequested),
      fallback_active_(snapshot.candidate.theme.materials.notification.usedFallback),
      border_color_(color(snapshot.candidate.theme.semantic.colors.borderInactive)),
      text_primary_color_(color(snapshot.candidate.theme.semantic.colors.textPrimary)),
      text_muted_color_(color(snapshot.candidate.theme.semantic.colors.textMuted)),
      focus_color_(color(snapshot.candidate.theme.semantic.colors.focusRing)),
      critical_color_(color(snapshot.candidate.theme.semantic.colors.danger)),
      control_color_(color(snapshot.candidate.theme.semantic.colors.elevatedSurface)),
      pressed_control_color_(color(snapshot.candidate.theme.semantic.colors.selection)),
      card_radius_(snapshot.candidate.theme.component.notificationCard.radiusPx),
      card_padding_(snapshot.candidate.theme.component.notificationCard.paddingPx),
      border_width_(snapshot.candidate.theme.component.notificationCard.borderWidthPx),
      focus_width_(snapshot.candidate.theme.accessibility.focusWidthPx),
      minimum_target_size_(snapshot.candidate.theme.accessibility.minimumTargetSizePx),
      body_font_family_(utf8(snapshot.candidate.theme.semantic.typography.bodyFamily)),
      body_font_pixels_(snapshot.candidate.theme.semantic.typography.bodySizePx),
      title_font_pixels_(snapshot.candidate.theme.semantic.typography.titleSizePx),
      fast_motion_ms_(snapshot.candidate.theme.semantic.motion.fastMs),
      normal_motion_ms_(snapshot.candidate.theme.semantic.motion.normalMs),
      reduced_motion_(snapshot.candidate.theme.accessibility.reducedMotion),
      high_contrast_(snapshot.candidate.theme.accessibility.highContrast),
      transparency_disabled_(snapshot.candidate.theme.accessibility.transparencyDisabled) {}

ShellThemeGeneration::ShellThemeGeneration(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot)
    : snapshot_(std::move(snapshot)), generation_(snapshot_->generation),
      generation_id_(QString::number(snapshot_->generation.value())),
      profile_id_(resolvedProfileId(snapshot_->candidate.theme.profile)),
      profile_display_name_(utf8(snapshot_->candidate.theme.profileDisplayName)),
      high_contrast_(snapshot_->candidate.theme.accessibility.highContrast),
      reduced_motion_(snapshot_->candidate.theme.accessibility.reducedMotion),
      transparency_disabled_(snapshot_->candidate.theme.accessibility.transparencyDisabled),
      text_scale_(snapshot_->candidate.theme.accessibility.textScale),
      animation_scale_(snapshot_->candidate.theme.accessibility.animationScale),
      panel_(new PanelThemeTokens(*snapshot_, this)),
      launcher_(new LauncherThemeTokens(*snapshot_, this)),
      notification_(new NotificationThemeTokens(*snapshot_, this)) {}

foundation::Result<void> ShellThemeSnapshotAdapter::applySnapshot(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "shell theme snapshot was applied from a non-owner thread",
             "queue the complete snapshot to the adapter's QObject thread"});
    }
    if (applying_) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "shell theme snapshot application is already in progress",
             "queue the newer complete snapshot until currentChanged has returned"});
    }
    if (!snapshot) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "shell theme snapshot is absent",
             "retain the prior presentation until a complete snapshot is available"});
    }
    auto valid = validateSnapshot(*snapshot);
    if (!valid) {
        return valid;
    }
    if (current_) {
        if (snapshot->generation < current_->snapshot()->generation) {
            return Result<void>::failure({ErrorCode::cancelled, "shell theme snapshot is stale",
                                          "retain the newer complete shell theme generation"});
        }
        if (snapshot->generation == current_->snapshot()->generation) {
            if (snapshot == current_->snapshot()) {
                return Result<void>::success();
            }
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "shell theme generation has conflicting snapshot identity",
                 "retain the prior presentation and publish changed content as a new generation"});
        }
    }

    ApplyingSnapshotGuard applyingGuard{applying_};
    std::shared_ptr<ShellThemeGeneration> replacement;
    try {
        replacement =
            std::shared_ptr<ShellThemeGeneration>(new ShellThemeGeneration(std::move(snapshot)));
    } catch (const std::bad_alloc &) {
        return Result<void>::failure(
            {ErrorCode::too_large, "shell theme generation could not be allocated",
             "retain the prior presentation and reduce memory pressure before retrying"});
    }
    previous_ = current_;
    current_ = std::move(replacement);
    emit currentChanged();
    return Result<void>::success();
}

} // namespace prismdrake::shell::theme
