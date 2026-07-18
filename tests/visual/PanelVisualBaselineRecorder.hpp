#pragma once

#include <QObject>
#include <QString>

namespace prismdrake::shell::visual::test {

/// Writes candidate render artifacts and complete, machine-readable WP13 sidecar metadata.
class PanelVisualBaselineRecorder final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString artifactDirectory READ artifactDirectory CONSTANT)
    Q_PROPERTY(QString lastError READ lastError)

  public:
    explicit PanelVisualBaselineRecorder(QObject *parent = nullptr);

    [[nodiscard]] QString artifactDirectory() const;
    [[nodiscard]] const QString &lastError() const noexcept { return last_error_; }
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

    QString last_error_;
};

} // namespace prismdrake::shell::visual::test
