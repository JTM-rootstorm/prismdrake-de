#include "TranslationCatalog.hpp"

#include <QCoreApplication>
#include <QLocale>

#include <gtest/gtest.h>

namespace prismdrake::shell::runtime {
namespace {

QCoreApplication &application() {
    auto *instance = QCoreApplication::instance();
    EXPECT_NE(instance, nullptr);
    return *instance;
}

TEST(TranslationCatalogTest, LoadsExpandedPseudoLocaleAndRestoresSourceStrings) {
    constexpr auto context = "LogoutConfirmationSurface";
    constexpr auto source = "Log out?";
    EXPECT_EQ(QCoreApplication::translate(context, source), QStringLiteral("Log out?"));

    {
        auto catalog = TranslationCatalog::install(application(), defaultTranslationDirectory(),
                                                   {QStringLiteral("en-XA")});
        ASSERT_TRUE(catalog->loaded());
        EXPECT_EQ(catalog->loadedLanguage(), QStringLiteral("en_XA"));
        const auto translated = QCoreApplication::translate(context, source);
        EXPECT_TRUE(translated.startsWith(QStringLiteral("[!!")));
        EXPECT_GT(translated.size(), QStringLiteral("Log out?").size() * 3);
    }

    EXPECT_EQ(QCoreApplication::translate(context, source), QStringLiteral("Log out?"));
}

TEST(TranslationCatalogTest, PreservesPlaceholdersInExpandedPseudoLocale) {
    auto catalog = TranslationCatalog::install(application(), defaultTranslationDirectory(),
                                               {QStringLiteral("en_XA")});
    ASSERT_TRUE(catalog->loaded());

    const auto translated =
        QCoreApplication::translate("QuickSettingsSurface", "%1 Current state: %2.");
    EXPECT_TRUE(translated.contains(QStringLiteral("%1")));
    EXPECT_TRUE(translated.contains(QStringLiteral("%2")));
    const auto rendered = translated.arg(QStringLiteral("Network"), QStringLiteral("offline"));
    EXPECT_TRUE(rendered.contains(QStringLiteral("Network")));
    EXPECT_TRUE(rendered.contains(QStringLiteral("offline")));
}

TEST(TranslationCatalogTest, LoadsRtlPseudoLocaleWithoutOwningLayoutPolicy) {
    auto catalog = TranslationCatalog::install(application(), defaultTranslationDirectory(),
                                               {QStringLiteral("ar_XB")});
    ASSERT_TRUE(catalog->loaded());
    EXPECT_EQ(catalog->loadedLanguage(), QStringLiteral("ar_XB"));
    EXPECT_NE(QCoreApplication::translate("LogoutConfirmationSurface", "Log out?"),
              QStringLiteral("Log out?"));
    EXPECT_EQ(QLocale(QLocale::Arabic).textDirection(), Qt::RightToLeft);
}

TEST(TranslationCatalogTest, RejectsUntrustedTagsAndFailsSoftWhenCatalogIsMissing) {
    auto catalog = TranslationCatalog::install(
        application(), defaultTranslationDirectory(),
        {QStringLiteral("../../private"), QStringLiteral("éé"), QStringLiteral("zz_ZZ")});
    ASSERT_FALSE(catalog->loaded());
    EXPECT_TRUE(catalog->loadedLanguage().isEmpty());
    EXPECT_EQ(QCoreApplication::translate("LogoutConfirmationSurface", "Log out?"),
              QStringLiteral("Log out?"));
}

TEST(TranslationCatalogTest, RejectsRelativeCatalogDirectories) {
    auto catalog = TranslationCatalog::install(application(), QStringLiteral("translations"),
                                               {QStringLiteral("en_XA")});
    EXPECT_FALSE(catalog->loaded());
}

} // namespace
} // namespace prismdrake::shell::runtime
