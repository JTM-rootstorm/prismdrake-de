#include "PanelVisualBaselineRecorder.hpp"

#include "ShellThemeSnapshotAdapter.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace prismdrake::shell::visual::test {
namespace {

constexpr auto expectedFontFamily = "sans-serif";
constexpr auto expectedResolvedFontFamily = PRISMDRAKE_VISUAL_EXPECTED_FONT_FAMILY;
constexpr auto expectedFontSource = PRISMDRAKE_VISUAL_EXPECTED_FONT_SOURCE;

[[nodiscard]] bool validTestName(const QString &value) {
    static const QRegularExpression pattern{QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$")};
    return pattern.match(value).hasMatch();
}

[[nodiscard]] QByteArray fileDigest(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

[[nodiscard]] QString environmentValue(const char *name) {
    return QString::fromUtf8(qgetenv(name));
}

[[nodiscard]] QJsonObject sha256Record(const QByteArray &digest, const QString &basis) {
    return {{QStringLiteral("algorithm"), QStringLiteral("sha256")},
            {QStringLiteral("digest"), QString::fromLatin1(digest)},
            {QStringLiteral("basis"), basis}};
}

[[nodiscard]] bool validDigestRecord(const QJsonObject &value) {
    static const QRegularExpression digestPattern{QStringLiteral("^[0-9a-f]{64}$")};
    return value.value(QStringLiteral("algorithm")).toString() == QStringLiteral("sha256") &&
           digestPattern.match(value.value(QStringLiteral("digest")).toString()).hasMatch() &&
           !value.value(QStringLiteral("basis")).toString().isEmpty();
}

[[nodiscard]] bool imageHasVisualContent(const QImage &image, int expectedWidth,
                                         int expectedHeight) {
    if (image.isNull() || image.width() != expectedWidth || image.height() != expectedHeight) {
        return false;
    }
    std::array<QRgb, 8> colors{};
    std::size_t colorCount = 0U;
    bool hasVisiblePixel = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixel(x, y);
            hasVisiblePixel = hasVisiblePixel || qAlpha(pixel) != 0;
            if (colorCount < colors.size() &&
                std::find(colors.begin(), colors.begin() + static_cast<std::ptrdiff_t>(colorCount),
                          pixel) == colors.begin() + static_cast<std::ptrdiff_t>(colorCount)) {
                colors[colorCount++] = pixel;
            }
            if (hasVisiblePixel && colorCount == colors.size()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

PanelVisualBaselineRecorder::PanelVisualBaselineRecorder(QObject *parent) : QObject(parent) {
    static_cast<void>(QDir{}.mkpath(artifactDirectory()));
}

QString PanelVisualBaselineRecorder::artifactDirectory() const {
    return QString::fromUtf8(PRISMDRAKE_VISUAL_ARTIFACT_DIR);
}

QString PanelVisualBaselineRecorder::imagePath(const QString &testName) const {
    if (!validTestName(testName)) {
        return {};
    }
    return QDir(artifactDirectory()).filePath(testName + QStringLiteral(".png"));
}

QString PanelVisualBaselineRecorder::metadataPath(const QString &testName) const {
    if (!validTestName(testName)) {
        return {};
    }
    return QDir(artifactDirectory()).filePath(testName + QStringLiteral(".json"));
}

bool PanelVisualBaselineRecorder::expectedFontAvailable() const {
    const QFontInfo info{QFont{QString::fromLatin1(expectedFontFamily)}};
    return info.family() == QString::fromUtf8(expectedResolvedFontFamily);
}

bool PanelVisualBaselineRecorder::record(const QString &testName, QObject *themeGeneration,
                                         int width, int height, double devicePixelRatio,
                                         const QString &layoutDirection, bool blurAvailable,
                                         bool thumbnailsAvailable) {
    auto *generation =
        qobject_cast<prismdrake::shell::theme::ShellThemeGeneration *>(themeGeneration);
    const auto image = imagePath(testName);
    const auto metadata = metadataPath(testName);
    last_error_.clear();
    const auto reject = [this](QString error) {
        last_error_ = std::move(error);
        return false;
    };
    if (!generation) {
        return reject(QStringLiteral("theme generation has the wrong runtime type"));
    }
    if (image.isEmpty() || metadata.isEmpty()) {
        return reject(QStringLiteral("test name is not a valid artifact identifier"));
    }
    if (width <= 0 || height <= 0) {
        return reject(QStringLiteral("capture dimensions are not positive"));
    }
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return reject(QStringLiteral("device pixel ratio is not positive and finite"));
    }
    if (layoutDirection != QStringLiteral("ltr") && layoutDirection != QStringLiteral("rtl")) {
        return reject(QStringLiteral("layout direction is not ltr or rtl"));
    }
    if (!expectedFontAvailable()) {
        return reject(QStringLiteral("Qt did not resolve the configure-time font family"));
    }
    const auto imageHash = fileDigest(image);
    const auto &snapshot = generation->snapshot();
    if (imageHash.isEmpty()) {
        return reject(QStringLiteral("encoded capture could not be hashed"));
    }
    if (!snapshot) {
        return reject(QStringLiteral("theme generation has no retained snapshot"));
    }
    const QImage captured{image};
    if (captured.isNull()) {
        return reject(QStringLiteral("encoded capture could not be decoded"));
    }
    if (captured.width() != width || captured.height() != height) {
        return reject(QStringLiteral("decoded capture is %1x%2; expected %3x%4")
                          .arg(captured.width())
                          .arg(captured.height())
                          .arg(width)
                          .arg(height));
    }
    if (!imageHasVisualContent(captured, width, height)) {
        return reject(QStringLiteral("decoded capture has insufficient visible color content"));
    }

    const auto snapshotBytes = QByteArray::fromStdString(snapshot->serializedJson);
    const auto themeHash = QCryptographicHash::hash(snapshotBytes, QCryptographicHash::Sha256);
    const QFontInfo resolvedFont{QFont{generation->panel()->bodyFontFamily()}};

    const QJsonObject root{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("baseline_status"), QStringLiteral("candidate")},
        {QStringLiteral("test_name"), testName},
        {QStringLiteral("profile"), generation->profileId()},
        {QStringLiteral("settings_generation"), generation->generationId()},
        {QStringLiteral("theme_content"),
         sha256Record(themeHash.toHex(),
                      QStringLiteral("serialized complete settings and theme generation"))},
        {QStringLiteral("capabilities"),
         QJsonObject{{QStringLiteral("blur_available"), blurAvailable},
                     {QStringLiteral("thumbnails_available"), thumbnailsAvailable},
                     {QStringLiteral("safe_mode"), false}}},
        {QStringLiteral("transparency_mode"), generation->transparencyDisabled()
                                                  ? QStringLiteral("disabled")
                                                  : QStringLiteral("enabled")},
        {QStringLiteral("motion_mode"),
         generation->reducedMotion() ? QStringLiteral("reduced") : QStringLiteral("normal")},
        {QStringLiteral("contrast_mode"),
         generation->highContrast() ? QStringLiteral("high") : QStringLiteral("normal")},
        {QStringLiteral("text_scale"), generation->textScale()},
        {QStringLiteral("locale"), QLocale().name()},
        {QStringLiteral("layout_direction"), layoutDirection},
        {QStringLiteral("output"),
         QJsonObject{{QStringLiteral("width_px"), width},
                     {QStringLiteral("height_px"), height},
                     {QStringLiteral("device_pixel_ratio"), devicePixelRatio}}},
        {QStringLiteral("rendering"),
         QJsonObject{
             {QStringLiteral("qpa_platform"), environmentValue("QT_QPA_PLATFORM")},
             {QStringLiteral("scenegraph_backend"), environmentValue("QSG_RHI_BACKEND")},
             {QStringLiteral("render_loop"), environmentValue("QSG_RENDER_LOOP")},
             {QStringLiteral("controls_style"), environmentValue("QT_QUICK_CONTROLS_STYLE")}}},
        {QStringLiteral("font"),
         QJsonObject{
             {QStringLiteral("requested_family"), generation->panel()->bodyFontFamily()},
             {QStringLiteral("expected_requested_family"), QString::fromLatin1(expectedFontFamily)},
             {QStringLiteral("expected_resolved_family"),
              QString::fromUtf8(expectedResolvedFontFamily)},
             {QStringLiteral("resolved_family"), resolvedFont.family()},
             {QStringLiteral("source_id"),
              QStringLiteral("fontconfig:") + QString::fromUtf8(expectedFontSource)}}},
        {QStringLiteral("toolkit"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("Qt")},
                     {QStringLiteral("version"), QString::fromLatin1(qVersion())}}},
        {QStringLiteral("image"),
         QJsonObject{{QStringLiteral("file"), QFileInfo(image).fileName()},
                     {QStringLiteral("content"),
                      sha256Record(imageHash, QStringLiteral("encoded PNG artifact"))}}}};

    QSaveFile output(metadata);
    if (!output.open(QIODevice::WriteOnly)) {
        return reject(QStringLiteral("metadata sidecar could not be opened for writing"));
    }
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        return reject(QStringLiteral("metadata sidecar could not be committed atomically"));
    }
    return true;
}

bool PanelVisualBaselineRecorder::metadataComplete(const QString &testName) const {
    const auto metadata = metadataPath(testName);
    QFile input(metadata);
    if (metadata.isEmpty() || !input.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const auto root = document.object();
    const auto themeContent = root.value(QStringLiteral("theme_content")).toObject();
    const auto capabilities = root.value(QStringLiteral("capabilities")).toObject();
    const auto output = root.value(QStringLiteral("output")).toObject();
    const auto rendering = root.value(QStringLiteral("rendering")).toObject();
    const auto font = root.value(QStringLiteral("font")).toObject();
    const auto toolkit = root.value(QStringLiteral("toolkit")).toObject();
    const auto image = root.value(QStringLiteral("image")).toObject();
    const auto content = image.value(QStringLiteral("content")).toObject();
    const auto imageFile =
        QDir(artifactDirectory()).filePath(image.value(QStringLiteral("file")).toString());
    return root.value(QStringLiteral("schema_version")).toInt() == 1 &&
           root.value(QStringLiteral("baseline_status")).toString() ==
               QStringLiteral("candidate") &&
           root.value(QStringLiteral("test_name")).toString() == testName &&
           (root.value(QStringLiteral("profile")).toString() == QStringLiteral("lustre") ||
            root.value(QStringLiteral("profile")).toString() == QStringLiteral("forge")) &&
           validDigestRecord(themeContent) &&
           capabilities.contains(QStringLiteral("blur_available")) &&
           capabilities.contains(QStringLiteral("thumbnails_available")) &&
           capabilities.contains(QStringLiteral("safe_mode")) &&
           output.value(QStringLiteral("width_px")).toInt() > 0 &&
           output.value(QStringLiteral("height_px")).toInt() > 0 &&
           output.value(QStringLiteral("device_pixel_ratio")).toDouble() > 0.0 &&
           rendering.value(QStringLiteral("qpa_platform")).toString() ==
               QStringLiteral("offscreen") &&
           rendering.value(QStringLiteral("scenegraph_backend")).toString() ==
               QStringLiteral("software") &&
           !font.value(QStringLiteral("resolved_family")).toString().isEmpty() &&
           toolkit.value(QStringLiteral("name")).toString() == QStringLiteral("Qt") &&
           !toolkit.value(QStringLiteral("version")).toString().isEmpty() &&
           validDigestRecord(content) &&
           content.value(QStringLiteral("digest")).toString().toLatin1() == fileDigest(imageFile);
}

bool PanelVisualBaselineRecorder::imagesEqual(const QString &firstTestName,
                                              const QString &secondTestName) const {
    const auto first = imagePath(firstTestName);
    const auto second = imagePath(secondTestName);
    const auto firstHash = fileDigest(first);
    return !firstHash.isEmpty() && firstHash == fileDigest(second) &&
           QImage{first} == QImage{second};
}

} // namespace prismdrake::shell::visual::test
