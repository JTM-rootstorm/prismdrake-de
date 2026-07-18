#pragma once

#include "Result.hpp"
#include "SettingsSnapshot.hpp"

#include <QColor>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

namespace prismdrake::shell::theme {

/// Immutable panel projection derived from one complete settings/theme generation.
class PanelThemeTokens final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor surfaceColor READ surfaceColor CONSTANT)
    Q_PROPERTY(bool blurRequested READ blurRequested CONSTANT)
    Q_PROPERTY(bool fallbackActive READ fallbackActive CONSTANT)
    Q_PROPERTY(QColor textPrimaryColor READ textPrimaryColor CONSTANT)
    Q_PROPERTY(QColor textMutedColor READ textMutedColor CONSTANT)
    Q_PROPERTY(QColor focusColor READ focusColor CONSTANT)
    Q_PROPERTY(QColor selectionColor READ selectionColor CONSTANT)
    Q_PROPERTY(QColor activeBorderColor READ activeBorderColor CONSTANT)
    Q_PROPERTY(QColor inactiveBorderColor READ inactiveBorderColor CONSTANT)
    Q_PROPERTY(QColor dangerColor READ dangerColor CONSTANT)
    Q_PROPERTY(QColor warningColor READ warningColor CONSTANT)
    Q_PROPERTY(QColor successColor READ successColor CONSTANT)
    Q_PROPERTY(double borderWidth READ borderWidth CONSTANT)
    Q_PROPERTY(double focusWidth READ focusWidth CONSTANT)
    Q_PROPERTY(double panelHeight READ panelHeight CONSTANT)
    Q_PROPERTY(double iconSize READ iconSize CONSTANT)
    Q_PROPERTY(double minimumTargetSize READ minimumTargetSize CONSTANT)
    Q_PROPERTY(QString bodyFontFamily READ bodyFontFamily CONSTANT)
    Q_PROPERTY(double bodyFontPixels READ bodyFontPixels CONSTANT)
    Q_PROPERTY(double titleFontPixels READ titleFontPixels CONSTANT)
    Q_PROPERTY(int fastMotionMs READ fastMotionMs CONSTANT)
    Q_PROPERTY(int normalMotionMs READ normalMotionMs CONSTANT)
    Q_PROPERTY(double taskRadius READ taskRadius CONSTANT)
    Q_PROPERTY(double taskPadding READ taskPadding CONSTANT)
    Q_PROPERTY(double taskBorderWidth READ taskBorderWidth CONSTANT)
    Q_PROPERTY(double launcherRadius READ launcherRadius CONSTANT)
    Q_PROPERTY(double launcherPadding READ launcherPadding CONSTANT)
    Q_PROPERTY(double launcherBorderWidth READ launcherBorderWidth CONSTANT)

  public:
    [[nodiscard]] const QColor &surfaceColor() const noexcept { return surface_color_; }
    [[nodiscard]] bool blurRequested() const noexcept { return blur_requested_; }
    [[nodiscard]] bool fallbackActive() const noexcept { return fallback_active_; }
    [[nodiscard]] const QColor &textPrimaryColor() const noexcept { return text_primary_color_; }
    [[nodiscard]] const QColor &textMutedColor() const noexcept { return text_muted_color_; }
    [[nodiscard]] const QColor &focusColor() const noexcept { return focus_color_; }
    [[nodiscard]] const QColor &selectionColor() const noexcept { return selection_color_; }
    [[nodiscard]] const QColor &activeBorderColor() const noexcept { return active_border_color_; }
    [[nodiscard]] const QColor &inactiveBorderColor() const noexcept {
        return inactive_border_color_;
    }
    [[nodiscard]] const QColor &dangerColor() const noexcept { return danger_color_; }
    [[nodiscard]] const QColor &warningColor() const noexcept { return warning_color_; }
    [[nodiscard]] const QColor &successColor() const noexcept { return success_color_; }
    [[nodiscard]] double borderWidth() const noexcept { return border_width_; }
    [[nodiscard]] double focusWidth() const noexcept { return focus_width_; }
    [[nodiscard]] double panelHeight() const noexcept { return panel_height_; }
    [[nodiscard]] double iconSize() const noexcept { return icon_size_; }
    [[nodiscard]] double minimumTargetSize() const noexcept { return minimum_target_size_; }
    [[nodiscard]] const QString &bodyFontFamily() const noexcept { return body_font_family_; }
    [[nodiscard]] double bodyFontPixels() const noexcept { return body_font_pixels_; }
    [[nodiscard]] double titleFontPixels() const noexcept { return title_font_pixels_; }
    [[nodiscard]] int fastMotionMs() const noexcept { return fast_motion_ms_; }
    [[nodiscard]] int normalMotionMs() const noexcept { return normal_motion_ms_; }
    [[nodiscard]] double taskRadius() const noexcept { return task_radius_; }
    [[nodiscard]] double taskPadding() const noexcept { return task_padding_; }
    [[nodiscard]] double taskBorderWidth() const noexcept { return task_border_width_; }
    [[nodiscard]] double launcherRadius() const noexcept { return launcher_radius_; }
    [[nodiscard]] double launcherPadding() const noexcept { return launcher_padding_; }
    [[nodiscard]] double launcherBorderWidth() const noexcept { return launcher_border_width_; }

  private:
    explicit PanelThemeTokens(const prismdrake::settings::SettingsSnapshot &snapshot,
                              QObject *parent);

    QColor surface_color_;
    bool blur_requested_;
    bool fallback_active_;
    QColor text_primary_color_;
    QColor text_muted_color_;
    QColor focus_color_;
    QColor selection_color_;
    QColor active_border_color_;
    QColor inactive_border_color_;
    QColor danger_color_;
    QColor warning_color_;
    QColor success_color_;
    double border_width_;
    double focus_width_;
    double panel_height_;
    double icon_size_;
    double minimum_target_size_;
    QString body_font_family_;
    double body_font_pixels_;
    double title_font_pixels_;
    int fast_motion_ms_;
    int normal_motion_ms_;
    double task_radius_;
    double task_padding_;
    double task_border_width_;
    double launcher_radius_;
    double launcher_padding_;
    double launcher_border_width_;

    friend class ShellThemeGeneration;
};

/// Immutable launcher projection derived from the same complete generation as panel tokens.
class LauncherThemeTokens final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor surfaceColor READ surfaceColor CONSTANT)
    Q_PROPERTY(bool blurRequested READ blurRequested CONSTANT)
    Q_PROPERTY(bool fallbackActive READ fallbackActive CONSTANT)
    Q_PROPERTY(QColor borderColor READ borderColor CONSTANT)
    Q_PROPERTY(QColor textPrimaryColor READ textPrimaryColor CONSTANT)
    Q_PROPERTY(QColor textMutedColor READ textMutedColor CONSTANT)
    Q_PROPERTY(QColor focusColor READ focusColor CONSTANT)
    Q_PROPERTY(QColor selectionColor READ selectionColor CONSTANT)
    Q_PROPERTY(QColor dangerColor READ dangerColor CONSTANT)
    Q_PROPERTY(QColor controlColor READ controlColor CONSTANT)
    Q_PROPERTY(double tileRadius READ tileRadius CONSTANT)
    Q_PROPERTY(double tilePadding READ tilePadding CONSTANT)
    Q_PROPERTY(double tileBorderWidth READ tileBorderWidth CONSTANT)
    Q_PROPERTY(double borderWidth READ borderWidth CONSTANT)
    Q_PROPERTY(double focusWidth READ focusWidth CONSTANT)
    Q_PROPERTY(double minimumTargetSize READ minimumTargetSize CONSTANT)
    Q_PROPERTY(QString bodyFontFamily READ bodyFontFamily CONSTANT)
    Q_PROPERTY(double bodyFontPixels READ bodyFontPixels CONSTANT)
    Q_PROPERTY(double titleFontPixels READ titleFontPixels CONSTANT)
    Q_PROPERTY(int fastMotionMs READ fastMotionMs CONSTANT)
    Q_PROPERTY(int normalMotionMs READ normalMotionMs CONSTANT)
    Q_PROPERTY(bool reducedMotion READ reducedMotion CONSTANT)
    Q_PROPERTY(bool highContrast READ highContrast CONSTANT)
    Q_PROPERTY(bool transparencyDisabled READ transparencyDisabled CONSTANT)

  public:
    [[nodiscard]] const QColor &surfaceColor() const noexcept { return surface_color_; }
    [[nodiscard]] bool blurRequested() const noexcept { return blur_requested_; }
    [[nodiscard]] bool fallbackActive() const noexcept { return fallback_active_; }
    [[nodiscard]] const QColor &borderColor() const noexcept { return border_color_; }
    [[nodiscard]] const QColor &textPrimaryColor() const noexcept { return text_primary_color_; }
    [[nodiscard]] const QColor &textMutedColor() const noexcept { return text_muted_color_; }
    [[nodiscard]] const QColor &focusColor() const noexcept { return focus_color_; }
    [[nodiscard]] const QColor &selectionColor() const noexcept { return selection_color_; }
    [[nodiscard]] const QColor &dangerColor() const noexcept { return danger_color_; }
    [[nodiscard]] const QColor &controlColor() const noexcept { return control_color_; }
    [[nodiscard]] double tileRadius() const noexcept { return tile_radius_; }
    [[nodiscard]] double tilePadding() const noexcept { return tile_padding_; }
    [[nodiscard]] double tileBorderWidth() const noexcept { return tile_border_width_; }
    [[nodiscard]] double borderWidth() const noexcept { return border_width_; }
    [[nodiscard]] double focusWidth() const noexcept { return focus_width_; }
    [[nodiscard]] double minimumTargetSize() const noexcept { return minimum_target_size_; }
    [[nodiscard]] const QString &bodyFontFamily() const noexcept { return body_font_family_; }
    [[nodiscard]] double bodyFontPixels() const noexcept { return body_font_pixels_; }
    [[nodiscard]] double titleFontPixels() const noexcept { return title_font_pixels_; }
    [[nodiscard]] int fastMotionMs() const noexcept { return fast_motion_ms_; }
    [[nodiscard]] int normalMotionMs() const noexcept { return normal_motion_ms_; }
    [[nodiscard]] bool reducedMotion() const noexcept { return reduced_motion_; }
    [[nodiscard]] bool highContrast() const noexcept { return high_contrast_; }
    [[nodiscard]] bool transparencyDisabled() const noexcept { return transparency_disabled_; }

  private:
    explicit LauncherThemeTokens(const prismdrake::settings::SettingsSnapshot &snapshot,
                                 QObject *parent);

    QColor surface_color_;
    bool blur_requested_;
    bool fallback_active_;
    QColor border_color_;
    QColor text_primary_color_;
    QColor text_muted_color_;
    QColor focus_color_;
    QColor selection_color_;
    QColor danger_color_;
    QColor control_color_;
    double tile_radius_;
    double tile_padding_;
    double tile_border_width_;
    double border_width_;
    double focus_width_;
    double minimum_target_size_;
    QString body_font_family_;
    double body_font_pixels_;
    double title_font_pixels_;
    int fast_motion_ms_;
    int normal_motion_ms_;
    bool reduced_motion_;
    bool high_contrast_;
    bool transparency_disabled_;

    friend class ShellThemeGeneration;
};

/// Immutable notification projection derived from the same generation as the panel tokens.
class NotificationThemeTokens final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor surfaceColor READ surfaceColor CONSTANT)
    Q_PROPERTY(bool blurRequested READ blurRequested CONSTANT)
    Q_PROPERTY(bool fallbackActive READ fallbackActive CONSTANT)
    Q_PROPERTY(QColor borderColor READ borderColor CONSTANT)
    Q_PROPERTY(QColor textPrimaryColor READ textPrimaryColor CONSTANT)
    Q_PROPERTY(QColor textMutedColor READ textMutedColor CONSTANT)
    Q_PROPERTY(QColor focusColor READ focusColor CONSTANT)
    Q_PROPERTY(QColor criticalColor READ criticalColor CONSTANT)
    Q_PROPERTY(QColor controlColor READ controlColor CONSTANT)
    Q_PROPERTY(QColor pressedControlColor READ pressedControlColor CONSTANT)
    Q_PROPERTY(double cardRadius READ cardRadius CONSTANT)
    Q_PROPERTY(double cardPadding READ cardPadding CONSTANT)
    Q_PROPERTY(double borderWidth READ borderWidth CONSTANT)
    Q_PROPERTY(double focusWidth READ focusWidth CONSTANT)
    Q_PROPERTY(double minimumTargetSize READ minimumTargetSize CONSTANT)
    Q_PROPERTY(QString bodyFontFamily READ bodyFontFamily CONSTANT)
    Q_PROPERTY(double bodyFontPixels READ bodyFontPixels CONSTANT)
    Q_PROPERTY(double titleFontPixels READ titleFontPixels CONSTANT)
    Q_PROPERTY(int fastMotionMs READ fastMotionMs CONSTANT)
    Q_PROPERTY(int normalMotionMs READ normalMotionMs CONSTANT)
    Q_PROPERTY(bool reducedMotion READ reducedMotion CONSTANT)
    Q_PROPERTY(bool highContrast READ highContrast CONSTANT)
    Q_PROPERTY(bool transparencyDisabled READ transparencyDisabled CONSTANT)

  public:
    [[nodiscard]] const QColor &surfaceColor() const noexcept { return surface_color_; }
    [[nodiscard]] bool blurRequested() const noexcept { return blur_requested_; }
    [[nodiscard]] bool fallbackActive() const noexcept { return fallback_active_; }
    [[nodiscard]] const QColor &borderColor() const noexcept { return border_color_; }
    [[nodiscard]] const QColor &textPrimaryColor() const noexcept { return text_primary_color_; }
    [[nodiscard]] const QColor &textMutedColor() const noexcept { return text_muted_color_; }
    [[nodiscard]] const QColor &focusColor() const noexcept { return focus_color_; }
    [[nodiscard]] const QColor &criticalColor() const noexcept { return critical_color_; }
    [[nodiscard]] const QColor &controlColor() const noexcept { return control_color_; }
    [[nodiscard]] const QColor &pressedControlColor() const noexcept {
        return pressed_control_color_;
    }
    [[nodiscard]] double cardRadius() const noexcept { return card_radius_; }
    [[nodiscard]] double cardPadding() const noexcept { return card_padding_; }
    [[nodiscard]] double borderWidth() const noexcept { return border_width_; }
    [[nodiscard]] double focusWidth() const noexcept { return focus_width_; }
    [[nodiscard]] double minimumTargetSize() const noexcept { return minimum_target_size_; }
    [[nodiscard]] const QString &bodyFontFamily() const noexcept { return body_font_family_; }
    [[nodiscard]] double bodyFontPixels() const noexcept { return body_font_pixels_; }
    [[nodiscard]] double titleFontPixels() const noexcept { return title_font_pixels_; }
    [[nodiscard]] int fastMotionMs() const noexcept { return fast_motion_ms_; }
    [[nodiscard]] int normalMotionMs() const noexcept { return normal_motion_ms_; }
    [[nodiscard]] bool reducedMotion() const noexcept { return reduced_motion_; }
    [[nodiscard]] bool highContrast() const noexcept { return high_contrast_; }
    [[nodiscard]] bool transparencyDisabled() const noexcept { return transparency_disabled_; }

  private:
    explicit NotificationThemeTokens(const prismdrake::settings::SettingsSnapshot &snapshot,
                                     QObject *parent);

    QColor surface_color_;
    bool blur_requested_;
    bool fallback_active_;
    QColor border_color_;
    QColor text_primary_color_;
    QColor text_muted_color_;
    QColor focus_color_;
    QColor critical_color_;
    QColor control_color_;
    QColor pressed_control_color_;
    double card_radius_;
    double card_padding_;
    double border_width_;
    double focus_width_;
    double minimum_target_size_;
    QString body_font_family_;
    double body_font_pixels_;
    double title_font_pixels_;
    int fast_motion_ms_;
    int normal_motion_ms_;
    bool reduced_motion_;
    bool high_contrast_;
    bool transparency_disabled_;

    friend class ShellThemeGeneration;
};

/// One immutable QObject graph backed by one retained settings snapshot.
class ShellThemeGeneration final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString generationId READ generationId CONSTANT)
    Q_PROPERTY(QString profileId READ profileId CONSTANT)
    Q_PROPERTY(QString profileDisplayName READ profileDisplayName CONSTANT)
    Q_PROPERTY(bool highContrast READ highContrast CONSTANT)
    Q_PROPERTY(bool reducedMotion READ reducedMotion CONSTANT)
    Q_PROPERTY(bool transparencyDisabled READ transparencyDisabled CONSTANT)
    Q_PROPERTY(double textScale READ textScale CONSTANT)
    Q_PROPERTY(double animationScale READ animationScale CONSTANT)
    Q_PROPERTY(PanelThemeTokens *panel READ panel CONSTANT)
    Q_PROPERTY(LauncherThemeTokens *launcher READ launcher CONSTANT)
    Q_PROPERTY(NotificationThemeTokens *notification READ notification CONSTANT)

  public:
    [[nodiscard]] prismdrake::foundation::Generation generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] const QString &generationId() const noexcept { return generation_id_; }
    [[nodiscard]] const QString &profileId() const noexcept { return profile_id_; }
    [[nodiscard]] const QString &profileDisplayName() const noexcept {
        return profile_display_name_;
    }
    [[nodiscard]] bool highContrast() const noexcept { return high_contrast_; }
    [[nodiscard]] bool reducedMotion() const noexcept { return reduced_motion_; }
    [[nodiscard]] bool transparencyDisabled() const noexcept { return transparency_disabled_; }
    [[nodiscard]] double textScale() const noexcept { return text_scale_; }
    [[nodiscard]] double animationScale() const noexcept { return animation_scale_; }
    [[nodiscard]] PanelThemeTokens *panel() const noexcept { return panel_; }
    [[nodiscard]] LauncherThemeTokens *launcher() const noexcept { return launcher_; }
    [[nodiscard]] NotificationThemeTokens *notification() const noexcept { return notification_; }

    [[nodiscard]] const std::shared_ptr<const prismdrake::settings::SettingsSnapshot> &
    snapshot() const noexcept {
        return snapshot_;
    }

  private:
    explicit ShellThemeGeneration(
        std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);

    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot_;
    prismdrake::foundation::Generation generation_;
    QString generation_id_;
    QString profile_id_;
    QString profile_display_name_;
    bool high_contrast_;
    bool reduced_motion_;
    bool transparency_disabled_;
    double text_scale_;
    double animation_scale_;
    PanelThemeTokens *panel_;
    LauncherThemeTokens *launcher_;
    NotificationThemeTokens *notification_;

    friend class ShellThemeSnapshotAdapter;
};

/// Owner-thread Qt projection of complete immutable settings/theme snapshots.
class ShellThemeSnapshotAdapter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(ShellThemeGeneration *current READ current NOTIFY currentChanged)

  public:
    explicit ShellThemeSnapshotAdapter(QObject *parent = nullptr) : QObject(parent) {}

    /// Applies one complete generation. Rejection retains the prior QObject graph unchanged.
    [[nodiscard]] foundation::Result<void>
    applySnapshot(std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);

    [[nodiscard]] ShellThemeGeneration *current() const noexcept { return current_.get(); }
    [[nodiscard]] std::shared_ptr<const ShellThemeGeneration> currentGeneration() const noexcept {
        return current_;
    }
    [[nodiscard]] std::shared_ptr<const ShellThemeGeneration> previousGeneration() const noexcept {
        return previous_;
    }

  signals:
    void currentChanged();

  private:
    std::shared_ptr<ShellThemeGeneration> current_;
    std::shared_ptr<ShellThemeGeneration> previous_;
    bool applying_{false};
};

} // namespace prismdrake::shell::theme
