#include "LauncherSurfaceQmlFixture.hpp"

#include "ApplicationSearch.hpp"
#include "Cancellation.hpp"
#include "DesktopEntryDiscovery.hpp"
#include "DesktopEntryVisibility.hpp"
#include "DesktopFileId.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <algorithm>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace prismdrake::shell::launcher::test {
namespace {

using prismdrake::launcher::ApplicationCatalogDecision;
using prismdrake::launcher::ApplicationCatalogEligibilityReason;
using prismdrake::launcher::ApplicationCatalogSnapshot;
using prismdrake::launcher::DesktopEntry;
using prismdrake::launcher::DesktopEntryDiscoverySnapshot;
using prismdrake::launcher::DesktopEntryVisibilityReason;
using prismdrake::launcher::DiscoveredDesktopEntry;

[[nodiscard]] bool copyTextFile(const std::filesystem::path &source,
                                const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return false;
    }
    return std::filesystem::copy_file(source, destination,
                                      std::filesystem::copy_options::overwrite_existing, error) &&
           !error;
}

[[nodiscard]] std::optional<DiscoveredDesktopEntry> discovered(std::string id, std::string name,
                                                               std::string genericName,
                                                               std::string comment, bool terminal) {
    auto identifier = prismdrake::launcher::deriveDesktopFileId(id);
    auto location = prismdrake::launcher::makeDiscoveredDesktopFileLocation(
        "/fixture/private/applications", id, 0U);
    if (!identifier || !location) {
        return std::nullopt;
    }
    DesktopEntry entry;
    entry.name = std::move(name);
    if (!genericName.empty()) {
        entry.genericName = std::move(genericName);
    }
    if (!comment.empty()) {
        entry.comment = std::move(comment);
    }
    entry.icon = "/fixture/private/icon.png";
    entry.exec = "private-command --secret %U";
    entry.path = "/fixture/private/working-directory";
    entry.terminal = terminal;
    return DiscoveredDesktopEntry{std::move(identifier).value(), std::move(entry),
                                  DesktopEntryVisibilityReason::visibleByDefault,
                                  std::move(location).value()};
}

} // namespace

LauncherSurfaceQmlFixture::LauncherSurfaceQmlFixture(QObject *parent) : QObject(parent) {
    static_cast<void>(resetLustre());
}

LauncherSurfaceQmlFixture::~LauncherSurfaceQmlFixture() {
    launcher_presentation_.reset();
    theme_adapter_.reset();
    settings_.reset();
    removeTemporaryDirectory();
}

QObject *LauncherSurfaceQmlFixture::themeGeneration() const noexcept {
    return theme_adapter_ ? theme_adapter_->current() : nullptr;
}

QAbstractItemModel *LauncherSurfaceQmlFixture::launcherModel() const noexcept {
    return launcher_presentation_.get();
}

bool LauncherSurfaceQmlFixture::resetLustre() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/lustre.toml");
}

bool LauncherSurfaceQmlFixture::resetAccessible() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/accessible.toml");
}

bool LauncherSurfaceQmlFixture::resetFromConfiguration(const std::filesystem::path &configuration) {
    static std::atomic_uint sequence{0U};
    const auto nextTemporaryDirectory =
        std::filesystem::temp_directory_path() /
        ("prismdrake-launcher-qml-test-" + std::to_string(sequence.fetch_add(1U)));
    const auto userConfiguration = nextTemporaryDirectory / "config/config.toml";
    if (!copyTextFile(configuration, userConfiguration)) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    const config::ConfigurationLocations locations{
        userConfiguration, nextTemporaryDirectory / "state/last-known-valid-config.toml",
        source / "data/defaults/config.toml"};
    constexpr prismdrake::theme::ThemeResolveOptions capabilities{{true, true}, false};
    auto started = prismdrake::settings::SettingsEngine::start(
        {locations, source / "themes", {}, capabilities});
    if (!started) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    auto nextSettings = std::move(started).value();
    auto nextTheme = std::make_unique<prismdrake::shell::theme::ShellThemeSnapshotAdapter>();
    auto nextPresentation = std::make_unique<LauncherPresentationModel>();
    if (!nextTheme->applySnapshot(nextSettings->current())) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }
    connect(nextTheme.get(), &prismdrake::shell::theme::ShellThemeSnapshotAdapter::currentChanged,
            this, &LauncherSurfaceQmlFixture::themeGenerationChanged);
    connect(nextPresentation.get(), &LauncherPresentationModel::launchRequested, this,
            &LauncherSurfaceQmlFixture::captureLaunch);

    const auto previousTemporaryDirectory = std::move(temporary_directory_);
    auto previousSettings = std::move(settings_);
    auto previousTheme = std::move(theme_adapter_);
    auto previousPresentation = std::move(launcher_presentation_);

    temporary_directory_ = nextTemporaryDirectory;
    settings_ = std::move(nextSettings);
    theme_adapter_ = std::move(nextTheme);
    launcher_presentation_ = std::move(nextPresentation);
    catalog_.reset();
    applications_.clear();
    catalog_generation_ = 6U;
    request_generation_ = 0U;
    launch_count_ = 0;
    last_launched_name_.clear();
    captured_search_.clear();
    emit themeGenerationChanged();
    emit launcherModelChanged();
    emit launchCaptured();
    emit searchCaptured();

    previousPresentation.reset();
    previousTheme.reset();
    previousSettings.reset();
    if (!previousTemporaryDirectory.empty()) {
        std::error_code error;
        std::filesystem::remove_all(previousTemporaryDirectory, error);
    }
    return true;
}

bool LauncherSurfaceQmlFixture::publishForge() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/forge.toml");
}

bool LauncherSurfaceQmlFixture::publishRepresentativeResults() {
    applications_ = {
        {"dragon-editor.desktop", "Dragon Editor", "Text editor", "Edit local plain-text documents",
         false},
        {"editor-dragon.desktop", "Editor Dragon", "Development tool", "Edit source code", false},
        {"terminal.desktop", "Terminal", "Command line", "Runs in a terminal", true}};
    return rebuildCatalog() &&
           publishSearch({}, prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::publishLongResult() {
    applications_ = {{"long.desktop", std::string(2048U, 'L'), "Literal <b>utility</b>",
                      "Untrusted <script>plain text</script> & metadata", true}};
    return rebuildCatalog() &&
           publishSearch({}, prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::publishLoading() {
    applications_ = {{"alpha.desktop", "Alpha", {}, {}, false},
                     {"beta.desktop", "Beta", {}, {}, false},
                     {"gamma.desktop", "Gamma", {}, {}, false}};
    return rebuildCatalog() && publishSearch({}, 1U);
}

bool LauncherSurfaceQmlFixture::publishNoResults() {
    if (!catalog_ && !publishRepresentativeResults()) {
        return false;
    }
    return publishSearch("definitely-absent",
                         prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::publishEmptyCatalog() {
    applications_.clear();
    return rebuildCatalog() &&
           publishSearch({}, prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::publishError() {
    if (!catalog_ && !publishRepresentativeResults()) {
        return false;
    }
    auto error = prismdrake::launcher::makeApplicationSearchErrorSnapshot(catalog_->generation,
                                                                          ++request_generation_);
    return error && launcher_presentation_->applySnapshot(catalog_, error.value()).hasValue();
}

bool LauncherSurfaceQmlFixture::publishReorderedResults() {
    return catalog_ &&
           publishSearch("editor", prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::removeResult(int row) {
    if (!catalog_ || !launcher_presentation_ || row < 0 ||
        static_cast<std::size_t>(row) >= launcher_presentation_->currentSearch()->results.size()) {
        return false;
    }
    const auto discoveryIndex = launcher_presentation_->currentSearch()
                                    ->results[static_cast<std::size_t>(row)]
                                    .discoveryEntryIndex;
    const auto id = catalog_->discovery->entries[discoveryIndex].id.value();
    const auto application = std::ranges::find_if(
        applications_, [&id](const FixtureApplication &candidate) { return candidate.id == id; });
    if (application == applications_.end()) {
        return false;
    }
    applications_.erase(application);
    return rebuildCatalog() &&
           publishSearch({}, prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::rejectInvalidPublication() {
    if (!catalog_ || !launcher_presentation_ || !launcher_presentation_->currentSearch()) {
        return false;
    }
    const auto retained = launcher_presentation_->currentSearch();
    auto invalid = std::make_shared<prismdrake::launcher::ApplicationSearchSnapshot>(*retained);
    invalid->requestGeneration = ++request_generation_;
    invalid->results = {{catalog_->discovery->entries.size() + 10U}};
    const auto rejected = launcher_presentation_->applySnapshot(catalog_, std::move(invalid));
    return !rejected && launcher_presentation_->currentSearch() == retained;
}

void LauncherSurfaceQmlFixture::captureSearch(const QString &query) {
    captured_search_ = query;
    ++request_generation_;
    emit searchCaptured();
}

bool LauncherSurfaceQmlFixture::publishCapturedSearch() {
    return publishSearchAtGeneration(captured_search_.toStdString(), request_generation_,
                                     prismdrake::launcher::maximumApplicationSearchWorkUnits);
}

bool LauncherSurfaceQmlFixture::rebuildCatalog() {
    std::vector<DiscoveredDesktopEntry> entries;
    std::vector<std::size_t> visible;
    std::vector<ApplicationCatalogDecision> decisions;
    entries.reserve(applications_.size());
    visible.reserve(applications_.size());
    decisions.reserve(applications_.size());
    for (std::size_t index = 0U; index < applications_.size(); ++index) {
        auto entry = discovered(applications_[index].id, applications_[index].name,
                                applications_[index].genericName, applications_[index].comment,
                                applications_[index].terminal);
        if (!entry) {
            return false;
        }
        entries.push_back(std::move(entry).value());
        visible.push_back(index);
        decisions.push_back({index, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec});
    }
    auto discovery =
        std::make_shared<const DesktopEntryDiscoverySnapshot>(DesktopEntryDiscoverySnapshot{
            std::move(entries), visible, {}, applications_.size(), true, false, false});
    catalog_ = std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
        ++catalog_generation_, discovery, std::move(decisions), std::move(visible),
        discovery->visibleEntryIndices.size(), discovery->visibleEntryIndices.size(), true});
    return true;
}

bool LauncherSurfaceQmlFixture::publishSearch(std::string_view query, std::size_t workUnits) {
    return publishSearchAtGeneration(query, ++request_generation_, workUnits);
}

bool LauncherSurfaceQmlFixture::publishSearchAtGeneration(std::string_view query,
                                                          std::uint64_t requestGeneration,
                                                          std::size_t workUnits) {
    if (!catalog_ || !launcher_presentation_) {
        return false;
    }
    auto parsed = prismdrake::launcher::parseApplicationSearchQuery(query);
    if (!parsed) {
        return false;
    }
    auto operation = prismdrake::launcher::createApplicationSearch(catalog_, requestGeneration,
                                                                   std::move(parsed).value());
    if (!operation) {
        return false;
    }
    prismdrake::foundation::CancellationSource cancellation;
    auto search = operation.value().advance(workUnits, cancellation.token());
    return search &&
           launcher_presentation_->applySnapshot(catalog_, std::move(search).value()).hasValue();
}

void LauncherSurfaceQmlFixture::captureLaunch(const ApplicationLaunchIntent &intent) {
    if (!catalog_ || !launcher_presentation_ || !launcher_presentation_->currentSearch() ||
        intent.catalogGeneration != catalog_->generation ||
        intent.requestGeneration != request_generation_) {
        return;
    }
    for (const auto &application : catalog_->discovery->entries) {
        if (application.id == intent.desktopFileId) {
            ++launch_count_;
            last_launched_name_ = QString::fromUtf8(application.entry.name.value_or(""));
            emit launchCaptured();
            return;
        }
    }
}

void LauncherSurfaceQmlFixture::removeTemporaryDirectory() {
    if (temporary_directory_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(temporary_directory_, error);
    temporary_directory_.clear();
}

void LauncherSurfaceQmlSetup::qmlEngineAvailable(QQmlEngine *engine) {
    engine->rootContext()->setContextProperty(QStringLiteral("launcherFixture"),
                                              new LauncherSurfaceQmlFixture(engine));
}

} // namespace prismdrake::shell::launcher::test
