#include "PresentationModel.hpp"
#include "X11DockAdapter.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QTextStream>
#include <QTimer>

#include <memory>

using prismdrake::experiments::PresentationModel;
using prismdrake::experiments::X11DockAdapter;

namespace {

int parseExitDelay(const QCommandLineParser &parser)
{
    bool valid = false;
    const int delay = parser.value(QStringLiteral("exit-after-ms")).toInt(&valid);
    return valid && delay >= 0 ? delay : -1;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("prismdrake-pd1-toolkit-spike"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("prismdrake.org"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Isolated Prismdrake PD1 Qt Quick and X11 evidence spike"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("theme-dir"),
                      QStringLiteral("Directory containing Prismdrake profile token files."),
                      QStringLiteral("path"),
                      QStringLiteral(PRISMDRAKE_SPIKE_DEFAULT_THEME_DIR)});
    parser.addOption({QStringLiteral("profile"),
                      QStringLiteral("Initial profile: lustre or forge."),
                      QStringLiteral("id"),
                      QStringLiteral("lustre")});
    parser.addOption({QStringLiteral("text-scale"),
                      QStringLiteral("Initial text scale from 1.0 through 2.0."),
                      QStringLiteral("scale"),
                      QStringLiteral("1.0")});
    parser.addOption({QStringLiteral("reduced-motion"),
                      QStringLiteral("Start with reduced motion enabled.")});
    parser.addOption({QStringLiteral("disable-transparency"),
                      QStringLiteral("Start with opaque fallback materials.")});
    parser.addOption({QStringLiteral("no-dock-properties"),
                      QStringLiteral("Do not apply the experimental X11 dock properties.")});
    parser.addOption({QStringLiteral("exit-after-ms"),
                      QStringLiteral("Exit after a bounded delay for restart testing."),
                      QStringLiteral("milliseconds")});
    parser.process(application);

    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        QTextStream(stderr) << "This evidence spike requires the Qt xcb platform; active platform is '"
                            << QGuiApplication::platformName() << "'.\n";
        return 2;
    }

    PresentationModel model(parser.value(QStringLiteral("theme-dir")));
    QString errorMessage;
    if (!model.setProfile(parser.value(QStringLiteral("profile")), &errorMessage)) {
        QTextStream(stderr) << errorMessage << '\n';
        return 2;
    }

    bool textScaleValid = false;
    const qreal textScale = parser.value(QStringLiteral("text-scale")).toDouble(&textScaleValid);
    if (!textScaleValid || textScale < 1.0 || textScale > 2.0) {
        QTextStream(stderr) << "--text-scale must be between 1.0 and 2.0.\n";
        return 2;
    }
    model.setTextScale(textScale);
    model.setReducedMotion(parser.isSet(QStringLiteral("reduced-motion")));
    model.setTransparencyDisabled(parser.isSet(QStringLiteral("disable-transparency")));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("presentationModel"), &model);
    engine.loadFromModule(
        QStringLiteral("org.prismdrake.experiments.toolkitspike"),
        QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 3;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        QTextStream(stderr) << "Spike root object is not a QQuickWindow.\n";
        return 3;
    }

    std::unique_ptr<X11DockAdapter> dockAdapter;
    if (!parser.isSet(QStringLiteral("no-dock-properties"))) {
        dockAdapter = std::make_unique<X11DockAdapter>();
        // winId() creates the native X11 window without mapping it. Complete
        // the checked property requests before show() so an EWMH window
        // manager observes the dock classification on the initial map.
        if (!dockAdapter->applyBottomDockProperties(
                static_cast<quint32>(window->winId()),
                window->geometry(),
                window->devicePixelRatio(),
                &errorMessage)) {
            QTextStream(stderr) << errorMessage << '\n';
            return 4;
        }
        QTextStream(stdout) << "PRISMDRAKE_SPIKE_WINDOW_ID=0x"
                            << QString::number(window->winId(), 16) << '\n';
    }
    window->show();
    QTextStream(stdout) << "PRISMDRAKE_SPIKE_DPR=" << window->devicePixelRatio() << '\n';

    const int exitDelay = parseExitDelay(parser);
    if (parser.isSet(QStringLiteral("exit-after-ms")) && exitDelay < 0) {
        QTextStream(stderr) << "--exit-after-ms must be a non-negative integer.\n";
        return 2;
    }
    if (exitDelay >= 0) {
        QTimer::singleShot(exitDelay, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
