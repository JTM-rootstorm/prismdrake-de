#pragma once

#include "ThemeTokens.hpp"

#include <QObject>
#include <QStringList>

namespace prismdrake::experiments {

class PresentationModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString profileId READ profileId NOTIFY presentationChanged)
    Q_PROPERTY(QString profileDisplayName READ profileDisplayName NOTIFY presentationChanged)
    Q_PROPERTY(QString panelColor READ panelColor NOTIFY presentationChanged)
    Q_PROPERTY(QString elevatedColor READ elevatedColor NOTIFY presentationChanged)
    Q_PROPERTY(QString borderActiveColor READ borderActiveColor NOTIFY presentationChanged)
    Q_PROPERTY(QString borderInactiveColor READ borderInactiveColor NOTIFY presentationChanged)
    Q_PROPERTY(QString focusColor READ focusColor NOTIFY presentationChanged)
    Q_PROPERTY(QString textPrimaryColor READ textPrimaryColor NOTIFY presentationChanged)
    Q_PROPERTY(QString textMutedColor READ textMutedColor NOTIFY presentationChanged)
    Q_PROPERTY(int bodyFontPixels READ bodyFontPixels NOTIFY presentationChanged)
    Q_PROPERTY(int titleFontPixels READ titleFontPixels NOTIFY presentationChanged)
    Q_PROPERTY(int minimumTargetPixels READ minimumTargetPixels NOTIFY presentationChanged)
    Q_PROPERTY(int borderWidthPixels READ borderWidthPixels NOTIFY presentationChanged)
    Q_PROPERTY(int focusWidthPixels READ focusWidthPixels NOTIFY presentationChanged)
    Q_PROPERTY(int taskRadiusPixels READ taskRadiusPixels NOTIFY presentationChanged)
    Q_PROPERTY(int motionDurationMs READ motionDurationMs NOTIFY presentationChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY presentationChanged)
    Q_PROPERTY(bool reducedMotion READ reducedMotion NOTIFY presentationChanged)
    Q_PROPERTY(bool transparencyDisabled READ transparencyDisabled NOTIFY presentationChanged)
    Q_PROPERTY(bool launcherVisible READ launcherVisible NOTIFY launcherVisibleChanged)
    Q_PROPERTY(QStringList tasks READ tasks CONSTANT)
    Q_PROPERTY(int activeTask READ activeTask NOTIFY presentationChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY presentationChanged)

public:
    explicit PresentationModel(QString themeDirectory, QObject *parent = nullptr);

    [[nodiscard]] bool setProfile(const QString &profileId, QString *errorMessage = nullptr);
    [[nodiscard]] QString profileId() const;
    [[nodiscard]] QString profileDisplayName() const;
    [[nodiscard]] QString panelColor() const;
    [[nodiscard]] QString elevatedColor() const;
    [[nodiscard]] QString borderActiveColor() const;
    [[nodiscard]] QString borderInactiveColor() const;
    [[nodiscard]] QString focusColor() const;
    [[nodiscard]] QString textPrimaryColor() const;
    [[nodiscard]] QString textMutedColor() const;
    [[nodiscard]] int bodyFontPixels() const;
    [[nodiscard]] int titleFontPixels() const;
    [[nodiscard]] int minimumTargetPixels() const;
    [[nodiscard]] int borderWidthPixels() const;
    [[nodiscard]] int focusWidthPixels() const;
    [[nodiscard]] int taskRadiusPixels() const;
    [[nodiscard]] int motionDurationMs() const;
    [[nodiscard]] qreal textScale() const;
    [[nodiscard]] bool reducedMotion() const;
    [[nodiscard]] bool transparencyDisabled() const;
    [[nodiscard]] bool launcherVisible() const;
    [[nodiscard]] QStringList tasks() const;
    [[nodiscard]] int activeTask() const;
    [[nodiscard]] QString statusMessage() const;

    Q_INVOKABLE void toggleProfile();
    Q_INVOKABLE void setTextScale(qreal scale);
    Q_INVOKABLE void cycleTextScale();
    Q_INVOKABLE void setReducedMotion(bool enabled);
    Q_INVOKABLE void setTransparencyDisabled(bool enabled);
    Q_INVOKABLE void activateLauncher();
    Q_INVOKABLE void dismissLauncher();
    Q_INVOKABLE void activateTask(int index);

signals:
    void presentationChanged();
    void launcherVisibleChanged();

private:
    [[nodiscard]] QString colorName(QColor color) const;

    ThemeTokenRepository tokenRepository_;
    ThemeTokens tokens_;
    qreal textScale_ = 1.0;
    bool reducedMotion_ = false;
    bool transparencyDisabled_ = false;
    bool launcherVisible_ = false;
    QStringList tasks_{
        QStringLiteral("Files"),
        QStringLiteral("Terminal"),
        QStringLiteral("Settings"),
    };
    int activeTask_ = 0;
    QString statusMessage_ = QStringLiteral("Files task selected");
};

} // namespace prismdrake::experiments
