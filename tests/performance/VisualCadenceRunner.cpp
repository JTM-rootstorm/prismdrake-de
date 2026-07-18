#include "PanelSurfaceQmlFixture.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QTimer>
#include <QtCore/qlogging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <cstdlib>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t warmupFrames = 2U;
constexpr std::size_t measuredIntervals = 240U;
constexpr auto seriesTimeout = std::chrono::seconds{10};
constexpr std::uint64_t descriptiveLongFrameNs = 25'000'000U;
std::atomic_bool qtWarningObserved{false};

void collectQtMessage(QtMsgType type, const QMessageLogContext &, const QString &) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        qtWarningObserved.store(true, std::memory_order_relaxed);
    }
    if (type == QtFatalMsg) {
        std::abort();
    }
}

struct Options final {
    QString sourceRevision;
    QString environmentId;
};

struct Scenario final {
    QString id;
    std::function<bool()> configure;
    bool blurAvailable;
};

[[nodiscard]] std::optional<Options> parseOptions(const QStringList &arguments) {
    Options result;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (index + 1 >= arguments.size()) {
            return std::nullopt;
        }
        const auto option = arguments[index];
        const auto value = arguments[++index];
        if (option == QStringLiteral("--revision") && result.sourceRevision.isEmpty()) {
            result.sourceRevision = value;
        } else if (option == QStringLiteral("--environment-id") && result.environmentId.isEmpty()) {
            result.environmentId = value;
        } else {
            return std::nullopt;
        }
    }
    static const QRegularExpression revisionPattern{
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$")};
    static const QRegularExpression environmentPattern{
        QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$")};
    if (!revisionPattern.match(result.sourceRevision).hasMatch() ||
        !environmentPattern.match(result.environmentId).hasMatch()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] QString environmentValue(const char *name) {
    return QString::fromUtf8(qgetenv(name));
}

[[nodiscard]] QString actualGraphicsBackend(const QQuickWindow &window) {
    return window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software
               ? QStringLiteral("software")
               : QStringLiteral("unsupported");
}

[[nodiscard]] QString actualRhiGraphicsApi() {
    switch (QQuickWindow::graphicsApi()) {
    case QSGRendererInterface::Unknown:
        return QStringLiteral("unknown");
    case QSGRendererInterface::Software:
        return QStringLiteral("software");
    case QSGRendererInterface::OpenVG:
        return QStringLiteral("openvg");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("opengl");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("direct3d11");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("null");
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("direct3d12");
#endif
    }
    return QStringLiteral("unsupported");
}

[[nodiscard]] QString actualFontFamily() {
    return QFontInfo{QFont{QStringLiteral("sans-serif")}}.family();
}

[[nodiscard]] bool environmentIsLocked() {
    return QLocale().name() == QStringLiteral("C") &&
           QDateTime::currentDateTime().offsetFromUtc() == 0 &&
           environmentValue("LANG") == QStringLiteral("C.UTF-8") &&
           environmentValue("LC_ALL") == QStringLiteral("C.UTF-8") &&
           environmentValue("TZ") == QStringLiteral("UTC") &&
           environmentValue("QT_QPA_PLATFORM") == QStringLiteral("offscreen") &&
           environmentValue("QT_QUICK_BACKEND") == QStringLiteral("software") &&
           environmentValue("QSG_RENDER_LOOP") == QStringLiteral("basic") &&
           environmentValue("QT_QUICK_CONTROLS_STYLE") == QStringLiteral("Basic") &&
           environmentValue("QT_SCALE_FACTOR") == QStringLiteral("1") &&
           actualFontFamily() == QString::fromUtf8(PRISMDRAKE_VISUAL_EXPECTED_FONT_FAMILY);
}

[[nodiscard]] QJsonArray samplesJson(const std::vector<std::uint64_t> &samples) {
    QJsonArray result;
    for (const auto sample : samples) {
        result.append(static_cast<qint64>(sample));
    }
    return result;
}

[[nodiscard]] QJsonObject summarize(const std::vector<std::uint64_t> &samples) {
    auto ordered = samples;
    std::ranges::sort(ordered);
    const auto p95Index = ((ordered.size() - 1U) * 95U + 99U) / 100U;
    const auto longFrames = static_cast<qint64>(std::ranges::count_if(
        samples, [](auto sample) { return sample > descriptiveLongFrameNs; }));
    return {{QStringLiteral("sample_count"), static_cast<qint64>(samples.size())},
            {QStringLiteral("minimum_ns"), static_cast<qint64>(ordered.front())},
            {QStringLiteral("median_ns"), static_cast<qint64>(ordered[(ordered.size() - 1U) / 2U])},
            {QStringLiteral("p95_ns"), static_cast<qint64>(ordered[p95Index])},
            {QStringLiteral("maximum_ns"), static_cast<qint64>(ordered.back())},
            {QStringLiteral("above_25000000_ns"), longFrames},
            {QStringLiteral("samples_ns"), samplesJson(samples)}};
}

[[nodiscard]] std::optional<std::vector<std::uint64_t>> measureSeries(QQuickWindow &window,
                                                                      QObject &root) {
    std::vector<std::uint64_t> samples;
    samples.reserve(measuredIntervals);
    std::size_t swaps = 0U;
    Clock::time_point previous{};
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto connection = QObject::connect(&window, &QQuickWindow::frameSwapped, &loop, [&] {
        const auto now = Clock::now();
        ++swaps;
        if (swaps == warmupFrames) {
            previous = now;
        } else if (swaps > warmupFrames) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous);
            if (elapsed.count() <= 0) {
                loop.quit();
                return;
            }
            samples.push_back(static_cast<std::uint64_t>(elapsed.count()));
            previous = now;
        }
        if (samples.size() == measuredIntervals) {
            loop.quit();
            return;
        }
        root.setProperty("cadencePhase", swaps % 2U == 0U ? 0.0 : 0.25);
        window.update();
    });
    deadline.start(seriesTimeout);
    window.update();
    loop.exec();
    QObject::disconnect(connection);
    if (samples.size() != measuredIntervals || !deadline.isActive()) {
        return std::nullopt;
    }
    return samples;
}

[[nodiscard]] bool settlePresentation() {
    QEventLoop loop;
    QTimer::singleShot(std::chrono::milliseconds{50}, &loop, &QEventLoop::quit);
    loop.exec();
    return true;
}

[[nodiscard]] int run(QGuiApplication &application) {
    const auto options = parseOptions(application.arguments());
    if (!options) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: invalid_arguments\n";
        return 2;
    }
    if (!environmentIsLocked()) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: environment_not_locked\n";
        return 2;
    }

    prismdrake::shell::panel::test::PanelSurfaceQmlFixture fixture;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("panelFixture"), &fixture);
    engine.load(QUrl::fromLocalFile(QStringLiteral(PRISMDRAKE_PERFORMANCE_QML_DIR) +
                                    QStringLiteral("/VisualCadenceSurface.qml")));
    if (engine.rootObjects().size() != 1) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: qml_load_failed\n";
        return 2;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if (!window) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: qml_root_invalid\n";
        return 2;
    }
    if (!settlePresentation()) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: presentation_not_settled\n";
        return 2;
    }
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: actual_qpa_not_locked\n";
        return 2;
    }
    if (actualRhiGraphicsApi() == QStringLiteral("unsupported")) {
        QTextStream{stderr} << "prismdrake-visual-cadence-evidence: unknown_graphics_api\n";
        return 2;
    }
    if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software) {
        QTextStream{stderr}
            << "prismdrake-visual-cadence-evidence: actual_graphics_backend_not_locked\n";
        return 2;
    }

    const std::array scenarios{
        Scenario{QStringLiteral("lustre"), [&] { return fixture.resetLustre(); }, true},
        Scenario{QStringLiteral("forge"),
                 [&] { return fixture.resetLustre() && fixture.publishForge(); }, true},
        Scenario{QStringLiteral("reduced_motion"), [&] { return fixture.resetReducedMotion(); },
                 true},
        Scenario{QStringLiteral("disabled_transparency"),
                 [&] { return fixture.resetTransparencyDisabled(); }, true},
        Scenario{QStringLiteral("missing_blur"), [&] { return fixture.resetLustreMissingBlur(); },
                 false},
    };

    QJsonArray series;
    for (const auto &scenario : scenarios) {
        if (!scenario.configure() || !fixture.publishRepresentativeTasks() ||
            !settlePresentation()) {
            QTextStream{stderr} << "prismdrake-visual-cadence-evidence: scenario_setup_failed\n";
            return 2;
        }
        auto samples = measureSeries(*window, *window);
        auto *generation = fixture.themeGeneration();
        if (!samples || !generation || qtWarningObserved.load(std::memory_order_relaxed)) {
            QTextStream{stderr} << "prismdrake-visual-cadence-evidence: frame_series_incomplete\n";
            return 2;
        }
        series.append(QJsonObject{
            {QStringLiteral("scenario"), scenario.id},
            {QStringLiteral("profile"), generation->property("profileId").toString()},
            {QStringLiteral("reduced_motion"), generation->property("reducedMotion").toBool()},
            {QStringLiteral("transparency_disabled"),
             generation->property("transparencyDisabled").toBool()},
            {QStringLiteral("blur_available"), scenario.blurAvailable},
            {QStringLiteral("statistics"), summarize(*samples)},
        });
    }

    const QJsonObject document{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("evidence_kind"), QStringLiteral("deterministic_visual_cadence")},
        {QStringLiteral("release_budget"), false},
        {QStringLiteral("source_revision"), options->sourceRevision},
        {QStringLiteral("reference_environment_id"), options->environmentId},
        {QStringLiteral("method"),
         QJsonObject{
             {QStringLiteral("clock"), QStringLiteral("std_chrono_steady_clock")},
             {QStringLiteral("warmup_frames"), static_cast<qint64>(warmupFrames)},
             {QStringLiteral("measured_intervals"), static_cast<qint64>(measuredIntervals)},
             {QStringLiteral("qpa_platform"), environmentValue("QT_QPA_PLATFORM")},
             {QStringLiteral("scenegraph_backend"), environmentValue("QT_QUICK_BACKEND")},
             {QStringLiteral("render_loop"), environmentValue("QSG_RENDER_LOOP")},
             {QStringLiteral("controls_style"), environmentValue("QT_QUICK_CONTROLS_STYLE")},
             {QStringLiteral("lang_environment"), environmentValue("LANG")},
             {QStringLiteral("lc_all_environment"), environmentValue("LC_ALL")},
             {QStringLiteral("timezone_environment"), environmentValue("TZ")},
             {QStringLiteral("qt_locale_name"), QLocale().name()},
             {QStringLiteral("runtime_utc_offset_seconds"),
              QDateTime::currentDateTime().offsetFromUtc()},
             {QStringLiteral("qpa_platform_actual"), QGuiApplication::platformName()},
             {QStringLiteral("graphics_backend_actual"), actualGraphicsBackend(*window)},
             {QStringLiteral("qt_graphics_api_reported"), actualRhiGraphicsApi()},
             {QStringLiteral("font_requested_family"), QStringLiteral("sans-serif")},
             {QStringLiteral("font_claimed_family"),
              QString::fromUtf8(PRISMDRAKE_VISUAL_EXPECTED_FONT_FAMILY)},
             {QStringLiteral("font_actual_family"), actualFontFamily()},
             {QStringLiteral("font_resolution_source"),
              QStringLiteral("fontconfig_fc_match_at_configure_time")},
             {QStringLiteral("font_source_basename"),
              QString::fromUtf8(PRISMDRAKE_VISUAL_FONT_SOURCE_BASENAME)},
             {QStringLiteral("font_source_sha256"),
              QString::fromUtf8(PRISMDRAKE_VISUAL_FONT_SOURCE_SHA256)}}},
        {QStringLiteral("series"), series},
        {QStringLiteral("limitations"),
         QJsonArray{QStringLiteral("offscreen_software_harness_not_production_gpu_or_compositor"),
                    QStringLiteral("above_25ms_count_is_descriptive_not_a_release_threshold")}},
        {QStringLiteral("redaction"),
         QJsonObject{{QStringLiteral("contains_filesystem_paths"), false},
                     {QStringLiteral("contains_process_or_thread_ids"), false},
                     {QStringLiteral("contains_host_or_user_names"), false},
                     {QStringLiteral("contains_application_or_window_content"), false},
                     {QStringLiteral("diagnostics_are_closed_ids"), true}}}};
    QTextStream{stdout} << QJsonDocument{document}.toJson(QJsonDocument::Indented);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    qInstallMessageHandler(collectQtMessage);
    QGuiApplication application{argc, argv};
    return run(application);
}
