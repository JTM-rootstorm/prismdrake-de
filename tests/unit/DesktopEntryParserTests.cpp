#include "DesktopEntryParser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;

[[nodiscard]] DesktopEntryParseContext locale(std::string value = "C") {
    return DesktopEntryParseContext{std::move(value)};
}

constexpr std::string_view minimalApplication =
    "[Desktop Entry]\nType=Application\nName=Prismdrake Tool\nExec=prismdrake-tool\n";

TEST(DesktopEntryParserTest, ParsesCompleteApplicationFieldsWithoutExecutingAnything) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Prismdrake Tool
GenericName=Desktop Tool
Comment=Displays\suseful\nstatus
Icon=prismdrake-tool
Keywords=desktop;settings\;control;utility;
Categories=Settings;Utility;
Exec=prismdrake-tool --open %U
TryExec=/usr/bin/prismdrake-tool
Path=/var/lib/prismdrake data
Terminal=true
Hidden=false
NoDisplay=true
DBusActivatable=false
OnlyShowIn=Prismdrake;Example;
NotShowIn=Other;
)";

    const auto result = parseDesktopEntry(document, locale());

    ASSERT_TRUE(result);
    const auto &entry = result.value();
    EXPECT_EQ(entry.name, "Prismdrake Tool");
    EXPECT_EQ(entry.genericName, "Desktop Tool");
    EXPECT_EQ(entry.comment, "Displays useful\nstatus");
    EXPECT_EQ(entry.icon, "prismdrake-tool");
    EXPECT_EQ(entry.keywords, (std::vector<std::string>{"desktop", "settings;control", "utility"}));
    EXPECT_EQ(entry.categories, (std::vector<std::string>{"Settings", "Utility"}));
    EXPECT_EQ(entry.exec, "prismdrake-tool --open %U");
    EXPECT_EQ(entry.tryExec, "/usr/bin/prismdrake-tool");
    EXPECT_EQ(entry.path, "/var/lib/prismdrake data");
    EXPECT_TRUE(entry.terminal);
    EXPECT_FALSE(entry.hidden);
    EXPECT_TRUE(entry.noDisplay);
    EXPECT_FALSE(entry.dbusActivatable);
    EXPECT_EQ(entry.onlyShowIn, std::optional<std::vector<std::string>>({"Prismdrake", "Example"}));
    EXPECT_EQ(entry.notShowIn, std::optional<std::vector<std::string>>({"Other"}));
}

TEST(DesktopEntryParserTest, UsesSpecificationLocaleFallbackOrderAfterRemovingEncoding) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Base
Name[sr]=Language
Name[sr@latin]=Language modifier
Name[sr_RS]=Country
Name[sr_RS@latin]=Exact
Keywords=base;
Keywords[sr]=language;
Keywords[sr_RS]=country;
Exec=tool
)";

    EXPECT_EQ(parseDesktopEntry(document, locale("sr_RS.UTF-8@latin")).value().name, "Exact");
    EXPECT_EQ(parseDesktopEntry(document, locale("sr_RS.UTF-8@other")).value().name, "Country");
    EXPECT_EQ(parseDesktopEntry(document, locale("sr_ME.UTF-8@latin")).value().name,
              "Language modifier");
    const auto language = parseDesktopEntry(document, locale("sr_ME.UTF-8"));
    ASSERT_TRUE(language);
    EXPECT_EQ(language.value().name, "Language");
    EXPECT_EQ(language.value().keywords, (std::vector<std::string>{"language"}));
    EXPECT_EQ(parseDesktopEntry(document, locale("de_DE.UTF-8")).value().name, "Base");
}

TEST(DesktopEntryParserTest, CPosixAndEmptyLocalesSelectOnlyBaseValues) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Base
Name[C]=Localized C must not win
Name[POSIX]=Localized POSIX must not win
Exec=tool
)";

    EXPECT_EQ(parseDesktopEntry(document, locale()).value().name, "Base");
    EXPECT_EQ(parseDesktopEntry(document, locale("POSIX")).value().name, "Base");
    EXPECT_EQ(parseDesktopEntry(document, locale("")).value().name, "Base");
    EXPECT_EQ(parseDesktopEntry(document, locale("C.UTF-8")).value().name, "Base");
    EXPECT_EQ(parseDesktopEntry(document, locale("POSIX.UTF-8")).value().name, "Base");
}

TEST(DesktopEntryParserTest, IgnoresAsciiSpacesAdjacentToEquals) {
    constexpr std::string_view document =
        "[Desktop Entry]\nType = Application\nName    =    Spaced Tool\nExec = tool\n";

    const auto result = parseDesktopEntry(document, locale());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().name, "Spaced Tool");
}

TEST(DesktopEntryParserTest, KeepsAbsentAndPresentEmptyVisibilityListsDistinct) {
    const auto absent = parseDesktopEntry(minimalApplication, locale());
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent.value().onlyShowIn.has_value());
    EXPECT_FALSE(absent.value().notShowIn.has_value());

    constexpr std::string_view presentEmpty = R"([Desktop Entry]
Type=Application
Name=Tool
Exec=tool
OnlyShowIn=
NotShowIn=
)";
    const auto present = parseDesktopEntry(presentEmpty, locale());
    ASSERT_TRUE(present);
    ASSERT_TRUE(present.value().onlyShowIn.has_value());
    ASSERT_TRUE(present.value().notShowIn.has_value());
    EXPECT_TRUE(present.value().onlyShowIn->empty());
    EXPECT_TRUE(present.value().notShowIn->empty());
}

TEST(DesktopEntryParserTest, AcceptsMinimalHiddenTombstoneBeforeLaunchableRequirements) {
    constexpr std::string_view document =
        "# masks a lower-precedence identifier\n[Desktop Entry]\nHidden=true\nX-Owner=example\n";

    const auto result = parseDesktopEntry(document, locale("en_US.UTF-8"));

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().hidden);
    EXPECT_FALSE(result.value().name.has_value());
    EXPECT_FALSE(result.value().exec.has_value());
}

TEST(DesktopEntryParserTest, AcceptsDbusActivatedApplicationWithoutExec) {
    constexpr std::string_view document =
        "[Desktop Entry]\nType=Application\nName=Settings\nDBusActivatable=true\n";

    const auto result = parseDesktopEntry(document, locale());

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().dbusActivatable);
    EXPECT_FALSE(result.value().exec.has_value());
}

TEST(DesktopEntryParserTest, AcceptsKnownIgnoredKeysAndExtensionSyntax) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Version=1.5
Name=Tool
Exec=tool
Actions=New;Open;
MimeType=text/plain;
Implements=org.example.Interface;
StartupNotify=true
StartupWMClass=Example
PrefersNonDefaultGPU=false
SingleMainWindow=true
URL=https://example.invalid/
X-Example-Value=opaque\qextension
X-Example-Value[en]=localized extension
[Desktop Action New]
Name=New
Exec=tool --new
UnknownActionExtension=value
)";

    EXPECT_TRUE(parseDesktopEntry(document, locale("en_US.UTF-8")));
}

TEST(DesktopEntryParserTest, AcceptsEqualsInExtensionGroupNames) {
    constexpr std::string_view document =
        "[Desktop Entry]\nHidden=true\n[X-Extension=Variant]\nOpaque=value\n";

    EXPECT_TRUE(parseDesktopEntry(document, locale()));
}

TEST(DesktopEntryParserTest, CanonicalizesEncodingInLocalizedKeySuffix) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Base
Name[en_US.UTF-8]=Encoded suffix
Exec=tool
)";

    const auto result = parseDesktopEntry(document, locale("en_US.ISO-8859-1"));

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().name, "Encoded suffix");
}

TEST(DesktopEntryParserTest, RejectsLocaleKeysThatCollideAfterEncodingRemoval) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Base
Name[en_US]=First private value
Name[en_US.UTF-8]=Second private value
Exec=tool
)";

    const auto result = parseDesktopEntry(document, locale("en_US.UTF-8"));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::validation_error);
    EXPECT_EQ(result.error().message.find("private"), std::string::npos);
}

TEST(DesktopEntryParserTest, RejectsLocalizedFamiliesWithoutUnlocalizedBase) {
    for (const std::string_view localized : {"GenericName[en]=Generic\n", "Comment[en]=Comment\n",
                                             "Icon[en]=icon\n", "Keywords[en]=one;\n"}) {
        std::string document = "[Desktop Entry]\nType=Application\nName=Base\nExec=tool\n";
        document.append(localized);
        const auto result = parseDesktopEntry(document, locale("en_US"));
        ASSERT_FALSE(result) << localized;
        EXPECT_EQ(result.error().code, ErrorCode::validation_error) << localized;
    }
}

TEST(DesktopEntryParserTest, AcceptsSupportedVersionsAndRejectsUnknownVersions) {
    for (const std::string_view version : {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5"}) {
        std::string document{minimalApplication};
        document.append("Version=").append(version).push_back('\n');
        EXPECT_TRUE(parseDesktopEntry(document, locale())) << version;
    }

    const auto newer = parseDesktopEntry(
        "[Desktop Entry]\nVersion=1.6\nType=Application\nName=private\nExec=tool\n", locale());
    ASSERT_FALSE(newer);
    EXPECT_EQ(newer.error().code, ErrorCode::unsupported);
    EXPECT_EQ(newer.error().message.find("private"), std::string::npos);
}

TEST(DesktopEntryParserTest, RejectsUnknownBareKeyInDesktopEntryGroup) {
    constexpr std::string_view document =
        "[Desktop Entry]\nType=Application\nName=private-name\nExec=tool\nUnknownKey=value\n";

    const auto result = parseDesktopEntry(document, locale());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
    EXPECT_EQ(result.error().message.find("private-name"), std::string::npos);
}

TEST(DesktopEntryParserTest, RequiresDesktopEntryAsFirstGroupAfterCommentsAndBlankLines) {
    constexpr std::string_view document =
        "# comment\n\n[Desktop Action New]\nName=New\n[Desktop Entry]\nHidden=true\n";

    const auto result = parseDesktopEntry(document, locale());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::validation_error);
}

TEST(DesktopEntryParserTest, RejectsMissingDuplicateAndMalformedStructure) {
    EXPECT_FALSE(parseDesktopEntry("# no group\n", locale()));
    EXPECT_FALSE(parseDesktopEntry("Name=outside\n", locale()));
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry\nHidden=true\n", locale()));
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry]\n=bad\n", locale()));
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry]\nBad Key=value\n", locale()));
    EXPECT_FALSE(
        parseDesktopEntry("[Desktop Entry]\nName[en_US..UTF-8]=bad\nHidden=true\n", locale()));
}

TEST(DesktopEntryParserTest, RejectsDuplicateGroupsAndExactKeys) {
    const auto group = parseDesktopEntry(
        "[Desktop Entry]\nHidden=true\n[Desktop Entry]\nName=private-name\n", locale());
    ASSERT_FALSE(group);
    EXPECT_EQ(group.error().code, ErrorCode::validation_error);

    const auto key = parseDesktopEntry(
        "[Desktop Entry]\nHidden=true\nX-Private=first\nX-Private=second\n", locale());
    ASSERT_FALSE(key);
    EXPECT_EQ(key.error().code, ErrorCode::validation_error);
    EXPECT_EQ(key.error().message.find("first"), std::string::npos);
    EXPECT_EQ(key.error().message.find("second"), std::string::npos);
}

TEST(DesktopEntryParserTest, RequiresApplicationTypeBaseNameAndActivationForLaunchableEntry) {
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry]\nName=Tool\nExec=tool\n", locale()));
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry]\nType=Application\nExec=tool\n", locale()));
    EXPECT_FALSE(
        parseDesktopEntry("[Desktop Entry]\nType=Application\nName[en]=Localized only\nExec=tool\n",
                          locale("en_US")));
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry]\nType=Application\nName=Tool\n", locale()));

    const auto wrongType =
        parseDesktopEntry("[Desktop Entry]\nType=Link\nName=Tool\nExec=tool\n", locale());
    ASSERT_FALSE(wrongType);
    EXPECT_EQ(wrongType.error().code, ErrorCode::unsupported);
}

TEST(DesktopEntryParserTest, RejectsInvalidBooleansEscapesAndListItems) {
    EXPECT_FALSE(parseDesktopEntry(
        "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool\nTerminal=TRUE\n", locale()));
    EXPECT_FALSE(parseDesktopEntry(
        "[Desktop Entry]\nType=Application\nName=Bad\\qName\nExec=tool\n", locale()));
    EXPECT_FALSE(parseDesktopEntry(
        "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool\nCategories=One;;Two;\n",
        locale()));
}

TEST(DesktopEntryParserTest, RejectsLiteralAndDecodedUnsafeExecControls) {
    const std::string literal =
        "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool\t--private\n";
    const auto literalResult = parseDesktopEntry(literal, locale());
    ASSERT_FALSE(literalResult);
    EXPECT_EQ(literalResult.error().code, ErrorCode::syntax_error);
    EXPECT_EQ(literalResult.error().message.find("private"), std::string::npos);

    const auto decoded = parseDesktopEntry(
        "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool\\n--private\n", locale());
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, ErrorCode::validation_error);
    EXPECT_EQ(decoded.error().message.find("private"), std::string::npos);
}

TEST(DesktopEntryParserTest, RejectsOverlappingVisibilityDesktopNames) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Tool
Exec=tool
OnlyShowIn=Prismdrake;Example;
NotShowIn=Other;Prismdrake;
)";

    const auto result = parseDesktopEntry(document, locale());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::validation_error);
}

TEST(DesktopEntryParserTest, RejectsInvalidUtf8AndNullWithoutDisclosingInput) {
    std::string invalid{minimalApplication};
    invalid.append("Comment=private-");
    invalid.push_back(static_cast<char>(0xC3));
    invalid.push_back('\n');
    const auto invalidResult = parseDesktopEntry(invalid, locale());
    ASSERT_FALSE(invalidResult);
    EXPECT_EQ(invalidResult.error().code, ErrorCode::syntax_error);
    EXPECT_EQ(invalidResult.error().message.find("private"), std::string::npos);

    std::string nul{minimalApplication};
    nul.append("Comment=private\0tail", 20U);
    const auto nulResult = parseDesktopEntry(nul, locale());
    ASSERT_FALSE(nulResult);
    EXPECT_EQ(nulResult.error().code, ErrorCode::syntax_error);
    EXPECT_EQ(nulResult.error().recovery.find("private"), std::string::npos);
}

TEST(DesktopEntryParserTest, EnforcesFileLineValueListAndLocaleBounds) {
    const auto oversizedFile =
        parseDesktopEntry(std::string(maximumDesktopEntryFileBytes + 1U, 'x'), locale());
    ASSERT_FALSE(oversizedFile);
    EXPECT_EQ(oversizedFile.error().code, ErrorCode::too_large);

    std::string longLine = "[Desktop Entry]\n#";
    longLine.append(maximumDesktopEntryLineBytes, 'x');
    const auto lineResult = parseDesktopEntry(longLine, locale());
    ASSERT_FALSE(lineResult);
    EXPECT_EQ(lineResult.error().code, ErrorCode::too_large);

    std::string longValue = "[Desktop Entry]\nHidden=true\nX-Big=";
    longValue.append(maximumDesktopEntryValueBytes + 1U, 'x');
    const auto valueResult = parseDesktopEntry(longValue, locale());
    ASSERT_FALSE(valueResult);
    EXPECT_EQ(valueResult.error().code, ErrorCode::too_large);

    std::string tooManyItems =
        "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool\nCategories=";
    for (std::size_t index = 0U; index <= maximumDesktopEntryListItems; ++index) {
        tooManyItems.append("Item;");
    }
    const auto listResult = parseDesktopEntry(tooManyItems, locale());
    ASSERT_FALSE(listResult);
    EXPECT_EQ(listResult.error().code, ErrorCode::too_large);

    const auto localeResult = parseDesktopEntry(
        minimalApplication, locale(std::string(maximumDesktopEntryLocaleBytes + 1U, 'x')));
    ASSERT_FALSE(localeResult);
    EXPECT_EQ(localeResult.error().code, ErrorCode::invalid_argument);
}

TEST(DesktopEntryParserTest, AcceptsExactLineListAndLocaleBounds) {
    std::string exactLine = "[Desktop Entry]\nHidden=true\n#";
    exactLine.append(maximumDesktopEntryLineBytes - 1U, 'x');
    EXPECT_TRUE(parseDesktopEntry(exactLine, locale()));

    std::string exactList = "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool\nCategories=";
    for (std::size_t index = 0U; index < maximumDesktopEntryListItems; ++index) {
        exactList.append("Item;");
    }
    const auto listResult = parseDesktopEntry(exactList, locale());
    ASSERT_TRUE(listResult);
    EXPECT_EQ(listResult.value().categories.size(), maximumDesktopEntryListItems);

    EXPECT_TRUE(parseDesktopEntry(minimalApplication,
                                  locale(std::string(maximumDesktopEntryLocaleBytes, 'a'))));
}

TEST(DesktopEntryParserTest, EnforcesLineGroupEntryAndCodepointCounts) {
    std::string tooManyLines;
    tooManyLines.reserve(maximumDesktopEntryLines * 2U);
    for (std::size_t index = 0U; index <= maximumDesktopEntryLines; ++index) {
        tooManyLines.append("#\n");
    }
    EXPECT_EQ(parseDesktopEntry(tooManyLines, locale()).error().code, ErrorCode::too_large);

    std::string tooManyGroups = "[Desktop Entry]\nHidden=true\n";
    for (std::size_t index = 1U; index <= maximumDesktopEntryGroups; ++index) {
        tooManyGroups.append("[X-").append(std::to_string(index)).append("]\n");
    }
    EXPECT_EQ(parseDesktopEntry(tooManyGroups, locale()).error().code, ErrorCode::too_large);

    std::string tooManyEntries = "[Desktop Entry]\nHidden=true\n";
    for (std::size_t index = 0U; index < maximumDesktopEntryEntries; ++index) {
        tooManyEntries.append("X-K").append(std::to_string(index)).append("=v\n");
    }
    EXPECT_EQ(parseDesktopEntry(tooManyEntries, locale()).error().code, ErrorCode::too_large);

    std::string tooManyCodepoints = "[Desktop Entry]\nHidden=true\nX-Large=";
    tooManyCodepoints.append(maximumDesktopEntryValueCodepoints + 1U, 'a');
    EXPECT_EQ(parseDesktopEntry(tooManyCodepoints, locale()).error().code, ErrorCode::too_large);
}

TEST(DesktopEntryParserTest, RejectsMalformedMessagesLocaleAndLocaleSuffix) {
    for (const std::string_view invalid :
         {"en__US", "en.UTF-8.extra", "en@one@two", "en.UTF-8@", "private locale"}) {
        const auto result = parseDesktopEntry(minimalApplication, locale(std::string{invalid}));
        ASSERT_FALSE(result) << invalid;
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument) << invalid;
        EXPECT_EQ(result.error().message.find(invalid), std::string::npos);
    }

    const auto badSuffix =
        parseDesktopEntry("[Desktop Entry]\nHidden=true\nName[en..UTF-8]=private\n", locale());
    ASSERT_FALSE(badSuffix);
    EXPECT_EQ(badSuffix.error().code, ErrorCode::syntax_error);
    EXPECT_EQ(badSuffix.error().message.find("private"), std::string::npos);
}

} // namespace
} // namespace prismdrake::launcher
