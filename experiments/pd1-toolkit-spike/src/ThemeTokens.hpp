#pragma once

#include <QColor>
#include <QString>

#include <optional>

namespace prismdrake::experiments {

struct ThemeTokens {
    QString profileId;
    QString profileDisplayName;
    QColor panelTint;
    qreal panelOpacity = 1.0;
    QColor panelFallback;
    QColor elevatedSurface;
    QColor borderActive;
    QColor borderInactive;
    QColor focusRing;
    QColor textPrimary;
    QColor textMuted;
    int bodyFontPixels = 14;
    int titleFontPixels = 16;
    int minimumTargetPixels = 44;
    int borderWidthPixels = 1;
    int focusWidthPixels = 2;
    int taskRadiusPixels = 6;
    int motionDurationMs = 0;
};

class ThemeTokenRepository final {
public:
    explicit ThemeTokenRepository(QString themeDirectory);

    [[nodiscard]] std::optional<ThemeTokens> loadProfile(
        const QString &profileId,
        QString *errorMessage = nullptr) const;

private:
    QString themeDirectory_;
};

} // namespace prismdrake::experiments
