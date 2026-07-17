#include "DesktopEntryParser.hpp"
#include "DesktopExec.hpp"
#include "Result.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;

[[nodiscard]] DesktopEntry application(std::string exec) {
    DesktopEntry entry;
    entry.name = "Prismdrake Tool";
    entry.exec = std::move(exec);
    return entry;
}

[[nodiscard]] DesktopExecExpansionContext targets(DesktopExecTargetKind kind,
                                                  std::vector<std::string> values) {
    DesktopExecExpansionContext context;
    context.targetKind = kind;
    context.targets = std::move(values);
    return context;
}

[[nodiscard]] std::vector<std::string> argv(const std::string &exec) {
    const auto result = expandDesktopExec(application(exec), {});
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    if (!result || result.value().empty()) {
        return {};
    }
    EXPECT_EQ(result.value().size(), 1U);
    return result.value().front().argv;
}

TEST(DesktopExecTest, TokenizesWholeQuotedAndExplicitEmptyArguments) {
    EXPECT_EQ(argv(R"(tool   --flag "two words" "" tail)"),
              (std::vector<std::string>{"tool", "--flag", "two words", "", "tail"}));
}

TEST(DesktopExecTest, AppliesExecQuotingAfterDesktopEntryGeneralEscapes) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Tool
Exec=tool "money\\$value" "tick\\`value" "slash\\\\value"
)";
    const auto parsed = parseDesktopEntry(document, DesktopEntryParseContext{"C"});
    ASSERT_TRUE(parsed);

    const auto result = expandDesktopExec(parsed.value(), {});

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value().front().argv,
              (std::vector<std::string>{"tool", "money$value", "tick`value", "slash\\value"}));
}

TEST(DesktopExecTest, PreservesProperlyQuotedReservedCharactersAsLiteralArguments) {
    EXPECT_EQ(
        argv(
            R"(tool "say \"hello\"" "money \$value" "tick \`value" "slash \\ value" "; & | < > * ? # ( ) ~ '")"),
        (std::vector<std::string>{"tool", "say \"hello\"", "money $value", "tick `value",
                                  "slash \\ value", "; & | < > * ? # ( ) ~ '"}));
}

TEST(DesktopExecTest, RejectsUnmatchedPartialAndInvalidQuotedForms) {
    for (const std::string_view invalid :
         {R"(tool "unterminated)", R"(tool pre"quoted")", R"(tool "quoted"tail)", R"(tool "\q")",
          R"(tool "$value")", R"(tool "`value")"}) {
        const auto result = expandDesktopExec(application(std::string{invalid}), {});
        ASSERT_FALSE(result) << invalid;
        EXPECT_EQ(result.error().code, ErrorCode::syntax_error) << invalid;
    }
}

TEST(DesktopExecTest, RejectsEveryUnquotedReservedCharacter) {
    constexpr std::string_view reserved = "\"'\\><~|&;$*?#()`";
    for (char character : reserved) {
        const auto result = expandDesktopExec(
            application("tool before" + std::string(1U, character) + "after"), {});
        ASSERT_FALSE(result) << character;
        EXPECT_EQ(result.error().code, ErrorCode::syntax_error) << character;
    }
}

TEST(DesktopExecTest, ExpandsLiteralPercentAndRejectsInvalidPercentForms) {
    EXPECT_EQ(argv(R"(tool 100%% "quoted%%")"),
              (std::vector<std::string>{"tool", "100%", "quoted%"}));

    for (const std::string_view invalid : {"tool %", "tool %1", "tool %Q", "tool \"%c\""}) {
        const auto result = expandDesktopExec(application(std::string{invalid}), {});
        ASSERT_FALSE(result) << invalid;
        EXPECT_TRUE(result.error().code == ErrorCode::syntax_error ||
                    result.error().code == ErrorCode::validation_error ||
                    result.error().code == ErrorCode::unsupported)
            << invalid;
    }
}

TEST(DesktopExecTest, RejectsAllFieldCodesInExecutableExceptLiteralPercent) {
    EXPECT_EQ(argv("tool%% arg"), (std::vector<std::string>{"tool%", "arg"}));
    for (const std::string_view invalid : {"%c arg", "%d arg", "tool%k arg"}) {
        const auto result = expandDesktopExec(application(std::string{invalid}), {});
        ASSERT_FALSE(result) << invalid;
        EXPECT_EQ(result.error().code, ErrorCode::validation_error) << invalid;
    }
    for (const std::string_view invalid : {"\"\" arg", "bad=name arg"}) {
        const auto result = expandDesktopExec(application(std::string{invalid}), {});
        ASSERT_FALSE(result) << invalid;
        EXPECT_EQ(result.error().code, ErrorCode::validation_error) << invalid;
    }
}

TEST(DesktopExecTest, RequiresMultiArgumentCodesToStandAlone) {
    for (const std::string_view invalid : {"tool pre%F", "tool %Upost", "tool --icon=%i"}) {
        const auto result = expandDesktopExec(application(std::string{invalid}), {});
        ASSERT_FALSE(result) << invalid;
        EXPECT_EQ(result.error().code, ErrorCode::validation_error) << invalid;
    }
}

TEST(DesktopExecTest, RejectsMoreThanOneFileOrUrlFieldCode) {
    for (const std::string_view invalid : {"tool %f %u", "tool %F %F", "tool pre%u%f"}) {
        const auto result = expandDesktopExec(application(std::string{invalid}), {});
        ASSERT_FALSE(result) << invalid;
        EXPECT_EQ(result.error().code, ErrorCode::validation_error) << invalid;
    }
}

TEST(DesktopExecTest, FansOutSingleFileTargetsWithoutRescanningReplacements) {
    auto context = targets(DesktopExecTargetKind::localFiles, {"/tmp/one file", "/tmp/%c"});
    const auto result = expandDesktopExec(application("tool --open=%f tail"), context);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_EQ(result.value()[0].argv,
              (std::vector<std::string>{"tool", "--open=/tmp/one file", "tail"}));
    EXPECT_EQ(result.value()[1].argv, (std::vector<std::string>{"tool", "--open=/tmp/%c", "tail"}));
}

TEST(DesktopExecTest, FansOutSingleUrlForUriAndLocalFileInputs) {
    const auto uriResult = expandDesktopExec(
        application("tool %u"),
        targets(DesktopExecTargetKind::uris, {"https://example.invalid/a%20b", "file:///tmp/x"}));
    ASSERT_TRUE(uriResult);
    ASSERT_EQ(uriResult.value().size(), 2U);
    EXPECT_EQ(uriResult.value()[0].argv,
              (std::vector<std::string>{"tool", "https://example.invalid/a%20b"}));
    EXPECT_EQ(uriResult.value()[1].argv, (std::vector<std::string>{"tool", "file:///tmp/x"}));

    const auto fileResult = expandDesktopExec(
        application("tool %u"), targets(DesktopExecTargetKind::localFiles, {"/tmp/local path"}));
    ASSERT_TRUE(fileResult);
    EXPECT_EQ(fileResult.value().front().argv,
              (std::vector<std::string>{"tool", "/tmp/local path"}));
}

TEST(DesktopExecTest, ExpandsFileAndUrlListsIntoOneInvocation) {
    const auto files =
        expandDesktopExec(application("tool --before %F --after"),
                          targets(DesktopExecTargetKind::localFiles, {"/tmp/a", "/tmp/b b"}));
    ASSERT_TRUE(files);
    ASSERT_EQ(files.value().size(), 1U);
    EXPECT_EQ(files.value().front().argv,
              (std::vector<std::string>{"tool", "--before", "/tmp/a", "/tmp/b b", "--after"}));

    const auto uris = expandDesktopExec(
        application("tool %U"),
        targets(DesktopExecTargetKind::uris, {"https://example.invalid/a", "file:///tmp/b"}));
    ASSERT_TRUE(uris);
    ASSERT_EQ(uris.value().size(), 1U);
    EXPECT_EQ(uris.value().front().argv,
              (std::vector<std::string>{"tool", "https://example.invalid/a", "file:///tmp/b"}));
}

TEST(DesktopExecTest, RemovesTargetCodesWhenNoTargetWasSupplied) {
    EXPECT_EQ(argv("tool %F tail"), (std::vector<std::string>{"tool", "tail"}));
    EXPECT_EQ(argv("tool --open=%f tail"), (std::vector<std::string>{"tool", "--open=", "tail"}));
    EXPECT_EQ(argv("tool %u"), (std::vector<std::string>{"tool"}));
}

TEST(DesktopExecTest, RejectsTargetsWithoutCodeAndUrisForFileCodes) {
    auto supplied = targets(DesktopExecTargetKind::localFiles, {"/tmp/a"});
    const auto withoutCode = expandDesktopExec(application("tool"), supplied);
    ASSERT_FALSE(withoutCode);
    EXPECT_EQ(withoutCode.error().code, ErrorCode::validation_error);

    const auto remoteFile =
        expandDesktopExec(application("tool %f"),
                          targets(DesktopExecTargetKind::uris, {"https://example.invalid/a"}));
    ASSERT_FALSE(remoteFile);
    EXPECT_EQ(remoteFile.error().code, ErrorCode::unsupported);
}

TEST(DesktopExecTest, ExpandsNameIconAndDesktopFileWithoutRescanning) {
    auto entry = application("tool --name=%c %i %k %%");
    entry.name = "A %F Name";
    entry.icon = "icon%U";
    DesktopExecExpansionContext context;
    context.desktopFileLocation = "file:///tmp/%c.desktop";

    const auto result = expandDesktopExec(entry, context);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().front().argv,
              (std::vector<std::string>{"tool", "--name=A %F Name", "--icon", "icon%U",
                                        "file:///tmp/%c.desktop", "%"}));
}

TEST(DesktopExecTest, IconExpandsToZeroArgumentsWhenMissingOrEmpty) {
    EXPECT_EQ(argv("tool before %i after"), (std::vector<std::string>{"tool", "before", "after"}));
    auto entry = application("tool %i");
    entry.icon = "";
    const auto result = expandDesktopExec(entry, {});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().front().argv, (std::vector<std::string>{"tool"}));
}

TEST(DesktopExecTest, DesktopFileLocationMayBePathUriOrUnknown) {
    DesktopExecExpansionContext path;
    path.desktopFileLocation = "/usr/share/applications/tool.desktop";
    const auto pathResult = expandDesktopExec(application("tool %k"), path);
    ASSERT_TRUE(pathResult);
    EXPECT_EQ(pathResult.value().front().argv,
              (std::vector<std::string>{"tool", "/usr/share/applications/tool.desktop"}));

    DesktopExecExpansionContext uri;
    uri.desktopFileLocation = "file:///usr/share/applications/tool.desktop";
    const auto uriResult = expandDesktopExec(application("tool pre%kpost"), uri);
    ASSERT_TRUE(uriResult);
    EXPECT_EQ(
        uriResult.value().front().argv,
        (std::vector<std::string>{"tool", "prefile:///usr/share/applications/tool.desktoppost"}));

    const auto unknown = expandDesktopExec(application("tool %k"), {});
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown.value().front().argv, (std::vector<std::string>{"tool", ""}));
}

TEST(DesktopExecTest, EnforcesDesktopFileLocationBoundary) {
    DesktopExecExpansionContext exact;
    exact.desktopFileLocation = std::string(maximumDesktopExecLocationBytes, 'a');
    const auto exactResult = expandDesktopExec(application("tool %k"), exact);
    ASSERT_TRUE(exactResult);
    EXPECT_EQ(exactResult.value().front().argv.back().size(), maximumDesktopExecLocationBytes);

    exact.desktopFileLocation->push_back('a');
    const auto oversized = expandDesktopExec(application("tool %k"), exact);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(DesktopExecTest, RemovesDeprecatedCodesWithoutInventingEmptyArguments) {
    EXPECT_EQ(argv("tool %d pre%Npost after %v"),
              (std::vector<std::string>{"tool", "prepost", "after"}));
}

TEST(DesktopExecTest, RejectsDbusActivationAndMissingExec) {
    auto dbus = application("tool");
    dbus.dbusActivatable = true;
    const auto dbusResult = expandDesktopExec(dbus, {});
    ASSERT_FALSE(dbusResult);
    EXPECT_EQ(dbusResult.error().code, ErrorCode::unsupported);

    DesktopEntry missing;
    missing.name = "Tool";
    const auto missingResult = expandDesktopExec(missing, {});
    ASSERT_FALSE(missingResult);
    EXPECT_EQ(missingResult.error().code, ErrorCode::validation_error);
}

TEST(DesktopExecTest, RejectsNonAsciiControlAndOversizedRawExec) {
    std::string nonAscii = "tool ";
    nonAscii.append("\xC3\xA9", 2U);
    EXPECT_EQ(expandDesktopExec(application(nonAscii), {}).error().code,
              ErrorCode::validation_error);

    EXPECT_EQ(expandDesktopExec(application("tool\targ"), {}).error().code,
              ErrorCode::validation_error);

    std::string exact(maximumDesktopExecBytes, 'a');
    const auto exactResult = expandDesktopExec(application(exact), {});
    ASSERT_TRUE(exactResult);
    ASSERT_EQ(exactResult.value().front().argv.size(), 1U);
    EXPECT_EQ(exactResult.value().front().argv.front().size(), maximumDesktopExecBytes);

    exact.push_back('a');
    const auto oversized = expandDesktopExec(application(exact), {});
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(DesktopExecTest, EnforcesExactArgumentCountBoundary) {
    std::string exact = "tool";
    for (std::size_t index = 1U; index < maximumDesktopExecArguments; ++index) {
        exact.append(" a");
    }
    const auto exactResult = expandDesktopExec(application(exact), {});
    ASSERT_TRUE(exactResult);
    EXPECT_EQ(exactResult.value().front().argv.size(), maximumDesktopExecArguments);

    exact.append(" a");
    const auto tooMany = expandDesktopExec(application(exact), {});
    ASSERT_FALSE(tooMany);
    EXPECT_EQ(tooMany.error().code, ErrorCode::too_large);
}

TEST(DesktopExecTest, EnforcesArgumentCountAfterMultiArgumentExpansion) {
    std::string exact = "tool";
    for (std::size_t index = 1U; index < maximumDesktopExecArguments - 2U; ++index) {
        exact.append(" a");
    }
    exact.append(" %i");
    auto entry = application(exact);
    entry.icon = "icon";
    const auto exactResult = expandDesktopExec(entry, {});
    ASSERT_TRUE(exactResult);
    EXPECT_EQ(exactResult.value().front().argv.size(), maximumDesktopExecArguments);

    exact.insert(4U, " a");
    entry.exec = exact;
    const auto tooManyIconArguments = expandDesktopExec(entry, {});
    ASSERT_FALSE(tooManyIconArguments);
    EXPECT_EQ(tooManyIconArguments.error().code, ErrorCode::too_large);

    std::vector<std::string> fileTargets(maximumDesktopExecArguments - 1U, "/tmp/a");
    const auto exactFiles = expandDesktopExec(
        application("tool %F"), targets(DesktopExecTargetKind::localFiles, fileTargets));
    ASSERT_TRUE(exactFiles);
    EXPECT_EQ(exactFiles.value().front().argv.size(), maximumDesktopExecArguments);

    fileTargets.push_back("/tmp/b");
    const auto tooManyFiles = expandDesktopExec(
        application("tool %F"), targets(DesktopExecTargetKind::localFiles, fileTargets));
    ASSERT_FALSE(tooManyFiles);
    EXPECT_EQ(tooManyFiles.error().code, ErrorCode::too_large);
}

TEST(DesktopExecTest, EnforcesTargetCountAndPerTargetBoundaries) {
    std::vector<std::string> exactCount(maximumDesktopExecTargets, "/tmp/a");
    const auto exactCountResult = expandDesktopExec(
        application("tool %f"), targets(DesktopExecTargetKind::localFiles, exactCount));
    ASSERT_TRUE(exactCountResult);
    EXPECT_EQ(exactCountResult.value().size(), maximumDesktopExecTargets);

    exactCount.push_back("/tmp/b");
    const auto tooMany = expandDesktopExec(application("tool %f"),
                                           targets(DesktopExecTargetKind::localFiles, exactCount));
    ASSERT_FALSE(tooMany);
    EXPECT_EQ(tooMany.error().code, ErrorCode::too_large);

    std::string exactTarget(maximumDesktopExecTargetBytes, 'a');
    const auto exactTargetResult = expandDesktopExec(
        application("tool %u"), targets(DesktopExecTargetKind::uris, {exactTarget}));
    ASSERT_TRUE(exactTargetResult);
    EXPECT_EQ(exactTargetResult.value().front().argv.back().size(), maximumDesktopExecTargetBytes);

    exactTarget.push_back('a');
    const auto oversizedTarget = expandDesktopExec(
        application("tool %u"), targets(DesktopExecTargetKind::uris, {exactTarget}));
    ASSERT_FALSE(oversizedTarget);
    EXPECT_EQ(oversizedTarget.error().code, ErrorCode::too_large);
}

TEST(DesktopExecTest, EnforcesExactAggregateExpandedArgumentBoundary) {
    std::vector<std::string> exactTargets(15U, std::string(maximumDesktopExecTargetBytes, 'a'));
    exactTargets.emplace_back(maximumDesktopExecTargetBytes - 4U, 'b');
    const auto exact = expandDesktopExec(application("tool %F"),
                                         targets(DesktopExecTargetKind::localFiles, exactTargets));
    ASSERT_TRUE(exact);
    std::size_t exactBytes = 0U;
    for (const auto &argument : exact.value().front().argv) {
        exactBytes += argument.size();
    }
    EXPECT_EQ(exactBytes, maximumDesktopExecArgumentVectorBytes);

    exactTargets.back().push_back('b');
    const auto oversized = expandDesktopExec(
        application("tool %F"), targets(DesktopExecTargetKind::localFiles, exactTargets));
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(DesktopExecTest, RejectsMalformedExpansionContextAndNulls) {
    DesktopExecExpansionContext untyped;
    untyped.targets = {"/tmp/a"};
    EXPECT_EQ(expandDesktopExec(application("tool %f"), untyped).error().code,
              ErrorCode::invalid_argument);

    DesktopExecExpansionContext invalidKind;
    invalidKind.targetKind = static_cast<DesktopExecTargetKind>(255U);
    EXPECT_EQ(expandDesktopExec(application("tool"), invalidKind).error().code,
              ErrorCode::invalid_argument);

    std::string nulTarget{"private-target\0tail", 19U};
    EXPECT_EQ(
        expandDesktopExec(application("tool %u"), targets(DesktopExecTargetKind::uris, {nulTarget}))
            .error()
            .code,
        ErrorCode::invalid_argument);

    auto nulName = application("tool %c");
    nulName.name = std::string{"private-name\0tail", 17U};
    EXPECT_EQ(expandDesktopExec(nulName, {}).error().code, ErrorCode::validation_error);

    auto nulIcon = application("tool %i");
    nulIcon.icon = std::string{"private-icon\0tail", 17U};
    EXPECT_FALSE(expandDesktopExec(nulIcon, {}));

    DesktopExecExpansionContext nulLocation;
    nulLocation.desktopFileLocation = std::string{"private-location\0tail", 21U};
    EXPECT_EQ(expandDesktopExec(application("tool %k"), nulLocation).error().code,
              ErrorCode::invalid_argument);
}

TEST(DesktopExecTest, NeverDisclosesRejectedInputsInErrors) {
    const auto execResult = expandDesktopExec(application("tool;private-exec-sentinel"), {});
    ASSERT_FALSE(execResult);
    EXPECT_EQ(execResult.error().message.find("private-exec-sentinel"), std::string::npos);
    EXPECT_EQ(execResult.error().recovery.find("private-exec-sentinel"), std::string::npos);

    const auto targetResult = expandDesktopExec(
        application("tool %u"), targets(DesktopExecTargetKind::uris,
                                        {std::string(maximumDesktopExecTargetBytes + 1U, 's')}));
    ASSERT_FALSE(targetResult);
    EXPECT_EQ(targetResult.error().message.find(std::string(16U, 's')), std::string::npos);
    EXPECT_EQ(targetResult.error().recovery.find(std::string(16U, 's')), std::string::npos);
}

} // namespace
} // namespace prismdrake::launcher
