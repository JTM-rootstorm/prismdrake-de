#include "LauncherPresentationModel.hpp"

#include "Cancellation.hpp"
#include "DesktopEntryParser.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QStringList>

#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::shell::launcher {
namespace {

using foundation::CancellationSource;
using foundation::ErrorCode;
using prismdrake::launcher::ApplicationCatalogDecision;
using prismdrake::launcher::ApplicationCatalogEligibilityReason;
using prismdrake::launcher::ApplicationCatalogSnapshot;
using prismdrake::launcher::ApplicationSearchOperation;
using prismdrake::launcher::ApplicationSearchSnapshot;
using prismdrake::launcher::DesktopEntry;
using prismdrake::launcher::DesktopEntryDiscoverySnapshot;
using prismdrake::launcher::DesktopEntryVisibilityReason;
using prismdrake::launcher::DiscoveredDesktopEntry;

QStringList *captured_warnings = nullptr;
QtMessageHandler prior_message_handler = nullptr;

void captureQtWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg && captured_warnings != nullptr) {
        captured_warnings->push_back(message);
        return;
    }
    if (prior_message_handler != nullptr) {
        prior_message_handler(type, context, message);
    }
}

class ScopedQtWarningCapture final {
  public:
    ScopedQtWarningCapture() {
        captured_warnings = &warnings_;
        prior_message_handler = qInstallMessageHandler(captureQtWarnings);
    }

    ~ScopedQtWarningCapture() {
        qInstallMessageHandler(prior_message_handler);
        prior_message_handler = nullptr;
        captured_warnings = nullptr;
    }

    ScopedQtWarningCapture(const ScopedQtWarningCapture &) = delete;
    ScopedQtWarningCapture &operator=(const ScopedQtWarningCapture &) = delete;

    [[nodiscard]] const QStringList &warnings() const noexcept { return warnings_; }

  private:
    QStringList warnings_;
};

[[nodiscard]] DiscoveredDesktopEntry discovered(std::string id, std::string name,
                                                std::string genericName = {},
                                                std::string comment = {}, std::string icon = {},
                                                bool terminal = false) {
    auto identifier = prismdrake::launcher::deriveDesktopFileId(id);
    if (!identifier) {
        throw std::runtime_error(identifier.error().message);
    }
    DesktopEntry entry;
    entry.name = std::move(name);
    if (!genericName.empty()) {
        entry.genericName = std::move(genericName);
    }
    if (!comment.empty()) {
        entry.comment = std::move(comment);
    }
    if (!icon.empty()) {
        entry.icon = std::move(icon);
    }
    entry.exec = "private --exec %U";
    entry.path = "/private/working/directory";
    entry.terminal = terminal;
    auto location =
        prismdrake::launcher::makeDiscoveredDesktopFileLocation("/fixture/applications", id, 0U);
    if (!location) {
        throw std::runtime_error(location.error().message);
    }
    return {std::move(identifier).value(), std::move(entry),
            DesktopEntryVisibilityReason::visibleByDefault, std::move(location).value()};
}

[[nodiscard]] std::shared_ptr<const ApplicationCatalogSnapshot>
catalog(std::vector<DiscoveredDesktopEntry> entries, std::uint64_t generation = 7U,
        bool complete = true) {
    std::vector<std::size_t> visible;
    std::vector<ApplicationCatalogDecision> decisions;
    visible.reserve(entries.size());
    decisions.reserve(entries.size());
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        visible.push_back(index);
        decisions.push_back({index, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec});
    }
    auto discovery = std::make_shared<const DesktopEntryDiscoverySnapshot>(
        DesktopEntryDiscoverySnapshot{std::move(entries), visible, {}, 0U, complete, false, false});
    return std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
        generation, discovery, std::move(decisions), std::move(visible),
        discovery->visibleEntryIndices.size(), discovery->visibleEntryIndices.size(), complete});
}

[[nodiscard]] ApplicationSearchOperation
searchOperation(std::shared_ptr<const ApplicationCatalogSnapshot> source, std::string_view text,
                std::uint64_t requestGeneration) {
    auto query = prismdrake::launcher::parseApplicationSearchQuery(text);
    if (!query) {
        throw std::runtime_error(query.error().message);
    }
    auto operation = prismdrake::launcher::createApplicationSearch(
        std::move(source), requestGeneration, std::move(query).value());
    if (!operation) {
        throw std::runtime_error(operation.error().message);
    }
    return std::move(operation).value();
}

[[nodiscard]] std::shared_ptr<const ApplicationSearchSnapshot>
advance(ApplicationSearchOperation &operation, std::size_t workUnits) {
    CancellationSource cancellation;
    auto snapshot = operation.advance(workUnits, cancellation.token());
    if (!snapshot) {
        throw std::runtime_error(snapshot.error().message);
    }
    return std::move(snapshot).value();
}

[[nodiscard]] std::shared_ptr<const ApplicationSearchSnapshot>
complete(ApplicationSearchOperation &operation) {
    return advance(operation, prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

TEST(LauncherPresentationModelTest, PresentsAllBoundedViewStatesAndOrderedRows) {
    const auto source =
        catalog({discovered("b.desktop", "Beta"), discovered("a.desktop", "Alpha")});
    auto loadingOperation = searchOperation(source, {}, 1U);
    const auto loading = advance(loadingOperation, 1U);
    LauncherPresentationModel loadingModel;
    ASSERT_TRUE(loadingModel.applySnapshot(source, loading));
    EXPECT_EQ(loadingModel.viewState(), LauncherPresentationModel::ViewState::loading);
    EXPECT_EQ(loadingModel.stateId(), QStringLiteral("loading"));
    EXPECT_FALSE(loadingModel.stateLabel().isEmpty());
    EXPECT_EQ(loadingModel.rowCount(), 1);

    const auto results = complete(loadingOperation);
    ASSERT_TRUE(loadingModel.applySnapshot(source, results));
    EXPECT_EQ(loadingModel.viewState(), LauncherPresentationModel::ViewState::results);
    ASSERT_EQ(loadingModel.rowCount(), 2);
    EXPECT_EQ(loadingModel.resultAt(0)->name(), QStringLiteral("Alpha"));
    EXPECT_EQ(loadingModel.resultAt(1)->name(), QStringLiteral("Beta"));

    auto absentOperation = searchOperation(source, "absent", 2U);
    LauncherPresentationModel absentModel;
    ASSERT_TRUE(absentModel.applySnapshot(source, complete(absentOperation)));
    EXPECT_EQ(absentModel.viewState(), LauncherPresentationModel::ViewState::noResults);
    EXPECT_EQ(absentModel.rowCount(), 0);

    const auto emptySource = catalog({}, 8U);
    auto emptyOperation = searchOperation(emptySource, {}, 1U);
    LauncherPresentationModel emptyModel;
    ASSERT_TRUE(emptyModel.applySnapshot(emptySource, complete(emptyOperation)));
    EXPECT_EQ(emptyModel.viewState(), LauncherPresentationModel::ViewState::emptyCatalog);

    auto error = prismdrake::launcher::makeApplicationSearchErrorSnapshot(7U, 3U);
    ASSERT_TRUE(error);
    LauncherPresentationModel errorModel;
    ASSERT_TRUE(errorModel.applySnapshot(source, error.value()));
    EXPECT_EQ(errorModel.viewState(), LauncherPresentationModel::ViewState::error);
    EXPECT_EQ(errorModel.stateId(), QStringLiteral("error"));
}

TEST(LauncherPresentationModelTest, PreservesLiteralLocalizedFieldsWithoutPrivateProperties) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Calculator
Name[fr]=Calculatrice
GenericName=Utility
GenericName[fr]=Utilitaire
Comment=Base comment
Comment[fr]=Commentaire localisé
Icon=accessories-calculator
Exec=private-tool --secret
Path=/private/path
Terminal=true
)";
    auto parsed = prismdrake::launcher::parseDesktopEntry(
        document, prismdrake::launcher::DesktopEntryParseContext{"fr_FR.UTF-8"});
    ASSERT_TRUE(parsed);
    auto identifier = prismdrake::launcher::deriveDesktopFileId("localized.desktop");
    ASSERT_TRUE(identifier);
    auto location = prismdrake::launcher::makeDiscoveredDesktopFileLocation(
        "/fixture/applications", "localized.desktop", 0U);
    ASSERT_TRUE(location);
    const auto source =
        catalog({{std::move(identifier).value(), std::move(parsed).value(),
                  DesktopEntryVisibilityReason::visibleByDefault, std::move(location).value()}});
    auto operation = searchOperation(source, "calculatrice", 1U);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, complete(operation)));

    auto *result = model.resultAt(0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->name(), QStringLiteral("Calculatrice"));
    EXPECT_EQ(result->genericName(), QStringLiteral("Utilitaire"));
    EXPECT_EQ(result->comment(), QStringLiteral("Commentaire localisé"));
    EXPECT_EQ(result->icon(), QStringLiteral("accessories-calculator"));
    EXPECT_TRUE(result->terminalRequired());
    const auto *metadata = result->metaObject();
    for (const char *property :
         {"desktopFileId", "discoveryEntryIndex", "catalogGeneration", "requestGeneration", "path",
          "exec", "desktopFileLocation", "sourceLocation"}) {
        EXPECT_EQ(metadata->indexOfProperty(property), -1) << property;
    }
}

TEST(LauncherPresentationModelTest, EmitsExactTypedLaunchIntentWithoutExecutionExpansion) {
    const auto source = catalog({discovered("dragon.desktop", "Dragon", {}, {}, "dragon")}, 19U);
    auto operation = searchOperation(source, "dragon", 23U);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, complete(operation)));
    std::optional<ApplicationLaunchIntent> captured;
    QObject::connect(&model, &LauncherPresentationModel::launchRequested, &model,
                     [&captured](const ApplicationLaunchIntent &intent) { captured = intent; });

    ASSERT_TRUE(model.resultAt(0)->requestLaunch());
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->desktopFileId.value(), "dragon.desktop");
    EXPECT_EQ(captured->catalogGeneration, 19U);
    EXPECT_EQ(captured->requestGeneration, 23U);
    EXPECT_EQ(model.resultAt(0)->metaObject()->indexOfProperty("exec"), -1);
}

TEST(LauncherPresentationModelTest, PreservesUnchangedIdentityAcrossFilteringAndReinsertion) {
    const auto source =
        catalog({discovered("one.desktop", "One"), discovered("two.desktop", "Two")});
    auto allFirst = searchOperation(source, {}, 1U);
    auto onlyTwo = searchOperation(source, "two", 2U);
    auto allAgain = searchOperation(source, {}, 3U);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, complete(allFirst)));
    auto *oneObject = model.resultAt(0);
    auto *twoObject = model.resultAt(1);
    QPointer<LauncherResultPresentation> onePointer = oneObject;
    bool aliveWhenRowsRemoved = false;
    bool aliveWhenPublicationApplied = false;
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                     [&] { aliveWhenRowsRemoved = !onePointer.isNull(); });
    QObject::connect(&model, &LauncherPresentationModel::publicationApplied, &model,
                     [&] { aliveWhenPublicationApplied = !onePointer.isNull(); });

    ASSERT_TRUE(model.applySnapshot(source, complete(onlyTwo)));
    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.resultAt(0), twoObject);
    EXPECT_TRUE(aliveWhenRowsRemoved);
    EXPECT_TRUE(aliveWhenPublicationApplied);
    ASSERT_FALSE(onePointer.isNull());
    EXPECT_FALSE(onePointer->requestLaunch());

    ASSERT_TRUE(model.applySnapshot(source, complete(allAgain)));
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.resultAt(1), twoObject);
    EXPECT_NE(model.resultAt(0), oneObject);
    EXPECT_FALSE(onePointer.isNull());
    EXPECT_FALSE(onePointer->requestLaunch());
}

TEST(LauncherPresentationModelTest, ReordersRetainedIdentitiesWithValidModelSignals) {
    const auto source = catalog({discovered("dragon-editor.desktop", "Dragon Editor"),
                                 discovered("editor-dragon.desktop", "Editor Dragon")});
    auto alphabeticalOperation = searchOperation(source, {}, 1U);
    auto rankedOperation = searchOperation(source, "editor", 2U);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, complete(alphabeticalOperation)));
    auto *dragonEditor = model.resultAt(0);
    auto *editorDragon = model.resultAt(1);

    QAbstractItemModelTester tester(&model,
                                    QAbstractItemModelTester::FailureReportingMode::Warning);
    int rowsMoved = 0;
    QObject::connect(&model, &QAbstractItemModel::rowsMoved, &model,
                     [&rowsMoved](const QModelIndex &, int first, int last, const QModelIndex &,
                                  int destination) {
                         ++rowsMoved;
                         EXPECT_EQ(first, 1);
                         EXPECT_EQ(last, 1);
                         EXPECT_EQ(destination, 0);
                     });
    ScopedQtWarningCapture warnings;

    ASSERT_TRUE(model.applySnapshot(source, complete(rankedOperation)));
    EXPECT_EQ(rowsMoved, 1);
    EXPECT_TRUE(warnings.warnings().isEmpty())
        << warnings.warnings().join(QLatin1Char('\n')).toStdString();
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.resultAt(0), editorDragon);
    EXPECT_EQ(model.resultAt(1), dragonEditor);
}

TEST(LauncherPresentationModelTest, ReplacesChangedContentAndUsesNewCurrentPublication) {
    const auto oldCatalog = catalog(
        {discovered("one.desktop", "One", {}, "Old comment"), discovered("two.desktop", "Two")},
        7U);
    const auto newCatalog = catalog(
        {discovered("one.desktop", "One", {}, "New comment"), discovered("two.desktop", "Two")},
        8U);
    auto oldOperation = searchOperation(oldCatalog, {}, 3U);
    auto newOperation = searchOperation(newCatalog, {}, 4U);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(oldCatalog, complete(oldOperation)));
    QPointer<LauncherResultPresentation> changed = model.resultAt(0);
    auto *unchanged = model.resultAt(1);

    ASSERT_TRUE(model.applySnapshot(newCatalog, complete(newOperation)));
    ASSERT_FALSE(changed.isNull());
    EXPECT_FALSE(changed->requestLaunch());
    ASSERT_NE(model.resultAt(0), nullptr);
    EXPECT_EQ(model.resultAt(0)->comment(), QStringLiteral("New comment"));
    EXPECT_EQ(model.resultAt(1), unchanged);

    std::optional<ApplicationLaunchIntent> captured;
    QObject::connect(&model, &LauncherPresentationModel::launchRequested, &model,
                     [&captured](const ApplicationLaunchIntent &intent) { captured = intent; });
    ASSERT_TRUE(model.resultAt(0)->requestLaunch());
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->desktopFileId.value(), "one.desktop");
    EXPECT_EQ(captured->catalogGeneration, 8U);
    EXPECT_EQ(captured->requestGeneration, 4U);
}

TEST(LauncherPresentationModelTest, RejectsAbsentIncompleteGenerationAndIndexCopies) {
    const auto source = catalog({discovered("one.desktop", "One")});
    auto operation = searchOperation(source, "one", 2U);
    const auto validSearch = complete(operation);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, validSearch));
    auto *retained = model.resultAt(0);

    EXPECT_FALSE(model.applySnapshot(nullptr, validSearch));
    EXPECT_FALSE(model.applySnapshot(source, nullptr));

    const auto incomplete = catalog({discovered("one.desktop", "One")}, 7U, false);
    EXPECT_FALSE(model.applySnapshot(incomplete, validSearch));

    auto wrongGeneration = std::make_shared<ApplicationSearchSnapshot>(*validSearch);
    wrongGeneration->catalogGeneration = 99U;
    EXPECT_FALSE(model.applySnapshot(source, std::move(wrongGeneration)));

    auto wrongIndex = std::make_shared<ApplicationSearchSnapshot>(*validSearch);
    wrongIndex->requestGeneration = 3U;
    wrongIndex->results = {{99U}};
    EXPECT_FALSE(model.applySnapshot(source, std::move(wrongIndex)));

    auto copiedCatalog = std::make_shared<ApplicationCatalogSnapshot>(*source);
    auto copiedSearch = std::make_shared<ApplicationSearchSnapshot>(*validSearch);
    const auto conflict = model.applySnapshot(std::move(copiedCatalog), std::move(copiedSearch));
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::validation_error);
    EXPECT_EQ(model.resultAt(0), retained);
    EXPECT_EQ(model.currentCatalog(), source);
    EXPECT_EQ(model.currentSearch(), validSearch);
}

TEST(LauncherPresentationModelTest, RejectsStaleAndCrossThreadPublicationWithRetention) {
    const auto source = catalog({discovered("one.desktop", "One")});
    auto firstOperation = searchOperation(source, "one", 1U);
    auto secondOperation = searchOperation(source, "one", 2U);
    const auto first = complete(firstOperation);
    const auto second = complete(secondOperation);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, second));
    auto *retained = model.resultAt(0);

    const auto stale = model.applySnapshot(source, first);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::cancelled);
    auto future = std::async(
        std::launch::async, [&model, source, first] { return model.applySnapshot(source, first); });
    const auto crossThread = future.get();
    ASSERT_FALSE(crossThread);
    EXPECT_EQ(crossThread.error().code, ErrorCode::cancelled);
    EXPECT_EQ(model.resultAt(0), retained);
    EXPECT_EQ(model.currentSearch(), second);
}

TEST(LauncherPresentationModelTest, RejectsReentrantPublicationAndIntentUntilCoherent) {
    const auto source = catalog({discovered("one.desktop", "One")});
    auto firstOperation = searchOperation(source, "one", 1U);
    auto secondOperation = searchOperation(source, "one", 2U);
    auto thirdOperation = searchOperation(source, "one", 3U);
    const auto first = complete(firstOperation);
    const auto second = complete(secondOperation);
    const auto third = complete(thirdOperation);
    LauncherPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(source, first));
    std::optional<foundation::Result<void>> reentrant;
    bool requestAccepted = true;
    int intentCount = 0;
    QObject::connect(&model, &LauncherPresentationModel::launchRequested, &model,
                     [&intentCount](const ApplicationLaunchIntent &) { ++intentCount; });
    QObject::connect(&model, &LauncherPresentationModel::publicationReconciliationStarted, &model,
                     [&] {
                         requestAccepted = model.resultAt(0)->requestLaunch();
                         reentrant.emplace(model.applySnapshot(source, third));
                     });
    bool appliedCoherently = false;
    QObject::connect(&model, &LauncherPresentationModel::publicationApplied, &model, [&] {
        appliedCoherently = !model.isApplyingSnapshot() && model.currentSearch() == second &&
                            model.resultAt(0)->name() == QStringLiteral("One");
    });

    ASSERT_TRUE(model.applySnapshot(source, second));
    ASSERT_TRUE(reentrant.has_value());
    EXPECT_FALSE(reentrant->hasValue());
    EXPECT_EQ(reentrant->error().code, ErrorCode::cancelled);
    EXPECT_FALSE(requestAccepted);
    EXPECT_EQ(intentCount, 0);
    EXPECT_TRUE(appliedCoherently);
    EXPECT_EQ(model.currentSearch(), second);
}

} // namespace
} // namespace prismdrake::shell::launcher
