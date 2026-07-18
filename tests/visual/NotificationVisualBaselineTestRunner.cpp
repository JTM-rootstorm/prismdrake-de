#include "NotificationListQmlFixture.hpp"
#include "PanelVisualBaselineRecorder.hpp"
#include "VisualThemeFixture.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <QtQuickTest/quicktest.h>

namespace prismdrake::shell::visual::test {

class NotificationVisualBaselineSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty(
            QStringLiteral("notificationFixture"),
            new prismdrake::shell::notifications::test::NotificationListQmlFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("visualThemeFixture"),
                                                  new VisualThemeFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("baselineRecorder"),
                                                  new PanelVisualBaselineRecorder(engine));
    }
};

} // namespace prismdrake::shell::visual::test

QUICK_TEST_MAIN_WITH_SETUP(NotificationVisualBaselineTest,
                           prismdrake::shell::visual::test::NotificationVisualBaselineSetup)

#include "NotificationVisualBaselineTestRunner.moc"
