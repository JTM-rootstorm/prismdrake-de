#pragma once

#include <QObject>
#include <QString>

class QQmlEngine;

namespace prismdrake::shell::visual::test {

/// Writes candidate render artifacts and complete, machine-readable WP13 sidecar metadata.
class PanelVisualBaselineRecorder final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString artifactDirectory READ artifactDirectory CONSTANT)

  public:
    explicit PanelVisualBaselineRecorder(QObject *parent = nullptr);

    [[nodiscard]] QString artifactDirectory() const;
    Q_INVOKABLE [[nodiscard]] QString imagePath(const QString &testName) const;
    Q_INVOKABLE [[nodiscard]] bool expectedFontAvailable() const;
    Q_INVOKABLE [[nodiscard]] bool record(const QString &testName, QObject *themeGeneration,
                                          int width, int height, double devicePixelRatio,
                                          const QString &layoutDirection, bool blurAvailable,
                                          bool thumbnailsAvailable);
    Q_INVOKABLE [[nodiscard]] bool metadataComplete(const QString &testName) const;
    Q_INVOKABLE [[nodiscard]] bool imagesEqual(const QString &firstTestName,
                                               const QString &secondTestName) const;

  private:
    [[nodiscard]] QString metadataPath(const QString &testName) const;
};

class PanelVisualBaselineSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::visual::test
