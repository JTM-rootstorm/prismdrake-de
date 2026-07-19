#pragma once

#include <QString>
#include <QStringList>

#include <memory>

class QCoreApplication;
class QTranslator;

namespace prismdrake::shell::runtime {

class TranslationCatalog final {
  public:
    [[nodiscard]] static std::unique_ptr<TranslationCatalog>
    install(QCoreApplication &application, const QString &directory,
            const QStringList &uiLanguages);

    ~TranslationCatalog();

    TranslationCatalog(const TranslationCatalog &) = delete;
    TranslationCatalog &operator=(const TranslationCatalog &) = delete;
    TranslationCatalog(TranslationCatalog &&) = delete;
    TranslationCatalog &operator=(TranslationCatalog &&) = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] const QString &loadedLanguage() const noexcept;

  private:
    explicit TranslationCatalog(QCoreApplication &application);

    QCoreApplication *application_;
    std::unique_ptr<QTranslator> translator_;
    QString loadedLanguage_;
};

[[nodiscard]] QString defaultTranslationDirectory();

} // namespace prismdrake::shell::runtime
