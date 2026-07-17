#include "ServiceWorker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <variant>

namespace prismdrake::settingsd {
namespace {

class WorkerTemporaryDirectory final {
  public:
    WorkerTemporaryDirectory() {
        static std::atomic_uint counter{0U};
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-worker-test-" + std::to_string(counter.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~WorkerTemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] settings::SettingsEngineOptions
workerOptions(const WorkerTemporaryDirectory &temporary) {
    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    return {{temporary.path() / "config/config.toml",
             temporary.path() / "state/last-known-valid-config.toml",
             source / "data/defaults/config.toml"},
            source / "themes",
            {},
            {}};
}

[[nodiscard]] std::optional<WorkerCompletion> waitForCompletion(ServiceWorker &worker) {
    pollfd descriptor{worker.notificationFileDescriptor(), POLLIN, 0};
    if (::poll(&descriptor, 1U, static_cast<int>(std::chrono::seconds{5}.count() * 1000)) <= 0) {
        return std::nullopt;
    }
    return worker.takeCompletion();
}

TEST(ServiceWorkerTest, SerializesOneJobAndPublishesItsImmutableSnapshot) {
    WorkerTemporaryDirectory temporary;
    auto engine = settings::SettingsEngine::start(workerOptions(temporary));
    ASSERT_TRUE(engine);
    auto worker = ServiceWorker::create(std::move(engine).value());
    ASSERT_TRUE(worker);
    EXPECT_EQ(worker.value()->currentSnapshot()->generation.value(), 1U);

    EXPECT_TRUE(worker.value()->trySubmit(WorkerJob{1U, ProfileChangeJob{"forge"}}));
    EXPECT_FALSE(worker.value()->trySubmit(WorkerJob{2U, ReloadJob{}}));

    auto completion = waitForCompletion(*worker.value());
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->requestId, 1U);
    const auto *publication = std::get_if<PublicationResult>(&completion->result);
    ASSERT_NE(publication, nullptr);
    ASSERT_TRUE(*publication);
    EXPECT_EQ(publication->value().snapshot->generation.value(), 2U);
    EXPECT_EQ(worker.value()->currentSnapshot()->generation.value(), 2U);

    EXPECT_TRUE(worker.value()->trySubmit(
        WorkerJob{2U, CandidateValidationJob{"schema_version = 1\nunknown = true\n"}}));
    completion = waitForCompletion(*worker.value());
    ASSERT_TRUE(completion);
    const auto *validation = std::get_if<CandidateValidationResult>(&completion->result);
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(*validation);
    EXPECT_FALSE(validation->value().valid);
    EXPECT_EQ(worker.value()->currentSnapshot()->generation.value(), 2U);
}

TEST(ServiceWorkerTest, RejectsNewWorkAfterStop) {
    WorkerTemporaryDirectory temporary;
    auto engine = settings::SettingsEngine::start(workerOptions(temporary));
    ASSERT_TRUE(engine);
    auto worker = ServiceWorker::create(std::move(engine).value());
    ASSERT_TRUE(worker);

    worker.value()->stop();
    EXPECT_FALSE(worker.value()->trySubmit(WorkerJob{1U, ReloadJob{}}));
}

} // namespace
} // namespace prismdrake::settingsd
