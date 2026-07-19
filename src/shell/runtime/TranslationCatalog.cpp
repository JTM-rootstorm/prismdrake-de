#include "TranslationCatalog.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QTranslator>

#include <algorithm>

namespace prismdrake::shell::runtime {
namespace {

constexpr qsizetype maximumLanguageTagLength = 32;
constexpr qsizetype maximumLanguagePreferences = 8;

[[nodiscard]] bool isAsciiLetter(const QChar character) {
    return (character >= u'A' && character <= u'Z') || (character >= u'a' && character <= u'z');
}

[[nodiscard]] bool isValidLanguageTag(const QString &tag) {
    if (tag.size() < 2 || tag.size() > maximumLanguageTagLength || !isAsciiLetter(tag.front())) {
        return false;
    }
    return std::ranges::all_of(tag, [](const QChar character) {
        return isAsciiLetter(character) || (character >= u'0' && character <= u'9') ||
               character == u'_' || character == u'-';
    });
}

[[nodiscard]] QStringList candidatesFor(const QString &tag) {
    QString candidate = tag;
    candidate.replace(u'-', u'_');

    QStringList candidates;
    while (!candidate.isEmpty()) {
        candidates.push_back(candidate);
        const auto separator = candidate.lastIndexOf(u'_');
        if (separator < 0) {
            break;
        }
        candidate.truncate(separator);
    }
    return candidates;
}

} // namespace

TranslationCatalog::TranslationCatalog(QCoreApplication &application)
    : application_(&application), translator_(std::make_unique<QTranslator>()) {}

std::unique_ptr<TranslationCatalog> TranslationCatalog::install(QCoreApplication &application,
                                                                const QString &directory,
                                                                const QStringList &uiLanguages) {
    auto catalog = std::unique_ptr<TranslationCatalog>(new TranslationCatalog(application));
    if (!QDir::isAbsolutePath(directory)) {
        return catalog;
    }

    QStringList attempted;
    const auto preferenceCount = std::min(uiLanguages.size(), maximumLanguagePreferences);
    for (qsizetype index = 0; index < preferenceCount; ++index) {
        const auto &tag = uiLanguages[index];
        if (!isValidLanguageTag(tag)) {
            continue;
        }
        for (const auto &candidate : candidatesFor(tag)) {
            if (attempted.contains(candidate)) {
                continue;
            }
            attempted.push_back(candidate);
            const auto path =
                QDir(directory).filePath(QStringLiteral("prismdrake_%1.qm").arg(candidate));
            if (!catalog->translator_->load(path)) {
                continue;
            }
            if (!application.installTranslator(catalog->translator_.get())) {
                catalog->translator_ = std::make_unique<QTranslator>();
                continue;
            }
            catalog->loadedLanguage_ = candidate;
            return catalog;
        }
    }
    return catalog;
}

TranslationCatalog::~TranslationCatalog() {
    if (!loadedLanguage_.isEmpty()) {
        application_->removeTranslator(translator_.get());
    }
}

bool TranslationCatalog::loaded() const noexcept { return !loadedLanguage_.isEmpty(); }

const QString &TranslationCatalog::loadedLanguage() const noexcept { return loadedLanguage_; }

QString defaultTranslationDirectory() { return QStringLiteral(PRISMDRAKE_TRANSLATION_DIRECTORY); }

} // namespace prismdrake::shell::runtime
