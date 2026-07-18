#include "LauncherSurfaceQmlFixture.hpp"
#include "PanelVisualBaselineRecorder.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <QtQuickTest/quicktest.h>

namespace prismdrake::shell::visual::test {

class LauncherVisualBaselineSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty(
            QStringLiteral("launcherFixture"),
            new prismdrake::shell::launcher::test::LauncherSurfaceQmlFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("baselineRecorder"),
                                                  new PanelVisualBaselineRecorder(engine));
    }
};

} // namespace prismdrake::shell::visual::test

QUICK_TEST_MAIN_WITH_SETUP(LauncherVisualBaselineTest,
                           prismdrake::shell::visual::test::LauncherVisualBaselineSetup)

#include "LauncherVisualBaselineTestRunner.moc"
