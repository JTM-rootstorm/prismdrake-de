#include "SettingsSnapshotClient.hpp"

#include "RuntimeSnapshot.hpp"
#include "SettingsEngine.hpp"
#include "ShellThemeSnapshotAdapter.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::settings {
namespace {

constexpr char serviceName[] = "org.prismdrake.Settings1";
constexpr char objectPath[] = "/org/prismdrake/Settings1";
constexpr char snapshotInterface[] = "org.prismdrake.SettingsSnapshot1";
constexpr char settingsInterface[] = "org.prismdrake.Settings1";

struct Response final {
    std::uint64_t generation;
    std::string bytes;
};

struct FakeSettingsServiceOptions final {
    std::uint64_t nameFlags{0U};
    bool holdSnapshotReply{false};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint counter{0U};
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-settings-client-test-" + std::to_string(counter.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string readFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("unable to read settings client fixture");
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("unable to write settings client fixture");
    }
}

[[nodiscard]] std::vector<Response> realProfileSnapshots() {
    TemporaryDirectory temporary;
    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    const config::ConfigurationLocations locations{temporary.path() / "config/config.toml",
                                                   temporary.path() /
                                                       "state/last-known-valid-config.toml",
                                                   source / "data/defaults/config.toml"};
    writeFile(locations.user, readFile(source / "examples/config/lustre.toml"));
    auto engine = prismdrake::settings::SettingsEngine::start(
        {locations, source / "themes", {}, {{true, true}, false}});
    if (!engine) {
        throw std::runtime_error(engine.error().message);
    }
    const auto first = engine.value()->current();
    auto changed = engine.value()->requestProfileChange("forge");
    if (!changed) {
        throw std::runtime_error(changed.error().message);
    }
    const auto second = changed.value().snapshot;
    return {{first->generation.value(), first->serializedJson},
            {second->generation.value(), second->serializedJson}};
}

class FakeSettingsService final {
  public:
    explicit FakeSettingsService(std::vector<Response> responses,
                                 FakeSettingsServiceOptions options = {})
        : responses_(std::move(responses)), options_(options) {
        if (responses_.empty()) {
            throw std::invalid_argument("fake settings service requires a response");
        }
        std::promise<bool> ready;
        auto readyResult = ready.get_future();
        thread_ =
            std::thread([this, signal = std::move(ready)]() mutable { run(std::move(signal)); });
        if (!readyResult.get()) {
            running_.store(false);
            thread_.join();
            throw std::runtime_error("unable to start fake settings service");
        }
    }

    ~FakeSettingsService() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FakeSettingsService(const FakeSettingsService &) = delete;
    FakeSettingsService &operator=(const FakeSettingsService &) = delete;

    void advanceAndSignal() noexcept { advance_.store(true); }
    void emitInvalidSignal() noexcept { invalid_signal_.store(true); }
    void releaseHeldSnapshotReply() noexcept { release_held_reply_.store(true); }
    [[nodiscard]] bool hasHeldSnapshotReply() const noexcept { return held_reply_ready_.load(); }

  private:
    [[nodiscard]] static const sd_bus_vtable *snapshotVtable() {
        static const sd_bus_vtable vtable[] = {SD_BUS_VTABLE_START(0),
                                               SD_BUS_METHOD("GetCurrentSnapshot", "u", "tay",
                                                             getCurrentSnapshot,
                                                             SD_BUS_VTABLE_UNPRIVILEGED),
                                               SD_BUS_VTABLE_END};
        return vtable;
    }

    static int getCurrentSnapshot(sd_bus_message *message, void *userdata,
                                  sd_bus_error *error) noexcept {
        auto &self = *static_cast<FakeSettingsService *>(userdata);
        std::uint32_t version = 0U;
        if (sd_bus_message_read(message, "u", &version) <= 0 || version != 1U ||
            sd_bus_message_at_end(message, 1) <= 0) {
            return sd_bus_error_set_const(error, "org.prismdrake.Settings1.Error.Unsupported",
                                          "Unsupported snapshot version.");
        }
        if (self.options_.holdSnapshotReply && !self.held_reply_) {
            self.held_reply_.reset(sd_bus_message_ref(message));
            if (!self.held_reply_) {
                return -ENOMEM;
            }
            self.held_reply_ready_.store(true);
            return 1;
        }
        return self.sendSnapshotReply(message);
    }

    int sendSnapshotReply(sd_bus_message *message) noexcept {
        const auto &response = responses_.at(index_);
        ipc::sdbus::Message reply;
        if (sd_bus_message_new_method_return(message, reply.put()) < 0 ||
            sd_bus_message_append(reply.get(), "t", response.generation) < 0 ||
            sd_bus_message_append_array(reply.get(), SD_BUS_TYPE_BYTE, response.bytes.data(),
                                        response.bytes.size()) < 0) {
            return -ENOMEM;
        }
        return sd_bus_send(bus_, reply.get(), nullptr);
    }

    void emitGenerationChanged(bool invalid = false) noexcept {
        ipc::sdbus::Message signal;
        if (sd_bus_message_new_signal(bus_, signal.put(), objectPath, settingsInterface,
                                      "SettingsGenerationChanged") < 0 ||
            sd_bus_message_append(signal.get(), "t", responses_.at(index_).generation) < 0 ||
            sd_bus_message_open_container(signal.get(), SD_BUS_TYPE_ARRAY, "s") < 0 ||
            sd_bus_message_append(signal.get(), "s", "profile") < 0 ||
            sd_bus_message_close_container(signal.get()) < 0 ||
            sd_bus_message_append(signal.get(), "sb", invalid ? "invalid" : "forge", 0) < 0 ||
            sd_bus_message_open_container(signal.get(), SD_BUS_TYPE_ARRAY, "s") < 0 ||
            sd_bus_message_close_container(signal.get()) < 0) {
            return;
        }
        (void)sd_bus_send(bus_, signal.get(), nullptr);
    }

    void run(std::promise<bool> ready) noexcept {
        ipc::sdbus::Bus bus;
        sd_bus *opened = nullptr;
        if (sd_bus_open_user(&opened) < 0 || opened == nullptr) {
            ready.set_value(false);
            return;
        }
        bus.reset(opened);
        bus_ = bus.get();
        ipc::sdbus::Slot object;
        if (sd_bus_add_object_vtable(bus.get(), object.put(), objectPath, snapshotInterface,
                                     snapshotVtable(), this) < 0 ||
            sd_bus_request_name(bus.get(), serviceName, options_.nameFlags) <= 0) {
            bus_ = nullptr;
            ready.set_value(false);
            return;
        }
        ready.set_value(true);

        while (running_.load()) {
            if (release_held_reply_.exchange(false) && held_reply_) {
                (void)sendSnapshotReply(held_reply_.get());
                held_reply_.reset();
                held_reply_ready_.store(false);
            }
            if (advance_.exchange(false) && index_ + 1U < responses_.size()) {
                ++index_;
                emitGenerationChanged();
            }
            if (invalid_signal_.exchange(false)) {
                emitGenerationChanged(true);
            }
            int processed = 0;
            do {
                processed = sd_bus_process(bus.get(), nullptr);
            } while (processed > 0);
            if (processed < 0) {
                break;
            }
            pollfd descriptor{sd_bus_get_fd(bus.get()),
                              static_cast<short>(sd_bus_get_events(bus.get())), 0};
            (void)::poll(&descriptor, 1U, 10);
        }
        held_reply_.reset();
        held_reply_ready_.store(false);
        bus_ = nullptr;
    }

    const std::vector<Response> responses_;
    const FakeSettingsServiceOptions options_;
    std::atomic_bool running_{true};
    std::atomic_bool advance_{false};
    std::atomic_bool invalid_signal_{false};
    std::atomic_bool release_held_reply_{false};
    std::atomic_bool held_reply_ready_{false};
    std::thread thread_;
    sd_bus *bus_{nullptr};
    ipc::sdbus::Message held_reply_;
    std::size_t index_{0U};
};

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate,
                             std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

TEST(SettingsSnapshotClientTest, FetchesInitialCompleteSnapshot) {
    auto responses = realProfileSnapshots();
    FakeSettingsService service({responses.front()});
    SettingsSnapshotClient client;
    prismdrake::shell::theme::ShellThemeSnapshotAdapter adapter;
    QObject::connect(&client, &SettingsSnapshotClient::snapshotChanged, &adapter, [&]() {
        if (client.currentSnapshot()) {
            EXPECT_TRUE(adapter.applySnapshot(client.currentSnapshot()));
        }
    });

    ASSERT_TRUE(client.start());
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SettingsSnapshotClient::State::ready; }));
    ASSERT_TRUE(client.currentSnapshot());
    EXPECT_EQ(client.currentSnapshot()->generation.value(), 1U);
    EXPECT_EQ(client.currentSnapshot()->candidate.configuration.profile, config::Profile::lustre);
    ASSERT_NE(adapter.current(), nullptr);
    EXPECT_EQ(adapter.current()->profileId(), QStringLiteral("lustre"));
}

TEST(SettingsSnapshotClientTest, RefetchesCompleteSnapshotWhenGenerationSignalArrives) {
    auto responses = realProfileSnapshots();
    FakeSettingsService service(responses);
    SettingsSnapshotClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));

    service.advanceAndSignal();

    ASSERT_TRUE(waitUntil([&]() {
        return client.currentSnapshot() != nullptr &&
               client.currentSnapshot()->generation.value() == 2U;
    }));
    EXPECT_EQ(client.currentSnapshot()->candidate.configuration.profile, config::Profile::forge);
}

TEST(SettingsSnapshotClientTest, RejectsMalformedAndOversizedSnapshotReplies) {
    {
        FakeSettingsService malformed({{1U, "{}"}});
        SettingsSnapshotClient client;
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(
            waitUntil([&]() { return client.state() == SettingsSnapshotClient::State::failed; }));
        EXPECT_FALSE(client.currentSnapshot());
        ASSERT_TRUE(client.lastError());
        EXPECT_EQ(client.lastError()->code, foundation::ErrorCode::validation_error);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    {
        FakeSettingsService oversized(
            {{1U, std::string(prismdrake::settings::maximumRuntimeSnapshotBytes + 1U, 'x')}});
        SettingsSnapshotClient client;
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(
            waitUntil([&]() { return client.state() == SettingsSnapshotClient::State::failed; }));
        EXPECT_FALSE(client.currentSnapshot());
        ASSERT_TRUE(client.lastError());
        EXPECT_EQ(client.lastError()->code, foundation::ErrorCode::too_large);
    }
}

TEST(SettingsSnapshotClientTest, RejectsInvalidClosedSignalFieldsAndRetainsSnapshot) {
    auto responses = realProfileSnapshots();
    FakeSettingsService service({responses.front()});
    SettingsSnapshotClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));
    const auto retained = client.currentSnapshot();

    service.emitInvalidSignal();

    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SettingsSnapshotClient::State::failed; }));
    EXPECT_EQ(client.currentSnapshot(), retained);
    ASSERT_TRUE(client.lastError());
    EXPECT_EQ(client.lastError()->code, foundation::ErrorCode::validation_error);
}

TEST(SettingsSnapshotClientTest, ClearsOnOwnerLossAndRefetchesAfterReacquisition) {
    auto responses = realProfileSnapshots();
    auto service = std::make_unique<FakeSettingsService>(std::vector<Response>{responses.front()});
    SettingsSnapshotClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));

    service.reset();
    ASSERT_TRUE(waitUntil([&]() {
        return client.state() == SettingsSnapshotClient::State::unavailable &&
               client.currentSnapshot() == nullptr;
    }));

    service = std::make_unique<FakeSettingsService>(std::vector<Response>{responses.back()});
    ASSERT_TRUE(waitUntil([&]() {
        return client.state() == SettingsSnapshotClient::State::ready &&
               client.currentSnapshot() != nullptr &&
               client.currentSnapshot()->generation.value() == 2U;
    }));
    EXPECT_EQ(client.currentSnapshot()->candidate.configuration.profile, config::Profile::forge);
}

TEST(SettingsSnapshotClientTest, RejectsHeldStartupReplyAfterOwnerReplacement) {
    auto responses = realProfileSnapshots();
    constexpr auto replaceableNameFlags = static_cast<std::uint64_t>(SD_BUS_NAME_ALLOW_REPLACEMENT);
    constexpr auto replacementNameFlags = static_cast<std::uint64_t>(SD_BUS_NAME_REPLACE_EXISTING);
    FakeSettingsService original({responses.front()},
                                 FakeSettingsServiceOptions{replaceableNameFlags, true});
    SettingsSnapshotClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() {
        return client.state() == SettingsSnapshotClient::State::fetching &&
               original.hasHeldSnapshotReply();
    }));

    FakeSettingsService replacement({responses.back()},
                                    FakeSettingsServiceOptions{replacementNameFlags, false});
    ASSERT_TRUE(waitUntil([&]() {
        return client.state() == SettingsSnapshotClient::State::ready &&
               client.currentSnapshot() != nullptr &&
               client.currentSnapshot()->generation.value() == 2U;
    }));
    EXPECT_EQ(client.currentSnapshot()->candidate.configuration.profile, config::Profile::forge);

    original.releaseHeldSnapshotReply();
    ASSERT_TRUE(waitUntil([&]() { return !original.hasHeldSnapshotReply(); }));
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < drainDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_EQ(client.state(), SettingsSnapshotClient::State::ready);
    ASSERT_TRUE(client.currentSnapshot());
    EXPECT_EQ(client.currentSnapshot()->generation.value(), 2U);
    EXPECT_EQ(client.currentSnapshot()->candidate.configuration.profile, config::Profile::forge);
    EXPECT_FALSE(client.lastError());
    client.stop();
}

} // namespace
} // namespace prismdrake::shell::settings

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
