#include "SessionControlClient.hpp"
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
constexpr char sessionServiceName[] = "org.prismdrake.Session1";
constexpr char sessionObjectPath[] = "/org/prismdrake/Session1";
constexpr char sessionInterface[] = "org.prismdrake.Session1";

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

enum class SessionReply : std::uint8_t { success, busy, timedOut, hold };

class FakeSessionService final {
  public:
    explicit FakeSessionService(SessionReply reply = SessionReply::success,
                                std::uint64_t transaction = 57U)
        : reply_(reply), transaction_(transaction) {
        std::promise<bool> ready;
        auto result = ready.get_future();
        thread_ =
            std::thread([this, signal = std::move(ready)]() mutable { run(std::move(signal)); });
        if (!result.get()) {
            running_.store(false);
            thread_.join();
            throw std::runtime_error("unable to start fake session service");
        }
    }

    ~FakeSessionService() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FakeSessionService(const FakeSessionService &) = delete;
    FakeSessionService &operator=(const FakeSessionService &) = delete;

    [[nodiscard]] std::uint32_t requestCount() const noexcept { return request_count_.load(); }
    [[nodiscard]] std::uint32_t confirmCount() const noexcept { return confirm_count_.load(); }
    [[nodiscard]] std::uint32_t cancelCount() const noexcept { return cancel_count_.load(); }

  private:
    [[nodiscard]] static const sd_bus_vtable *vtable() {
        static const sd_bus_vtable table[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetSessionState", "", "ts", getSessionState, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("RequestLogout", "", "t", requestLogout, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ConfirmLogout", "t", "", confirmLogout, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("CancelLogout", "t", "", cancelLogout, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_SIGNAL("SessionStateChanged", "ts", 0),
            SD_BUS_VTABLE_END,
        };
        return table;
    }

    static int getSessionState(sd_bus_message *message, void *userdata, sd_bus_error *) noexcept {
        auto &self = *static_cast<FakeSessionService *>(userdata);
        std::lock_guard lock(self.state_mutex_);
        return sd_bus_reply_method_return(message, "ts", self.current_transaction_,
                                          self.state_.c_str());
    }

    static int requestLogout(sd_bus_message *message, void *userdata,
                             sd_bus_error *error) noexcept {
        auto &self = *static_cast<FakeSessionService *>(userdata);
        self.request_count_.fetch_add(1U);
        switch (self.reply_) {
        case SessionReply::success: {
            std::lock_guard lock(self.state_mutex_);
            self.current_transaction_ = self.transaction_;
            self.state_ = "confirmation_requested";
        }
            self.emitState(self.transaction_, "confirmation_requested");
            return sd_bus_reply_method_return(message, "t", self.transaction_);
        case SessionReply::busy:
            return sd_bus_error_set_const(error, "org.prismdrake.Session1.Error.Busy",
                                          "A logout transaction is already active.");
        case SessionReply::timedOut:
            return sd_bus_error_set_const(error, "org.prismdrake.Session1.Error.TimedOut",
                                          "The logout request timed out.");
        case SessionReply::hold:
            self.held_call_.reset(sd_bus_message_ref(message));
            return self.held_call_ ? 1 : -ENOMEM;
        }
        return -EINVAL;
    }

    template <bool Confirm>
    static int mutate(sd_bus_message *message, void *userdata, sd_bus_error *error) noexcept {
        auto &self = *static_cast<FakeSessionService *>(userdata);
        std::uint64_t transaction = 0U;
        if (sd_bus_message_read(message, "t", &transaction) <= 0 ||
            sd_bus_message_at_end(message, 1) <= 0) {
            return -EINVAL;
        }
        {
            std::lock_guard lock(self.state_mutex_);
            if (transaction != self.current_transaction_ ||
                self.state_ != "confirmation_requested") {
                return sd_bus_error_set_const(error,
                                              "org.prismdrake.Session1.Error.TransactionMismatch",
                                              "The logout transaction does not match.");
            }
            self.state_ = Confirm ? "confirmed" : "cancelled";
        }
        if constexpr (Confirm) {
            self.confirm_count_.fetch_add(1U);
            self.emitState(transaction, "confirmed");
        } else {
            self.cancel_count_.fetch_add(1U);
            self.emitState(transaction, "cancelled");
            {
                std::lock_guard lock(self.state_mutex_);
                self.current_transaction_ = 0U;
                self.state_ = "idle";
            }
            self.emitState(transaction, "idle");
        }
        return sd_bus_reply_method_return(message, "");
    }

    static int confirmLogout(sd_bus_message *message, void *userdata,
                             sd_bus_error *error) noexcept {
        return mutate<true>(message, userdata, error);
    }

    static int cancelLogout(sd_bus_message *message, void *userdata, sd_bus_error *error) noexcept {
        return mutate<false>(message, userdata, error);
    }

    void emitState(std::uint64_t transaction, const char *state) noexcept {
        ipc::sdbus::Message signal;
        if (bus_ == nullptr ||
            sd_bus_message_new_signal(bus_, signal.put(), sessionObjectPath, sessionInterface,
                                      "SessionStateChanged") < 0 ||
            sd_bus_message_append(signal.get(), "ts", transaction, state) < 0) {
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
        if (sd_bus_add_object_vtable(bus.get(), object.put(), sessionObjectPath, sessionInterface,
                                     vtable(), this) < 0 ||
            sd_bus_request_name(bus.get(), sessionServiceName, 0U) <= 0) {
            bus_ = nullptr;
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
        bus_ = nullptr;
    }

    const SessionReply reply_;
    const std::uint64_t transaction_;
    std::atomic_bool running_{true};
    std::atomic_uint32_t request_count_{0U};
    std::atomic_uint32_t confirm_count_{0U};
    std::atomic_uint32_t cancel_count_{0U};
    std::mutex state_mutex_;
    std::uint64_t current_transaction_{0U};
    std::string state_{"idle"};
    ipc::sdbus::Message held_call_;
    std::thread thread_;
    sd_bus *bus_{nullptr};
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

TEST(SessionControlClientTest, RequestsAndConfirmsExactTypedTransaction) {
    FakeSessionService service(SessionReply::success, 73U);
    std::vector<SessionControlCompletion> completions;
    std::vector<SessionStatePublication> states;
    SessionControlClient client(
        [&](auto completion) { completions.push_back(std::move(completion)); },
        [&](auto publication) { states.push_back(std::move(publication)); });
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back().state, SessionState::idle);

    EXPECT_TRUE(client.requestLogout().accepted());
    EXPECT_TRUE(completions.empty());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
    EXPECT_EQ(completions.front().operation, SessionControlOperation::request);
    EXPECT_EQ(completions.front().failure, quicksettings::CommandFailure::none);
    ASSERT_TRUE(completions.front().transaction);
    const auto transaction = *completions.front().transaction;
    EXPECT_EQ(transaction.value, 73U);
    EXPECT_EQ(transaction.ownerEpoch, client.ownerEpoch());
    ASSERT_TRUE(waitUntil([&]() { return client.currentTransaction() == transaction; }));

    EXPECT_TRUE(client.confirmLogout(transaction).accepted());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 2U; }));
    EXPECT_EQ(completions.back().operation, SessionControlOperation::confirm);
    EXPECT_EQ(completions.back().failure, quicksettings::CommandFailure::none);
    EXPECT_EQ(completions.back().transaction, transaction);
    EXPECT_EQ(service.requestCount(), 1U);
    EXPECT_EQ(service.confirmCount(), 1U);
}

TEST(SessionControlClientTest, CancelsOnlyCurrentOwnerEpochTransaction) {
    FakeSessionService service(SessionReply::success, 81U);
    std::vector<SessionControlCompletion> completions;
    SessionControlClient client(
        [&](auto completion) { completions.push_back(std::move(completion)); }, nullptr);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
    ASSERT_TRUE(client.requestLogout().accepted());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
    ASSERT_TRUE(completions.front().transaction);
    const auto transaction = *completions.front().transaction;

    EXPECT_EQ(client.cancelLogout({transaction.ownerEpoch + 1U, transaction.value}).failure,
              quicksettings::CommandFailure::failed);
    EXPECT_EQ(client.cancelLogout({transaction.ownerEpoch, transaction.value + 1U}).failure,
              quicksettings::CommandFailure::failed);
    EXPECT_EQ(service.cancelCount(), 0U);
    EXPECT_TRUE(client.cancelLogout(transaction).accepted());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 2U; }));
    EXPECT_EQ(completions.back().operation, SessionControlOperation::cancel);
    EXPECT_EQ(completions.back().failure, quicksettings::CommandFailure::none);
    EXPECT_EQ(service.cancelCount(), 1U);
    ASSERT_TRUE(waitUntil([&]() { return !client.currentTransaction(); }));
}

TEST(SessionControlClientTest, RejectsConcurrentDispatchAndCompletesOwnerLoss) {
    auto service = std::make_unique<FakeSessionService>(SessionReply::hold);
    std::vector<SessionControlCompletion> completions;
    std::vector<SessionStatePublication> states;
    SessionControlClient client(
        [&](auto completion) { completions.push_back(std::move(completion)); },
        [&](auto publication) { states.push_back(std::move(publication)); });
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
    const auto firstEpoch = client.ownerEpoch();

    EXPECT_TRUE(client.requestLogout().accepted());
    EXPECT_EQ(client.requestLogout().failure, quicksettings::CommandFailure::busy);
    ASSERT_TRUE(waitUntil([&]() { return service->requestCount() == 1U; }));
    service.reset();

    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
    EXPECT_EQ(completions.front().failure, quicksettings::CommandFailure::unavailable);
    EXPECT_FALSE(completions.front().transaction);
    ASSERT_TRUE(waitUntil([&]() {
        return client.state() == SessionControlClient::State::unavailable && !states.empty() &&
               states.back().state == SessionState::unavailable;
    }));
    EXPECT_GT(client.ownerEpoch(), firstEpoch);
}

TEST(SessionControlClientTest, MapsBusyAndTimeoutWithoutSynthesizingTransaction) {
    for (const auto [reply, expected] : {
             std::pair{SessionReply::busy, quicksettings::CommandFailure::busy},
             std::pair{SessionReply::timedOut, quicksettings::CommandFailure::failed},
         }) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        FakeSessionService service(reply);
        std::vector<SessionControlCompletion> completions;
        SessionControlClient client(
            [&](auto completion) { completions.push_back(std::move(completion)); }, nullptr);
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(
            waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
        ASSERT_TRUE(client.requestLogout().accepted());
        ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
        EXPECT_EQ(completions.front().failure, expected);
        EXPECT_FALSE(completions.front().transaction);
    }
}

TEST(SessionControlClientTest, ReacquisitionRejectsPriorOwnerTransaction) {
    auto service = std::make_unique<FakeSessionService>(SessionReply::success, 91U);
    std::vector<SessionControlCompletion> completions;
    SessionControlClient client(
        [&](auto completion) { completions.push_back(std::move(completion)); }, nullptr);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
    ASSERT_TRUE(client.requestLogout().accepted());
    ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
    ASSERT_TRUE(completions.front().transaction);
    const auto stale = *completions.front().transaction;

    service.reset();
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SessionControlClient::State::unavailable; }));
    service = std::make_unique<FakeSessionService>(SessionReply::success, 92U);
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
    EXPECT_EQ(client.confirmLogout(stale).failure, quicksettings::CommandFailure::failed);
    EXPECT_EQ(service->confirmCount(), 0U);
}

TEST(SessionControlClientTest, OrdinaryStopCompletesButDestructionSuppressesCallbacks) {
    FakeSessionService service(SessionReply::hold);
    std::vector<SessionControlCompletion> completions;
    std::vector<SessionStatePublication> states;
    {
        SessionControlClient client(
            [&](auto completion) { completions.push_back(std::move(completion)); },
            [&](auto publication) { states.push_back(std::move(publication)); });
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(
            waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
        ASSERT_TRUE(client.requestLogout().accepted());
        ASSERT_TRUE(waitUntil([&]() { return service.requestCount() == 1U; }));
        client.stop();
        ASSERT_TRUE(waitUntil([&]() { return completions.size() == 1U; }));
        EXPECT_EQ(completions.front().failure, quicksettings::CommandFailure::unavailable);
        ASSERT_TRUE(waitUntil(
            [&]() { return !states.empty() && states.back().state == SessionState::unavailable; }));
    }

    completions.clear();
    states.clear();
    {
        SessionControlClient client(
            [&](auto completion) { completions.push_back(std::move(completion)); },
            [&](auto publication) { states.push_back(std::move(publication)); });
        ASSERT_TRUE(client.start());
        ASSERT_TRUE(
            waitUntil([&]() { return client.state() == SessionControlClient::State::available; }));
        ASSERT_TRUE(client.requestLogout().accepted());
        completions.clear();
        states.clear();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(completions.empty());
    EXPECT_TRUE(states.empty());
}

} // namespace
} // namespace prismdrake::shell::controlclients

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
