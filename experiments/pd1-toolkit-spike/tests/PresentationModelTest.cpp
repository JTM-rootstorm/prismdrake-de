#include "PresentationModel.hpp"
#include "ThemeTokens.hpp"

#include <QColor>
#include <QSignalSpy>
#include <QtTest>

using prismdrake::experiments::PresentationModel;
using prismdrake::experiments::ThemeTokenRepository;

class PresentationModelTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsBothCommittedProfiles();
    void rejectsUnknownProfileWithoutReplacingState();
    void keepsAccessibilityPreferencesAcrossProfileSwitch();
    void selectsOpaqueFallbackWhenTransparencyIsDisabled();
    void exposesBoundedTextScalingAndReducedMotion();
    void modelsLauncherDismissalAndTaskSelection();
};

void PresentationModelTest::loadsBothCommittedProfiles()
{
    ThemeTokenRepository repository(QStringLiteral(PRISMDRAKE_SPIKE_TEST_THEME_DIR));
    QString error;
    const auto lustre = repository.loadProfile(QStringLiteral("lustre"), &error);
    QVERIFY2(lustre.has_value(), qPrintable(error));
    QCOMPARE(lustre->profileDisplayName, QStringLiteral("Prismdrake Lustre"));
    const auto forge = repository.loadProfile(QStringLiteral("forge"), &error);
    QVERIFY2(forge.has_value(), qPrintable(error));
    QCOMPARE(forge->profileDisplayName, QStringLiteral("Prismdrake Forge"));
    QCOMPARE(lustre->minimumTargetPixels, 44);
    QCOMPARE(forge->minimumTargetPixels, 44);
}

void PresentationModelTest::rejectsUnknownProfileWithoutReplacingState()
{
    PresentationModel model(QStringLiteral(PRISMDRAKE_SPIKE_TEST_THEME_DIR));
    QString error;
    QVERIFY2(model.setProfile(QStringLiteral("lustre"), &error), qPrintable(error));
    QVERIFY(!model.setProfile(QStringLiteral("unknown"), &error));
    QCOMPARE(model.profileId(), QStringLiteral("lustre"));
    QVERIFY(error.contains(QStringLiteral("Unsupported")));
}

void PresentationModelTest::keepsAccessibilityPreferencesAcrossProfileSwitch()
{
    PresentationModel model(QStringLiteral(PRISMDRAKE_SPIKE_TEST_THEME_DIR));
    QString error;
    QVERIFY2(model.setProfile(QStringLiteral("lustre"), &error), qPrintable(error));
    model.setTextScale(1.5);
    model.setReducedMotion(true);
    model.setTransparencyDisabled(true);
    model.toggleProfile();

    QCOMPARE(model.profileId(), QStringLiteral("forge"));
    QCOMPARE(model.textScale(), 1.5);
    QVERIFY(model.reducedMotion());
    QVERIFY(model.transparencyDisabled());
}

void PresentationModelTest::selectsOpaqueFallbackWhenTransparencyIsDisabled()
{
    PresentationModel model(QStringLiteral(PRISMDRAKE_SPIKE_TEST_THEME_DIR));
    QString error;
    QVERIFY2(model.setProfile(QStringLiteral("lustre"), &error), qPrintable(error));
    const QColor translucent(model.panelColor());
    QVERIFY(translucent.alphaF() < 1.0);

    model.setTransparencyDisabled(true);
    const QColor opaque(model.panelColor());
    const QColor opaqueElevated(model.elevatedColor());
    QCOMPARE(opaque.alphaF(), 1.0);
    QCOMPARE(opaqueElevated.alphaF(), 1.0);
    QCOMPARE(opaque, QColor(0x20, 0x2a, 0x42, 0xff));
}

void PresentationModelTest::exposesBoundedTextScalingAndReducedMotion()
{
    PresentationModel model(QStringLiteral(PRISMDRAKE_SPIKE_TEST_THEME_DIR));
    QString error;
    QVERIFY2(model.setProfile(QStringLiteral("forge"), &error), qPrintable(error));
    const int originalDuration = model.motionDurationMs();
    QVERIFY(originalDuration > 0);

    model.setTextScale(9.0);
    QCOMPARE(model.textScale(), 2.0);
    QCOMPARE(model.bodyFontPixels(), 28);
    model.setReducedMotion(true);
    QCOMPARE(model.motionDurationMs(), 0);
}

void PresentationModelTest::modelsLauncherDismissalAndTaskSelection()
{
    PresentationModel model(QStringLiteral(PRISMDRAKE_SPIKE_TEST_THEME_DIR));
    QString error;
    QVERIFY2(model.setProfile(QStringLiteral("lustre"), &error), qPrintable(error));
    QSignalSpy changed(&model, &PresentationModel::presentationChanged);

    model.activateLauncher();
    QVERIFY(model.launcherVisible());
    model.dismissLauncher();
    QVERIFY(!model.launcherVisible());
    model.activateTask(2);
    QCOMPARE(model.activeTask(), 2);
    QCOMPARE(model.statusMessage(), QStringLiteral("Settings task selected"));
    QCOMPARE(changed.count(), 3);
}

QTEST_GUILESS_MAIN(PresentationModelTest)

#include "PresentationModelTest.moc"
