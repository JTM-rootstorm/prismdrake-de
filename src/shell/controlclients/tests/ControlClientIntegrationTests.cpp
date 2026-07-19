#include "SettingsProfileControlClient.hpp"

#include "SdBusApi.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::controlclients {
namespace {

constexpr char settingsServiceName[] = "org.prismdrake.Settings1";
constexpr char settingsObjectPath[] = "/org/prismdrake/Settings1";
constexpr char settingsInterface[] = "org.prismdrake.Settings1";

enum class SettingsReply : std::uint8_t { success, busy, timedOut, hold };

class FakeSettingsProfileService final {
  public:
    explicit FakeSettingsProfileService(SettingsReply reply = SettingsReply::success,
                                        std::uint64_t generation = 7U)
        : reply_(reply), generation_(generation) {
        std::promise<bool> ready;
        auto result = ready.get_future();
        thread_ =
            std::thread([this, signal = std::move(ready)]() mutable { run(std::move(signal)); });
        if (!result.get()) {
            running_.store(false);
            thread_.join();
            throw std::runtime_error("unable to start fake settings profile service");
        }
    }

    ~FakeSettingsProfileService() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FakeSettingsProfileService(const FakeSettingsProfileService &) = delete;
    FakeSettingsProfileService &operator=(const FakeSettingsProfileService &) = delete;

    [[nodiscard]] std::uint32_t callCount() const noexcept { return call_count_.load(); }
    [[nodiscard]] std::string requestedProfile() const {
        std::lock_guard lock(profile_mutex_);
        return requested_profile_;
    }

  private:
    [[nodiscard]] static const sd_bus_vtable *vtable() {
        static const sd_bus_vtable table[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("RequestProfileChange", "s", "t", requestProfileChange,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END,
        };
        return table;
    }

    static int requestProfileChange(sd_bus_message *message, void *userdata,
                                    sd_bus_error *error) noexcept {
        auto &self = *static_cast<FakeSettingsProfileService *>(userdata);
        const char *profile = nullptr;
        if (sd_bus_message_read(message, "s", &profile) <= 0 || profile == nullptr ||
            sd_bus_message_at_end(message, 1) <= 0) {
            return -EINVAL;
        }
        {
            std::lock_guard lock(self.profile_mutex_);
            self.requested_profile_ = profile;
        }
        self.call_count_.fetch_add(1U);
        switch (self.reply_) {
        case SettingsReply::success:
            return sd_bus_reply_method_return(message, "t", self.generation_);
        case SettingsReply::busy:
            return sd_bus_error_set_const(error, "org.prismdrake.Settings1.Error.Busy",
                                          "Settings operation is already active.");
        case SettingsReply::timedOut:
            return sd_bus_error_set_const(error, "org.prismdrake.Settings1.Error.TimedOut",
                                          "Settings operation timed out.");
        case SettingsReply::hold:
            self.held_call_.reset(sd_bus_message_ref(message));
            return self.held_call_ ? 1 : -ENOMEM;
        }
        return -EINVAL;
    }

    void run(std::promise<bool> ready) noexcept {
        ipc::sdbus::Bus bus;
        sd_bus *opened = nullptr;
        if (sd_bus_open_user(&opened) < 0 || opened == nullptr) {
            ready.set_value(false);
            return;
        }
        bus.reset(opened);
        ipc::sdbus::Slot object;
        if (sd_bus_add_object_vtable(bus.get(), object.put(), settingsObjectPath, settingsInterface,
                                     vtable(), this) < 0 ||
            sd_bus_request_name(bus.get(), settingsServiceName, 0U) <= 0) {
            ready.set_value(false);
            return;
        }
        ready.set_value(true);
        while (running_.load()) {
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
        held_call_.reset();
    }

    const SettingsReply reply_;
    const std::uint64_t generation_;
    std::atomic_bool running_{true};
    std::atomic_uint32_t call_count_{0U};
    mutable std::mutex profile_mutex_;
    std::string requested_profile_;
    ipc::sdbus::Message held_call_;
    std::thread thread_;
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

TEST(SettingsProfileControlClientTest, CorrelatesRequestAndExactGenerationAsynchronously) {
    FakeSettingsProfileService service(SettingsReply::success, 19U);
    std::vector<std::pair<quicksettings::CommandRequestId, quicksettings::CommandCompletion>>
        completions;
    SettingsProfileControlClient client([&](auto requestId, auto completion) {
        completions.emplace_back(requestId, std::move(completion));
    });
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil(
        [&]() { return client.state() == SettingsProfileControlClient::State::available; }));

    const quicksettings::CommandRequestId requestId{41U};
    EXPECT_TRUE(client.requestProfile(config::Profile::forge, requestId).accepted());
    EXPECT_TRUE(completions.empty());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));

    EXPECT_EQ(completions.front().first, requestId);
    EXPECT_EQ(completions.front().second.failure, quicksettings::CommandFailure::none);
    ASSERT_TRUE(completions.front().second.resultingGeneration);
    EXPECT_EQ(completions.front().second.resultingGeneration->value(), 19U);
    EXPECT_FALSE(completions.front().second.ownerEpoch);
    EXPECT_EQ(service.requestedProfile(), "forge");
    EXPECT_EQ(service.callCount(), 1U);
}

TEST(SettingsProfileControlClientTest, RejectsConcurrentDispatchWithoutQueueing) {
    auto service = std::make_unique<FakeSettingsProfileService>(SettingsReply::hold);
    std::vector<quicksettings::CommandCompletion> completions;
    SettingsProfileControlClient client(
        [&](auto, auto completion) { completions.push_back(std::move(completion)); });
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil(
        [&]() { return client.state() == SettingsProfileControlClient::State::available; }));

    EXPECT_TRUE(client.requestProfile(config::Profile::forge, {1U}).accepted());
    EXPECT_EQ(client.requestProfile(config::Profile::lustre, {2U}).failure,
              quicksettings::CommandFailure::busy);
    ASSERT_TRUE(waitUntil([&]() { return service->callCount() == 1U; }));

    service.reset();
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
    EXPECT_EQ(completions.front().failure, quicksettings::CommandFailure::unavailable);
    EXPECT_FALSE(completions.front().resultingGeneration);
}

TEST(SettingsProfileControlClientTest, MapsClosedServiceFailures) {
    for (const auto [reply, expected] : {
             std::pair{SettingsReply::busy, quicksettings::CommandFailure::busy},
             std::pair{SettingsReply::timedOut, quicksettings::CommandFailure::failed},
         }) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        FakeSettingsProfileService service(reply);
        std::vector<quicksettings::CommandCompletion> completions;
        SettingsProfileControlClient client(
            [&](auto, auto completion) { completions.push_back(std::move(completion)); });
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(waitUntil(
            [&]() { return client.state() == SettingsProfileControlClient::State::available; }));
        ASSERT_TRUE(client.requestProfile(config::Profile::forge, {8U}).accepted());
        ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
        EXPECT_EQ(completions.front().failure, expected);
        EXPECT_FALSE(completions.front().resultingGeneration);
    }
}

TEST(SettingsProfileControlClientTest, OrdinaryStopCompletesButDestructionSuppressesCallback) {
    FakeSettingsProfileService service(SettingsReply::hold);
    std::vector<quicksettings::CommandCompletion> completions;
    {
        SettingsProfileControlClient client(
            [&](auto, auto completion) { completions.push_back(std::move(completion)); });
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(waitUntil(
            [&]() { return client.state() == SettingsProfileControlClient::State::available; }));
        ASSERT_TRUE(client.requestProfile(config::Profile::forge, {1U}).accepted());
        ASSERT_TRUE(waitUntil([&]() { return service.callCount() == 1U; }));
        client.stop();
        ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
        EXPECT_EQ(completions.front().failure, quicksettings::CommandFailure::unavailable);
    }

    completions.clear();
    {
        SettingsProfileControlClient client(
            [&](auto, auto completion) { completions.push_back(std::move(completion)); });
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(waitUntil(
            [&]() { return client.state() == SettingsProfileControlClient::State::available; }));
        ASSERT_TRUE(client.requestProfile(config::Profile::forge, {2U}).accepted());
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(completions.empty());
}

TEST(SettingsProfileControlClientTest, InvalidatesOwnerEpochAndReacquires) {
    auto service = std::make_unique<FakeSettingsProfileService>(SettingsReply::success, 2U);
    std::vector<quicksettings::CommandCompletion> completions;
    SettingsProfileControlClient client(
        [&](auto, auto completion) { completions.push_back(std::move(completion)); });
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil(
        [&]() { return client.state() == SettingsProfileControlClient::State::available; }));
    const auto firstEpoch = client.ownerEpoch();

    service.reset();
    ASSERT_TRUE(waitUntil(
        [&]() { return client.state() == SettingsProfileControlClient::State::unavailable; }));
    EXPECT_GT(client.ownerEpoch(), firstEpoch);
    EXPECT_EQ(client.requestProfile(config::Profile::forge, {1U}).failure,
              quicksettings::CommandFailure::unavailable);

    service = std::make_unique<FakeSettingsProfileService>(SettingsReply::success, 3U);
    ASSERT_TRUE(waitUntil(
        [&]() { return client.state() == SettingsProfileControlClient::State::available; }));
    EXPECT_TRUE(client.requestProfile(config::Profile::lustre, {9U}).accepted());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
    ASSERT_TRUE(completions.front().resultingGeneration);
    EXPECT_EQ(completions.front().resultingGeneration->value(), 3U);
}

} // namespace
} // namespace prismdrake::shell::controlclients

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
