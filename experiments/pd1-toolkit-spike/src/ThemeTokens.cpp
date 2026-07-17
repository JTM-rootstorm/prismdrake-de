#include "ThemeTokens.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <algorithm>

namespace prismdrake::experiments {
namespace {

std::optional<QJsonObject> requireObject(
    const QJsonObject &parent,
    const QString &key,
    QString *errorMessage)
{
    const QJsonValue value = parent.value(key);
    if (!value.isObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Required token object '%1' is missing").arg(key);
        }
        return std::nullopt;
    }
    return value.toObject();
}

std::optional<QColor> requireColor(
    const QJsonObject &parent,
    const QString &key,
    QString *errorMessage)
{
    const QJsonValue value = parent.value(key);
    if (!value.isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Required color token '%1' is missing").arg(key);
        }
        return std::nullopt;
    }

    const QString encoded = value.toString();
    bool redValid = false;
    bool greenValid = false;
    bool blueValid = false;
    bool alphaValid = false;
    const int red = encoded.mid(1, 2).toInt(&redValid, 16);
    const int green = encoded.mid(3, 2).toInt(&greenValid, 16);
    const int blue = encoded.mid(5, 2).toInt(&blueValid, 16);
    const int alpha = encoded.mid(7, 2).toInt(&alphaValid, 16);
    if (encoded.size() != 9 || !encoded.startsWith(QLatin1Char('#'))
        || !redValid || !greenValid || !blueValid || !alphaValid) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "Color token '%1' must use the contract's #RRGGBBAA encoding")
                                .arg(key);
        }
        return std::nullopt;
    }
    return QColor(red, green, blue, alpha);
}

std::optional<int> requireInteger(
    const QJsonObject &parent,
    const QString &key,
    QString *errorMessage)
{
    const QJsonValue value = parent.value(key);
    if (!value.isDouble()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Required numeric token '%1' is missing").arg(key);
        }
        return std::nullopt;
    }
    return value.toInt();
}

} // namespace

ThemeTokenRepository::ThemeTokenRepository(QString themeDirectory)
    : themeDirectory_(std::move(themeDirectory))
{
}

std::optional<ThemeTokens> ThemeTokenRepository::loadProfile(
    const QString &profileId,
    QString *errorMessage) const
{
    static const QSet<QString> supportedProfiles{
        QStringLiteral("lustre"),
        QStringLiteral("forge"),
    };
    if (!supportedProfiles.contains(profileId)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unsupported spike profile '%1'").arg(profileId);
        }
        return std::nullopt;
    }

    const QString path = themeDirectory_ + QLatin1Char('/') + profileId
        + QStringLiteral(".tokens.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot read token file '%1': %2")
                                .arg(path, file.errorString());
        }
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot parse token file '%1': %2")
                                .arg(path, parseError.errorString());
        }
        return std::nullopt;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt(-1) != 1
        || root.value(QStringLiteral("layer")).toString() != QStringLiteral("profile")
        || root.value(QStringLiteral("profile_id")).toString() != profileId) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Token file '%1' has an unexpected identity or schema")
                                .arg(path);
        }
        return std::nullopt;
    }

    const auto semantic = requireObject(root, QStringLiteral("semantic"), errorMessage);
    const auto component = requireObject(root, QStringLiteral("component"), errorMessage);
    const auto overrides = requireObject(
        root, QStringLiteral("accessibility_overrides"), errorMessage);
    if (!semantic || !component || !overrides) {
        return std::nullopt;
    }

    const auto colors = requireObject(*semantic, QStringLiteral("colors"), errorMessage);
    const auto materials = requireObject(*semantic, QStringLiteral("materials"), errorMessage);
    const auto typography = requireObject(*semantic, QStringLiteral("typography"), errorMessage);
    const auto targets = requireObject(*semantic, QStringLiteral("targets"), errorMessage);
    const auto border = requireObject(*semantic, QStringLiteral("border"), errorMessage);
    const auto focus = requireObject(*semantic, QStringLiteral("focus"), errorMessage);
    const auto motion = requireObject(*semantic, QStringLiteral("motion"), errorMessage);
    const auto taskButton = requireObject(*component, QStringLiteral("task_button"), errorMessage);
    const auto panel = materials
        ? requireObject(*materials, QStringLiteral("panel"), errorMessage)
        : std::nullopt;
    const auto fallback = panel
        ? requireObject(*panel, QStringLiteral("fallback"), errorMessage)
        : std::nullopt;
    if (!colors || !materials || !typography || !targets || !border || !focus
        || !motion || !taskButton || !panel || !fallback) {
        return std::nullopt;
    }

    const auto panelTint = requireColor(*panel, QStringLiteral("tint"), errorMessage);
    const auto panelFallback = requireColor(*fallback, QStringLiteral("color"), errorMessage);
    const auto elevated = requireColor(
        *colors, QStringLiteral("elevated_surface"), errorMessage);
    const auto borderActive = requireColor(
        *colors, QStringLiteral("border_active"), errorMessage);
    const auto borderInactive = requireColor(
        *colors, QStringLiteral("border_inactive"), errorMessage);
    const auto focusRing = requireColor(*colors, QStringLiteral("focus_ring"), errorMessage);
    const auto textPrimary = requireColor(
        *colors, QStringLiteral("text_primary"), errorMessage);
    const auto textMuted = requireColor(*colors, QStringLiteral("text_muted"), errorMessage);
    const auto bodySize = requireInteger(
        *typography, QStringLiteral("body_size_px"), errorMessage);
    const auto titleSize = requireInteger(
        *typography, QStringLiteral("title_size_px"), errorMessage);
    const auto targetSize = requireInteger(
        *targets, QStringLiteral("minimum_px"), errorMessage);
    const auto overrideTarget = requireInteger(
        *overrides, QStringLiteral("minimum_target_size_px"), errorMessage);
    const auto borderWidth = requireInteger(
        *border, QStringLiteral("thickness_px"), errorMessage);
    const auto focusWidth = requireInteger(*focus, QStringLiteral("width_px"), errorMessage);
    const auto taskRadius = requireInteger(
        *taskButton, QStringLiteral("radius_px"), errorMessage);
    const auto motionDuration = requireInteger(
        *motion, QStringLiteral("normal_ms"), errorMessage);
    if (!panelTint || !panelFallback || !elevated || !borderActive || !borderInactive
        || !focusRing || !textPrimary || !textMuted || !bodySize || !titleSize
        || !targetSize || !overrideTarget || !borderWidth || !focusWidth
        || !taskRadius || !motionDuration) {
        return std::nullopt;
    }

    const QJsonValue opacityValue = panel->value(QStringLiteral("opacity"));
    if (!opacityValue.isDouble() || opacityValue.toDouble() < 0.0
        || opacityValue.toDouble() > 1.0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Panel opacity must be between zero and one");
        }
        return std::nullopt;
    }

    ThemeTokens tokens;
    tokens.profileId = profileId;
    tokens.profileDisplayName = root.value(QStringLiteral("profile_display_name")).toString();
    tokens.panelTint = *panelTint;
    tokens.panelOpacity = opacityValue.toDouble();
    tokens.panelFallback = *panelFallback;
    tokens.elevatedSurface = *elevated;
    tokens.borderActive = *borderActive;
    tokens.borderInactive = *borderInactive;
    tokens.focusRing = *focusRing;
    tokens.textPrimary = *textPrimary;
    tokens.textMuted = *textMuted;
    tokens.bodyFontPixels = *bodySize;
    tokens.titleFontPixels = *titleSize;
    tokens.minimumTargetPixels = std::max(*targetSize, *overrideTarget);
    tokens.borderWidthPixels = *borderWidth;
    tokens.focusWidthPixels = *focusWidth;
    tokens.taskRadiusPixels = *taskRadius;
    tokens.motionDurationMs = *motionDuration;
    return tokens;
}

} // namespace prismdrake::experiments
