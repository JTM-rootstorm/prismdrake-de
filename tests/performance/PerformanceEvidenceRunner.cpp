#include "ApplicationCatalog.hpp"
#include "ApplicationSearch.hpp"
#include "BuildInfo.hpp"
#include "DesktopEntryDiscovery.hpp"
#include "DesktopEntryVisibility.hpp"
#include "DesktopFileId.hpp"
#include "SettingsEngine.hpp"
#include "TaskModel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace foundation = prismdrake::foundation;
namespace launcher = prismdrake::launcher;
namespace settings = prismdrake::settings;
namespace x11 = prismdrake::x11;

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr std::size_t minimumIterations = 3U;
constexpr std::size_t maximumIterations = 200U;
constexpr std::size_t defaultIterations = 25U;
constexpr std::size_t discoveryWorkBudget = 64U;
constexpr std::size_t maximumDiscoveryPulls = 4096U;
constexpr std::array<std::size_t, 3U> controlledFixtureSizes{16U, 64U, 256U};

struct Options final {
    std::string sourceRevision;
    std::string environmentId;
    std::size_t iterations{defaultIterations};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-pd1-performance-" + std::to_string(::getpid()));
        std::error_code error;
        if (!std::filesystem::create_directory(path_, error) || error) {
            throw std::runtime_error{"temporary_directory_creation_failed"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[noreturn]] void fail(std::string_view errorId) { throw std::runtime_error{std::string{errorId}}; }

[[nodiscard]] bool validRevision(std::string_view value) {
    const bool validLength = value.size() == 40U || value.size() == 64U;
    return validLength && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool validEnvironmentId(std::string_view value) {
    if (value.empty() || value.size() > 64U ||
        !((value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= '0' && value.front() <= '9'))) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '.' || character == '_' || character == '-';
    });
}

[[nodiscard]] std::optional<std::size_t> parseCount(std::string_view value) {
    std::size_t parsed = 0U;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] Options parseOptions(int argc, char **argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            std::cout << "Usage: prismdrake-pd1-performance-evidence --revision <40-or-64-hex> "
                         "--environment-id <redacted-id> [--iterations <3-200>]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            fail("missing_option_value");
        }
        const std::string_view value{argv[++index]};
        if (argument == "--revision") {
            if (!options.sourceRevision.empty() || !validRevision(value)) {
                fail("invalid_source_revision");
            }
            options.sourceRevision = value;
        } else if (argument == "--environment-id") {
            if (!options.environmentId.empty() || !validEnvironmentId(value)) {
                fail("invalid_environment_id");
            }
            options.environmentId = value;
        } else if (argument == "--iterations") {
            const auto parsed = parseCount(value);
            if (!parsed || *parsed < minimumIterations || *parsed > maximumIterations) {
                fail("invalid_iteration_count");
            }
            options.iterations = *parsed;
        } else {
            fail("unknown_option");
        }
    }
    if (options.sourceRevision.empty() || options.environmentId.empty()) {
        fail("required_option_missing");
    }
    return options;
}

template <typename Operation> [[nodiscard]] std::uint64_t measure(Operation &&operation) {
    const auto started = Clock::now();
    operation();
    const auto finished = Clock::now();
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(finished - started).count();
    if (elapsed < 0) {
        fail("non_monotonic_measurement");
    }
    return static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] nlohmann::json summarize(std::vector<std::uint64_t> samples) {
    if (samples.empty()) {
        fail("empty_measurement_series");
    }
    const auto raw = samples;
    std::ranges::sort(samples);
    const auto percentileIndex = [size = samples.size()](std::size_t percentile) {
        return ((size - 1U) * percentile + 99U) / 100U;
    };
    return {{"sample_count", samples.size()},
            {"minimum_ns", samples.front()},
            {"median_ns", samples[(samples.size() - 1U) / 2U]},
            {"p95_ns", samples[percentileIndex(95U)]},
            {"maximum_ns", samples.back()},
            {"samples_ns", raw}};
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        fail("fixture_write_failed");
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) {
        fail("fixture_write_failed");
    }
}

[[nodiscard]] settings::SettingsEngineOptions settingsOptions(const TemporaryDirectory &temporary) {
    const auto source = std::filesystem::path{PRISMDRAKE_PERFORMANCE_SOURCE_DIR};
    return {{temporary.path() / "config/config.toml",
             temporary.path() / "state/last-known-valid-config.toml",
             source / "data/defaults/config.toml"},
            source / "themes",
            {},
            {}};
}

[[nodiscard]] std::unique_ptr<settings::SettingsEngine>
startSettings(const TemporaryDirectory &temporary) {
    auto started = settings::SettingsEngine::start(settingsOptions(temporary));
    if (!started || !started.value()->current() ||
        started.value()->current()->generation.value() != 1U) {
        fail("settings_initial_load_failed");
    }
    return std::move(started).value();
}

[[nodiscard]] nlohmann::json measureSettingsInitialLoad(const TemporaryDirectory &temporary,
                                                        std::size_t iterations) {
    static_cast<void>(startSettings(temporary));
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        samples.push_back(measure([&temporary] { static_cast<void>(startSettings(temporary)); }));
    }
    return {{"name", "settings_initial_load"},
            {"scope", "packaged_config_and_theme_to_first_immutable_generation"},
            {"fixture_count", nullptr},
            {"statistics", summarize(std::move(samples))}};
}

[[nodiscard]] nlohmann::json measureProfilePublication(const TemporaryDirectory &temporary,
                                                       std::size_t iterations) {
    auto engine = startSettings(temporary);
    auto warmup = engine->requestProfileChange("forge");
    if (!warmup || !warmup.value().published) {
        fail("profile_publication_failed");
    }
    auto reset = engine->requestProfileChange("lustre");
    if (!reset || !reset.value().published) {
        fail("profile_publication_failed");
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const std::string_view profile = iteration % 2U == 0U ? "forge" : "lustre";
        samples.push_back(measure([&engine, profile] {
            auto publication = engine->requestProfileChange(profile);
            if (!publication || !publication.value().published) {
                fail("profile_publication_failed");
            }
        }));
    }
    return {{"name", "profile_switch_publication"},
            {"scope", "validated_profile_request_to_immutable_in_process_publication"},
            {"fixture_count", nullptr},
            {"statistics", summarize(std::move(samples))}};
}

[[nodiscard]] launcher::CurrentDesktopContext currentDesktop() {
    auto parsed = launcher::parseCurrentDesktopContext("Prismdrake");
    if (!parsed) {
        fail("desktop_context_creation_failed");
    }
    return std::move(parsed).value();
}

[[nodiscard]] std::shared_ptr<const launcher::DesktopEntryDiscoverySnapshot>
discover(const std::filesystem::path &root) {
    auto created = launcher::createDesktopEntryDiscovery(launcher::ApplicationPaths{{root}}, {"C"},
                                                         currentDesktop());
    if (!created) {
        fail("desktop_discovery_creation_failed");
    }
    auto discovery = std::move(created).value();
    foundation::CancellationSource cancellation;
    for (std::size_t pull = 0U; pull < maximumDiscoveryPulls; ++pull) {
        auto batch = discovery.pull(discoveryWorkBudget, cancellation.token());
        if (!batch) {
            fail("desktop_discovery_failed");
        }
        if (batch.value().snapshot->complete) {
            return batch.value().snapshot;
        }
    }
    fail("desktop_discovery_pull_bound_exceeded");
}

void createDiscoveryFixture(const std::filesystem::path &root, std::size_t count) {
    std::error_code error;
    if (!std::filesystem::create_directory(root, error) || error) {
        fail("discovery_fixture_creation_failed");
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const auto document = "[Desktop Entry]\nType=Application\nName=Prismdrake Fixture " +
                              std::to_string(index) +
                              "\nGenericName=Controlled Application\nKeywords=dragon;fixture;\n"
                              "Categories=Utility;\nExec=fixture-tool\n";
        writeFile(root / ("fixture-" + std::to_string(index) + ".desktop"), document);
    }
}

[[nodiscard]] nlohmann::json measureDesktopDiscovery(const std::filesystem::path &root,
                                                     std::size_t fixtureCount,
                                                     std::size_t iterations) {
    const auto warmup = discover(root);
    if (warmup->entries.size() != fixtureCount ||
        warmup->visibleEntryIndices.size() != fixtureCount) {
        fail("desktop_discovery_fixture_mismatch");
    }
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        samples.push_back(measure([&root, fixtureCount] {
            const auto result = discover(root);
            if (result->entries.size() != fixtureCount ||
                result->visibleEntryIndices.size() != fixtureCount || result->truncated) {
                fail("desktop_discovery_fixture_mismatch");
            }
        }));
    }
    return {{"name", "desktop_entry_discovery"},
            {"scope", "scanner_creation_through_complete_bounded_fixture_publication"},
            {"fixture_count", fixtureCount},
            {"statistics", summarize(std::move(samples))}};
}

[[nodiscard]] launcher::DiscoveredDesktopEntry syntheticEntry(std::size_t index) {
    const auto relative = "fixture-" + std::to_string(index) + ".desktop";
    auto identifier = launcher::deriveDesktopFileId(relative);
    auto location = launcher::makeDiscoveredDesktopFileLocation("/controlled-fixture/applications",
                                                                relative, 0U);
    if (!identifier || !location) {
        fail("search_fixture_creation_failed");
    }
    launcher::DesktopEntry entry;
    entry.name = index % 4U == 0U ? "Dragon Editor " + std::to_string(index)
                                  : "Utility " + std::to_string(index);
    entry.genericName = "Controlled Application";
    entry.keywords = {"fixture", index % 4U == 0U ? "dragon" : "utility"};
    entry.categories = {"Utility"};
    entry.exec = "fixture-tool";
    return {std::move(identifier).value(), std::move(entry),
            launcher::DesktopEntryVisibilityReason::visibleByDefault, std::move(location).value()};
}

[[nodiscard]] std::shared_ptr<const launcher::ApplicationCatalogSnapshot>
syntheticCatalog(std::size_t count) {
    std::vector<launcher::DiscoveredDesktopEntry> entries;
    std::vector<std::size_t> visible;
    std::vector<launcher::ApplicationCatalogDecision> decisions;
    entries.reserve(count);
    visible.reserve(count);
    decisions.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        entries.push_back(syntheticEntry(index));
        visible.push_back(index);
        decisions.push_back(
            {index, launcher::ApplicationCatalogEligibilityReason::eligibleWithoutTryExec});
    }
    auto discovery = std::make_shared<const launcher::DesktopEntryDiscoverySnapshot>(
        launcher::DesktopEntryDiscoverySnapshot{
            std::move(entries), visible, {}, count, true, false, false});
    return std::make_shared<const launcher::ApplicationCatalogSnapshot>(
        launcher::ApplicationCatalogSnapshot{1U, std::move(discovery), std::move(decisions),
                                             std::move(visible), count, count, true});
}

void runSearch(const std::shared_ptr<const launcher::ApplicationCatalogSnapshot> &catalog,
               const launcher::ApplicationSearchQuery &query, std::uint64_t requestGeneration) {
    auto created = launcher::createApplicationSearch(catalog, requestGeneration, query);
    if (!created) {
        fail("search_creation_failed");
    }
    auto operation = std::move(created).value();
    foundation::CancellationSource cancellation;
    auto result =
        operation.advance(launcher::maximumApplicationSearchWorkUnits, cancellation.token());
    if (!result || result.value()->state != launcher::ApplicationSearchViewState::results ||
        result.value()->examinedApplications != catalog->eligibleEntryIndices.size()) {
        fail("search_publication_failed");
    }
}

[[nodiscard]] nlohmann::json measureSearch(std::size_t fixtureCount, std::size_t iterations) {
    const auto catalog = syntheticCatalog(fixtureCount);
    auto query = launcher::parseApplicationSearchQuery("dragon editor");
    if (!query) {
        fail("search_query_creation_failed");
    }
    runSearch(catalog, query.value(), 1U);
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        samples.push_back(measure(
            [&catalog, &query, iteration] { runSearch(catalog, query.value(), iteration + 2U); }));
    }
    return {{"name", "application_search_response"},
            {"scope", "validated_query_operation_to_complete_in_memory_result_publication"},
            {"fixture_count", fixtureCount},
            {"statistics", summarize(std::move(samples))}};
}

[[nodiscard]] x11::WindowId window(std::size_t index) {
    auto result = x11::WindowId::fromProtocol(static_cast<std::uint32_t>(index + 1U));
    if (!result) {
        fail("ewmh_fixture_creation_failed");
    }
    return result.value();
}

[[nodiscard]] x11::WindowIncarnationId incarnation(std::size_t index) {
    auto result = x11::WindowIncarnationId::fromObserved(index + 1U);
    if (!result) {
        fail("ewmh_fixture_creation_failed");
    }
    return result.value();
}

[[nodiscard]] x11::TaskModelObservation taskObservation(std::size_t count, bool updated) {
    std::vector<x11::WindowId::Value> protocolWindows;
    protocolWindows.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        protocolWindows.push_back(static_cast<x11::WindowId::Value>(index + 1U));
    }
    auto authoritative =
        x11::buildEwmhTaskListSnapshot({protocolWindows, std::nullopt, protocolWindows.back()});
    if (!authoritative) {
        fail("ewmh_fixture_creation_failed");
    }

    std::vector<x11::DecodedTaskObservation> windows;
    windows.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto id = window(index);
        x11::WindowMetadata metadata{
            id,
            updated ? "Updated Fixture" : "Baseline Fixture",
            {x11::ApplicationIdentitySource::wmClass, "fixture", "Fixture", "fixture"},
            x11::WindowType::normal,
            true,
            {},
            0U,
            false,
            false,
            updated && index == count - 1U,
            false,
            false,
            std::nullopt,
            {}};
        windows.push_back(
            {id, incarnation(index), std::move(metadata), "application-x-executable"});
    }
    return {std::move(authoritative).value(), std::move(windows)};
}

void publishTaskUpdate(const x11::TaskModelObservation &baseline,
                       const x11::TaskModelObservation &updated) {
    x11::TaskModel model;
    auto first = model.publish(baseline);
    if (!first || first.value()->tasks().size() != baseline.windows.size()) {
        fail("ewmh_baseline_publication_failed");
    }
    auto second = model.publish(updated);
    if (!second || second.value()->tasks().size() != updated.windows.size() ||
        second.value()->generation().value() != 2U) {
        fail("ewmh_update_publication_failed");
    }
}

[[nodiscard]] nlohmann::json measureEwmhUpdate(std::size_t fixtureCount, std::size_t iterations) {
    const auto baseline = taskObservation(fixtureCount, false);
    const auto updated = taskObservation(fixtureCount, true);
    publishTaskUpdate(baseline, updated);
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        x11::TaskModel model;
        auto first = model.publish(baseline);
        if (!first) {
            fail("ewmh_baseline_publication_failed");
        }
        samples.push_back(measure([&model, &updated] {
            auto publication = model.publish(updated);
            if (!publication || publication.value()->tasks().size() != updated.windows.size()) {
                fail("ewmh_update_publication_failed");
            }
        }));
    }
    return {{"name", "ewmh_task_model_update"},
            {"scope", "complete_decoded_observation_to_immutable_mirror_publication"},
            {"fixture_count", fixtureCount},
            {"statistics", summarize(std::move(samples))}};
}

[[nodiscard]] std::string buildType() {
    constexpr std::string_view configured{PRISMDRAKE_PERFORMANCE_BUILD_TYPE};
    return configured.empty() ? "unspecified" : std::string{configured};
}

[[nodiscard]] nlohmann::json collectEvidence(const Options &options) {
    TemporaryDirectory temporary;
    std::vector<nlohmann::json> measurements;
    measurements.reserve(2U + controlledFixtureSizes.size() * 3U);
    measurements.push_back(measureSettingsInitialLoad(temporary, options.iterations));
    measurements.push_back(measureProfilePublication(temporary, options.iterations));

    for (const auto size : controlledFixtureSizes) {
        const auto root = temporary.path() / ("discovery-" + std::to_string(size));
        createDiscoveryFixture(root, size);
        measurements.push_back(measureDesktopDiscovery(root, size, options.iterations));
        measurements.push_back(measureSearch(size, options.iterations));
        measurements.push_back(measureEwmhUpdate(size, options.iterations));
    }

    return {{"schema_version", 1},
            {"evidence_kind", "pd1_prototype_performance"},
            {"release_budget", false},
            {"source_revision", options.sourceRevision},
            {"reference_environment_id", options.environmentId},
            {"build",
             {{"product_version", foundation::productVersion()},
              {"architecture", PRISMDRAKE_PERFORMANCE_ARCHITECTURE},
              {"cmake_generator", PRISMDRAKE_PERFORMANCE_CMAKE_GENERATOR},
              {"cmake_build_type", buildType()},
              {"compiler_id", PRISMDRAKE_PERFORMANCE_COMPILER_ID},
              {"compiler_version", PRISMDRAKE_PERFORMANCE_COMPILER_VERSION},
              {"cxx_standard", 20},
              {"developer_overrides", foundation::developerOverridesEnabled()}}},
            {"method",
             {{"clock", "std_chrono_steady_clock"},
              {"clock_is_steady", Clock::is_steady},
              {"duration_unit", "nanoseconds"},
              {"warmup_iterations", 1},
              {"measured_iterations", options.iterations},
              {"filesystem_cache_state", "warm_after_untimed_warmup"},
              {"fixture_sizes", controlledFixtureSizes}}},
            {"measurements", std::move(measurements)},
            {"redaction",
             {{"contains_filesystem_paths", false},
              {"contains_host_or_user_names", false},
              {"contains_application_or_window_content", false},
              {"diagnostics_are_closed_ids", true}}}};
}

} // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parseOptions(argc, argv);
        std::cout << collectEvidence(options).dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "prismdrake-pd1-performance-evidence: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "prismdrake-pd1-performance-evidence: unknown_bounded_failure\n";
        return 2;
    }
}
