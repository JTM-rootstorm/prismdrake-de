#include "PresentationModel.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace prismdrake::experiments {

PresentationModel::PresentationModel(QString themeDirectory, QObject *parent)
    : QObject(parent)
    , tokenRepository_(std::move(themeDirectory))
{
}

bool PresentationModel::setProfile(const QString &profileId, QString *errorMessage)
{
    const auto candidate = tokenRepository_.loadProfile(profileId, errorMessage);
    if (!candidate) {
        return false;
    }

    tokens_ = *candidate;
    statusMessage_ = QStringLiteral("%1 profile active").arg(tokens_.profileDisplayName);
    emit presentationChanged();
    return true;
}

QString PresentationModel::profileId() const
{
    return tokens_.profileId;
}

QString PresentationModel::profileDisplayName() const
{
    return tokens_.profileDisplayName;
}

QString PresentationModel::panelColor() const
{
    QColor color = transparencyDisabled_ ? tokens_.panelFallback : tokens_.panelTint;
    color.setAlphaF(static_cast<float>(
        transparencyDisabled_ ? 1.0 : tokens_.panelOpacity));
    return colorName(color);
}

QString PresentationModel::elevatedColor() const
{
    return colorName(tokens_.elevatedSurface);
}

QString PresentationModel::borderActiveColor() const
{
    return colorName(tokens_.borderActive);
}

QString PresentationModel::borderInactiveColor() const
{
    return colorName(tokens_.borderInactive);
}

QString PresentationModel::focusColor() const
{
    return colorName(tokens_.focusRing);
}

QString PresentationModel::textPrimaryColor() const
{
    return colorName(tokens_.textPrimary);
}

QString PresentationModel::textMutedColor() const
{
    return colorName(tokens_.textMuted);
}

int PresentationModel::bodyFontPixels() const
{
    return qRound(tokens_.bodyFontPixels * textScale_);
}

int PresentationModel::titleFontPixels() const
{
    return qRound(tokens_.titleFontPixels * textScale_);
}

int PresentationModel::minimumTargetPixels() const
{
    return qRound(tokens_.minimumTargetPixels * textScale_);
}

int PresentationModel::borderWidthPixels() const
{
    return tokens_.borderWidthPixels;
}

int PresentationModel::focusWidthPixels() const
{
    return tokens_.focusWidthPixels;
}

int PresentationModel::taskRadiusPixels() const
{
    return tokens_.taskRadiusPixels;
}

int PresentationModel::motionDurationMs() const
{
    return reducedMotion_ ? 0 : tokens_.motionDurationMs;
}

qreal PresentationModel::textScale() const
{
    return textScale_;
}

bool PresentationModel::reducedMotion() const
{
    return reducedMotion_;
}

bool PresentationModel::transparencyDisabled() const
{
    return transparencyDisabled_;
}

bool PresentationModel::launcherVisible() const
{
    return launcherVisible_;
}

QStringList PresentationModel::tasks() const
{
    return tasks_;
}

int PresentationModel::activeTask() const
{
    return activeTask_;
}

QString PresentationModel::statusMessage() const
{
    return statusMessage_;
}

void PresentationModel::toggleProfile()
{
    QString errorMessage;
    const QString nextProfile = profileId() == QStringLiteral("lustre")
        ? QStringLiteral("forge")
        : QStringLiteral("lustre");
    if (!setProfile(nextProfile, &errorMessage)) {
        statusMessage_ = errorMessage;
        emit presentationChanged();
    }
}

void PresentationModel::setTextScale(qreal scale)
{
    const qreal boundedScale = std::clamp(scale, 1.0, 2.0);
    if (qFuzzyCompare(textScale_, boundedScale)) {
        return;
    }
    textScale_ = boundedScale;
    statusMessage_ = QStringLiteral("Text scale %1 percent")
                         .arg(qRound(textScale_ * 100.0));
    emit presentationChanged();
}

void PresentationModel::cycleTextScale()
{
    constexpr std::array<qreal, 3> scales{1.0, 1.25, 1.5};
    const auto current = std::find_if(scales.begin(), scales.end(), [this](qreal scale) {
        return qFuzzyCompare(textScale_, scale);
    });
    const auto next = current == scales.end() || std::next(current) == scales.end()
        ? scales.begin()
        : std::next(current);
    setTextScale(*next);
}

void PresentationModel::setReducedMotion(bool enabled)
{
    if (reducedMotion_ == enabled) {
        return;
    }
    reducedMotion_ = enabled;
    statusMessage_ = enabled
        ? QStringLiteral("Reduced motion enabled")
        : QStringLiteral("Reduced motion disabled");
    emit presentationChanged();
}

void PresentationModel::setTransparencyDisabled(bool enabled)
{
    if (transparencyDisabled_ == enabled) {
        return;
    }
    transparencyDisabled_ = enabled;
    statusMessage_ = enabled
        ? QStringLiteral("Transparency disabled; opaque fallback active")
        : QStringLiteral("Profile transparency active");
    emit presentationChanged();
}

void PresentationModel::activateLauncher()
{
    launcherVisible_ = true;
    statusMessage_ = QStringLiteral("Launcher sample opened");
    emit presentationChanged();
}

void PresentationModel::dismissLauncher()
{
    if (!launcherVisible_) {
        return;
    }
    launcherVisible_ = false;
    statusMessage_ = QStringLiteral("Launcher sample dismissed");
    emit presentationChanged();
}

void PresentationModel::activateTask(int index)
{
    if (index < 0 || index >= tasks_.size()) {
        return;
    }
    activeTask_ = index;
    statusMessage_ = QStringLiteral("%1 task selected").arg(tasks_.at(index));
    emit presentationChanged();
}

QString PresentationModel::colorName(QColor color) const
{
    return color.name(QColor::HexArgb);
}

} // namespace prismdrake::experiments
