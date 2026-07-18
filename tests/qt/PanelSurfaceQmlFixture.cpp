#include "PanelSurfaceQmlFixture.hpp"

#include "EwmhTaskList.hpp"
#include "WindowMetadata.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <atomic>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace prismdrake::shell::panel::test {
namespace {

[[nodiscard]] prismdrake::x11::WindowId window(std::uint32_t value) {
    return prismdrake::x11::WindowId::fromProtocol(value).value();
}

[[nodiscard]] prismdrake::x11::WindowIncarnationId incarnation(std::uint64_t value) {
    return prismdrake::x11::WindowIncarnationId::fromObserved(value).value();
}

[[nodiscard]] bool writeTextFile(const std::filesystem::path &destination,
                                 std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output{destination, std::ios::binary | std::ios::trunc};
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

[[nodiscard]] std::optional<std::string> readTextFile(const std::filesystem::path &source) {
    std::ifstream input{source, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return contents.str();
}

[[nodiscard]] bool replaceOnce(std::string &document, std::string_view before,
                               std::string_view after) {
    const auto position = document.find(before);
    if (position == std::string::npos ||
        document.find(before, position + before.size()) != std::string::npos) {
        return false;
    }
    document.replace(position, before.size(), after);
    return true;
}

} // namespace

PanelSurfaceQmlFixture::PanelSurfaceQmlFixture(QObject *parent) : QObject(parent) {
    static_cast<void>(resetLustre());
}

PanelSurfaceQmlFixture::~PanelSurfaceQmlFixture() {
    task_presentation_.reset();
    task_source_.reset();
    theme_adapter_.reset();
    settings_.reset();
    removeTemporaryDirectory();
}

QObject *PanelSurfaceQmlFixture::themeGeneration() const noexcept {
    return theme_adapter_ ? theme_adapter_->current() : nullptr;
}

QAbstractItemModel *PanelSurfaceQmlFixture::taskModel() const noexcept {
    return task_presentation_.get();
}

QString PanelSurfaceQmlFixture::taskGeneration() const {
    const auto snapshot = task_presentation_ ? task_presentation_->currentSnapshot() : nullptr;
    return snapshot ? QString::number(snapshot->generation().value()) : QString{};
}

bool PanelSurfaceQmlFixture::resetLustre() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/lustre.toml");
}

bool PanelSurfaceQmlFixture::resetLustreMissingBlur() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                      "examples/config/lustre.toml",
                                  {{false, true}, false});
}

bool PanelSurfaceQmlFixture::resetAccessible() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/accessible.toml");
}

bool PanelSurfaceQmlFixture::resetReducedMotion() {
    const auto source =
        std::filesystem::path(PRISMDRAKE_SOURCE_DIR) / "examples/config/lustre.toml";
    auto document = readTextFile(source);
    if (!document || !replaceOnce(*document, "reduced_motion = false", "reduced_motion = true") ||
        !replaceOnce(*document, "animation_scale = 1.0", "animation_scale = 0.0")) {
        return false;
    }
    return resetFromConfigurationText(*document);
}

bool PanelSurfaceQmlFixture::resetTransparencyDisabled() {
    const auto source =
        std::filesystem::path(PRISMDRAKE_SOURCE_DIR) / "examples/config/lustre.toml";
    auto document = readTextFile(source);
    if (!document ||
        !replaceOnce(*document, "transparency_enabled = true", "transparency_enabled = false") ||
        !replaceOnce(*document, "blur_quality = \"balanced\"", "blur_quality = \"off\"")) {
        return false;
    }
    return resetFromConfigurationText(*document);
}

bool PanelSurfaceQmlFixture::resetFromConfiguration(
    const std::filesystem::path &configuration,
    prismdrake::theme::ThemeResolveOptions capabilities) {
    const auto document = readTextFile(configuration);
    return document && resetFromConfigurationText(*document, capabilities);
}

bool PanelSurfaceQmlFixture::resetFromConfigurationText(
    std::string_view configuration, prismdrake::theme::ThemeResolveOptions capabilities) {
    static std::atomic_uint sequence{0U};
    const auto nextTemporaryDirectory =
        std::filesystem::temp_directory_path() /
        ("prismdrake-panel-qml-test-" + std::to_string(sequence.fetch_add(1U)));
    const auto userConfiguration = nextTemporaryDirectory / "config/config.toml";
    if (!writeTextFile(userConfiguration, configuration)) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    const config::ConfigurationLocations locations{
        userConfiguration, nextTemporaryDirectory / "state/last-known-valid-config.toml",
        source / "data/defaults/config.toml"};
    auto started = prismdrake::settings::SettingsEngine::start(
        {locations, source / "themes", {}, capabilities});
    if (!started) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    auto nextSettings = std::move(started).value();
    auto nextTheme = std::make_unique<prismdrake::shell::theme::ShellThemeSnapshotAdapter>();
    if (!nextTheme->applySnapshot(nextSettings->current())) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }
    auto nextTaskSource = std::make_unique<prismdrake::x11::TaskModel>();
    auto nextTaskPresentation = std::make_unique<prismdrake::shell::tasks::TaskPresentationModel>();

    connect(nextTheme.get(), &prismdrake::shell::theme::ShellThemeSnapshotAdapter::currentChanged,
            this, &PanelSurfaceQmlFixture::themeGenerationChanged);
    connect(nextTaskPresentation.get(),
            &prismdrake::shell::tasks::TaskPresentationModel::activationRequested, this,
            &PanelSurfaceQmlFixture::captureActivation);
    connect(nextTaskPresentation.get(),
            &prismdrake::shell::tasks::TaskPresentationModel::minimizationRequested, this,
            &PanelSurfaceQmlFixture::captureMinimization);
    connect(nextTaskPresentation.get(),
            &prismdrake::shell::tasks::TaskPresentationModel::closeRequested, this,
            &PanelSurfaceQmlFixture::captureClose);

    const auto previousTemporaryDirectory = std::move(temporary_directory_);
    auto previousSettings = std::move(settings_);
    auto previousTheme = std::move(theme_adapter_);
    auto previousTaskSource = std::move(task_source_);
    auto previousTaskPresentation = std::move(task_presentation_);

    temporary_directory_ = nextTemporaryDirectory;
    settings_ = std::move(nextSettings);
    theme_adapter_ = std::move(nextTheme);
    task_source_ = std::move(nextTaskSource);
    task_presentation_ = std::move(nextTaskPresentation);
    tasks_.clear();
    activation_count_ = 0;
    last_activation_title_.clear();
    last_activation_generation_.clear();
    minimization_count_ = 0;
    last_minimization_title_.clear();
    last_minimization_generation_.clear();
    close_count_ = 0;
    last_close_title_.clear();
    last_close_generation_.clear();
    emit themeGenerationChanged();
    emit taskModelChanged();
    emit activationCaptured();
    emit minimizationCaptured();
    emit closeCaptured();

    previousTaskPresentation.reset();
    previousTaskSource.reset();
    previousTheme.reset();
    previousSettings.reset();
    if (!previousTemporaryDirectory.empty()) {
        std::error_code error;
        std::filesystem::remove_all(previousTemporaryDirectory, error);
    }
    return true;
}

bool PanelSurfaceQmlFixture::publishForge() {
    if (!settings_ || !theme_adapter_) {
        return false;
    }
    auto publication = settings_->requestProfileChange("forge");
    return publication && theme_adapter_->applySnapshot(publication.value().snapshot).hasValue();
}

bool PanelSurfaceQmlFixture::publishRepresentativeTasks() {
    tasks_ = {{101U, 1001U, "Editor", "org.prismdrake.Editor.desktop", "application-x-executable"},
              {202U, 2002U, "Terminal", "org.prismdrake.Terminal.desktop", "utilities-terminal",
               false, false, false},
              {303U, 3003U, "Urgent dialog", "org.prismdrake.Dialog.desktop", "dialog-warning",
               false, true, true}};
    return publishTasks();
}

bool PanelSurfaceQmlFixture::swapFirstTwoTasks() {
    if (tasks_.size() < 2U) {
        return false;
    }
    std::swap(tasks_[0], tasks_[1]);
    return publishTasks();
}

bool PanelSurfaceQmlFixture::removeTask(int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= tasks_.size()) {
        return false;
    }
    tasks_.erase(tasks_.begin() + row);
    return publishTasks();
}

bool PanelSurfaceQmlFixture::setTaskMinimized(int row, bool minimized) {
    if (row < 0 || static_cast<std::size_t>(row) >= tasks_.size()) {
        return false;
    }
    tasks_[static_cast<std::size_t>(row)].minimized = minimized;
    return publishTasks();
}

bool PanelSurfaceQmlFixture::publishTasks() {
    if (!task_source_ || !task_presentation_) {
        return false;
    }

    std::vector<prismdrake::x11::DecodedTaskObservation> observations;
    std::vector<std::uint32_t> order;
    observations.reserve(tasks_.size());
    order.reserve(tasks_.size());
    for (const auto &task : tasks_) {
        const auto identifier = window(task.window);
        prismdrake::x11::WindowMetadata metadata{
            identifier,
            task.title,
            {prismdrake::x11::ApplicationIdentitySource::wmClass, std::nullopt, "Fixture",
             task.applicationId},
            prismdrake::x11::WindowType::normal,
            true,
            {},
            0U,
            false,
            task.minimized,
            task.urgent,
            false,
            task.modal,
            std::nullopt,
            {}};
        observations.push_back(
            {identifier, incarnation(task.incarnation), std::move(metadata), task.icon});
        order.push_back(task.window);
    }

    const std::optional<std::uint32_t> active =
        tasks_.empty() ? std::nullopt : std::optional<std::uint32_t>{tasks_.front().window};
    auto authoritative = prismdrake::x11::buildEwmhTaskListSnapshot(
        prismdrake::x11::EwmhTaskListObservation{order, std::nullopt, active});
    if (!authoritative) {
        return false;
    }
    auto snapshot = task_source_->publish(prismdrake::x11::TaskModelObservation{
        std::move(authoritative).value(), std::move(observations)});
    if (!snapshot || !task_presentation_->applySnapshot(std::move(snapshot).value())) {
        return false;
    }
    emit taskModelChanged();
    return true;
}

void PanelSurfaceQmlFixture::captureActivation(prismdrake::x11::TaskLifetimeId lifetime,
                                               prismdrake::x11::TaskModelGeneration generation) {
    const auto snapshot = task_presentation_ ? task_presentation_->currentSnapshot() : nullptr;
    if (!snapshot || snapshot->generation() != generation) {
        return;
    }
    for (const auto &task : snapshot->tasks()) {
        if (task.lifetime() == lifetime) {
            ++activation_count_;
            last_activation_title_ = QString::fromUtf8(task.title());
            last_activation_generation_ = QString::number(generation.value());
            emit activationCaptured();
            return;
        }
    }
}

void PanelSurfaceQmlFixture::captureMinimization(prismdrake::x11::TaskLifetimeId lifetime,
                                                 prismdrake::x11::TaskModelGeneration generation) {
    const auto snapshot = task_presentation_ ? task_presentation_->currentSnapshot() : nullptr;
    if (!snapshot || snapshot->generation() != generation) {
        return;
    }
    for (const auto &task : snapshot->tasks()) {
        if (task.lifetime() == lifetime) {
            ++minimization_count_;
            last_minimization_title_ = QString::fromUtf8(task.title());
            last_minimization_generation_ = QString::number(generation.value());
            emit minimizationCaptured();
            return;
        }
    }
}

void PanelSurfaceQmlFixture::captureClose(prismdrake::x11::TaskLifetimeId lifetime,
                                          prismdrake::x11::TaskModelGeneration generation) {
    const auto snapshot = task_presentation_ ? task_presentation_->currentSnapshot() : nullptr;
    if (!snapshot || snapshot->generation() != generation) {
        return;
    }
    for (const auto &task : snapshot->tasks()) {
        if (task.lifetime() == lifetime) {
            ++close_count_;
            last_close_title_ = QString::fromUtf8(task.title());
            last_close_generation_ = QString::number(generation.value());
            emit closeCaptured();
            return;
        }
    }
}

void PanelSurfaceQmlFixture::removeTemporaryDirectory() {
    if (temporary_directory_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(temporary_directory_, error);
    temporary_directory_.clear();
}

void PanelSurfaceQmlSetup::qmlEngineAvailable(QQmlEngine *engine) {
    engine->rootContext()->setContextProperty(QStringLiteral("panelFixture"),
                                              new PanelSurfaceQmlFixture(engine));
}

} // namespace prismdrake::shell::panel::test
