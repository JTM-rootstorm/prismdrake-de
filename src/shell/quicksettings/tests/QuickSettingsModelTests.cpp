#include "QuickSettingsModel.hpp"

#include "SettingsEngine.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace prismdrake::shell::quicksettings {
namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::string pathTemplate =
            (std::filesystem::temp_directory_path() / "prismdrake-quick-settings-test.XXXXXX")
                .string();
        char *created = ::mkdtemp(pathTemplate.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class SettingsFixture final {
  public:
    SettingsFixture() {
        const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
        const config::ConfigurationLocations locations{temporary_.path() / "config/config.toml",
                                                       temporary_.path() /
                                                           "state/last-known-valid-config.toml",
                                                       source / "data/defaults/config.toml"};
        auto started = settings::SettingsEngine::start({locations, source / "themes", {}, {}});
        if (!started) {
            throw std::runtime_error(started.error().message);
        }
        engine_ = std::move(started).value();
    }

    [[nodiscard]] settings::SettingsEngine &engine() noexcept { return *engine_; }

  private:
    TemporaryDirectory temporary_;
    std::unique_ptr<settings::SettingsEngine> engine_;
};

class ProfilePort final : public ProfileControlPort {
  public:
    CommandResult result{CommandFailure::none};
    int calls{0};
    config::Profile last{config::Profile::lustre};
    CommandRequestId lastRequest{0U};

    [[nodiscard]] CommandResult requestProfile(config::Profile profile,
                                               CommandRequestId requestId) override {
        ++calls;
        last = profile;
        lastRequest = requestId;
        return result;
    }
};

class DndPort final : public DoNotDisturbControlPort {
  public:
    CommandResult result{CommandFailure::none};
    int calls{0};
    bool last{false};
    CommandRequestId lastRequest{0U};

    [[nodiscard]] CommandResult requestDoNotDisturb(bool enabled,
                                                    CommandRequestId requestId) override {
        ++calls;
        last = enabled;
        lastRequest = requestId;
        return result;
    }
};

class CenterPort final : public NotificationCenterPort {
  public:
    CommandResult result{CommandFailure::none};
    int calls{0};

    [[nodiscard]] CommandResult requestOpen() override {
        ++calls;
        return result;
    }
};

class LogoutPort final : public LogoutControlPort {
  public:
    CommandResult result{CommandFailure::none};
    int calls{0};

    [[nodiscard]] CommandResult requestLogout() override {
        ++calls;
        return result;
    }
};

[[nodiscard]] DoNotDisturbPublication dnd(std::uint64_t owner, std::uint64_t generation,
                                          bool effective) {
    return {owner, foundation::Generation::fromPublished(generation).value(), effective};
}

[[nodiscard]] std::shared_ptr<const settings::SettingsSnapshot>
snapshotAt(const settings::SettingsSnapshot &content, std::uint64_t generation) {
    return std::make_shared<const settings::SettingsSnapshot>(settings::SettingsSnapshot{
        content.snapshotSchemaVersion, foundation::Generation::fromPublished(generation).value(),
        content.candidate, content.serializedJson});
}

TEST(QuickSettingsModelTest, StartsWithOnlyFiveUnavailablePrismdrakeCapabilities) {
    QuickSettingsModel model;

    EXPECT_EQ(model.profile()->kindId(), QStringLiteral("profile"));
    EXPECT_EQ(model.doNotDisturb()->kindId(), QStringLiteral("do_not_disturb"));
    EXPECT_EQ(model.notificationCenter()->kindId(), QStringLiteral("notification_center"));
    EXPECT_EQ(model.logout()->kindId(), QStringLiteral("logout"));
    EXPECT_EQ(model.diagnostics()->kindId(), QStringLiteral("diagnostics"));
    EXPECT_EQ(model.profile()->state(), AdapterState::unavailable);
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::unavailable);
    EXPECT_EQ(model.notificationCenter()->state(), AdapterState::unavailable);
    EXPECT_EQ(model.logout()->state(), AdapterState::unavailable);
    EXPECT_EQ(model.diagnostics()->state(), AdapterState::unavailable);
}

TEST(QuickSettingsModelTest, AppliesProfileOnlyAfterCompleteSettingsGeneration) {
    SettingsFixture settings;
    ProfilePort port;
    QuickSettingsModel model;
    model.setProfileControlPort(&port);

    ASSERT_TRUE(model.applySettingsSnapshot(settings.engine().current()));
    EXPECT_EQ(model.profile()->state(), AdapterState::available);
    EXPECT_EQ(model.profile()->valueId(), QStringLiteral("lustre"));

    EXPECT_TRUE(model.requestForgeProfile());
    EXPECT_EQ(port.calls, 1);
    EXPECT_EQ(port.last, config::Profile::forge);
    EXPECT_EQ(model.profile()->state(), AdapterState::pending);
    EXPECT_EQ(model.profile()->valueId(), QStringLiteral("lustre"));

    auto publication = settings.engine().requestProfileChange("forge");
    ASSERT_TRUE(publication);
    ASSERT_TRUE(model.completeProfileRequest(
        port.lastRequest,
        {CommandFailure::none, publication.value().snapshot->generation, std::nullopt}));
    ASSERT_TRUE(model.applySettingsSnapshot(publication.value().snapshot));
    EXPECT_EQ(model.profile()->state(), AdapterState::available);
    EXPECT_EQ(model.profile()->valueId(), QStringLiteral("forge"));
    EXPECT_EQ(model.profile()->valueText(), QStringLiteral("Prismdrake Forge"));
}

TEST(QuickSettingsModelTest, RejectsStaleAndConflictingSettingsWithoutReplacingState) {
    SettingsFixture settings;
    ProfilePort port;
    QuickSettingsModel model;
    model.setProfileControlPort(&port);
    ASSERT_TRUE(model.applySettingsSnapshot(settings.engine().current()));
    auto original = settings.engine().current();

    auto forge = settings.engine().requestProfileChange("forge");
    ASSERT_TRUE(forge);
    ASSERT_TRUE(model.applySettingsSnapshot(forge.value().snapshot));
    EXPECT_EQ(model.profile()->valueId(), QStringLiteral("forge"));
    EXPECT_FALSE(model.applySettingsSnapshot(original));

    auto conflict = std::make_shared<const settings::SettingsSnapshot>(settings::SettingsSnapshot{
        forge.value().snapshot->snapshotSchemaVersion, forge.value().snapshot->generation,
        original->candidate, original->serializedJson});
    EXPECT_FALSE(model.applySettingsSnapshot(std::move(conflict)));
    EXPECT_EQ(model.profile()->valueId(), QStringLiteral("forge"));
}

TEST(QuickSettingsModelTest, DndUsesOwnerScopedCompleteGenerations) {
    DndPort port;
    QuickSettingsModel model;
    model.setDoNotDisturbControlPort(&port);
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(10U, 4U, false)));
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::disabled);

    EXPECT_TRUE(model.requestDoNotDisturb(true));
    EXPECT_EQ(port.calls, 1);
    EXPECT_TRUE(port.last);
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::pending);
    EXPECT_FALSE(model.doNotDisturb()->checked());

    ASSERT_TRUE(model.completeDoNotDisturbRequest(
        port.lastRequest,
        {CommandFailure::none, foundation::Generation::fromPublished(5U).value(), 10U}));
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(10U, 5U, true)));
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::enabled);
    EXPECT_TRUE(model.doNotDisturb()->checked());
    EXPECT_FALSE(model.applyDoNotDisturbPublication(dnd(10U, 4U, false)));
    EXPECT_FALSE(model.applyDoNotDisturbPublication(dnd(10U, 5U, false)));

    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(11U, 1U, false)));
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::disabled);
    EXPECT_FALSE(model.doNotDisturb()->checked());
    EXPECT_FALSE(model.applyDoNotDisturbPublication(dnd(10U, 99U, true)));
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::disabled);
}

TEST(QuickSettingsModelTest, UnrelatedGenerationsDoNotFalselyRejectPendingMutations) {
    SettingsFixture settings;
    ProfilePort profilePort;
    DndPort dndPort;
    QuickSettingsModel model;
    model.setProfileControlPort(&profilePort);
    model.setDoNotDisturbControlPort(&dndPort);
    const auto original = settings.engine().current();
    ASSERT_TRUE(model.applySettingsSnapshot(original));
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(20U, 7U, false)));

    ASSERT_TRUE(model.requestForgeProfile());
    ASSERT_TRUE(model.requestDoNotDisturb(true));

    const auto forge = settings.engine().requestProfileChange("forge");
    ASSERT_TRUE(forge);
    ASSERT_TRUE(model.applySettingsSnapshot(snapshotAt(*original, 2U)));
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(20U, 8U, false)));
    EXPECT_EQ(model.profile()->state(), AdapterState::pending);
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::pending);

    ASSERT_TRUE(model.completeProfileRequest(
        profilePort.lastRequest,
        {CommandFailure::none, foundation::Generation::fromPublished(3U).value(), std::nullopt}));
    ASSERT_TRUE(model.completeDoNotDisturbRequest(
        dndPort.lastRequest,
        {CommandFailure::none, foundation::Generation::fromPublished(9U).value(), 20U}));
    EXPECT_EQ(model.profile()->state(), AdapterState::pending);
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::pending);

    ASSERT_TRUE(model.applySettingsSnapshot(snapshotAt(*forge.value().snapshot, 3U)));
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(20U, 9U, true)));
    EXPECT_EQ(model.profile()->state(), AdapterState::available);
    EXPECT_EQ(model.profile()->valueId(), QStringLiteral("forge"));
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::enabled);
    EXPECT_TRUE(model.doNotDisturb()->checked());
}

TEST(QuickSettingsModelTest, FailedAdapterDoesNotDamageOtherCapabilities) {
    SettingsFixture settings;
    ProfilePort profilePort;
    DndPort dndPort;
    CenterPort centerPort;
    LogoutPort logoutPort;
    QuickSettingsModel model;
    model.setProfileControlPort(&profilePort);
    model.setDoNotDisturbControlPort(&dndPort);
    model.setNotificationCenterPort(&centerPort);
    model.setLogoutControlPort(&logoutPort);
    model.setNotificationCenterAvailable(true);
    model.setSessionAvailable(true);
    model.applyDiagnosticSummary(DiagnosticSummary::healthy);
    ASSERT_TRUE(model.applySettingsSnapshot(settings.engine().current()));
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(1U, 1U, false)));

    dndPort.result = {CommandFailure::denied};
    EXPECT_FALSE(model.requestDoNotDisturb(true));
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::error);
    EXPECT_EQ(model.profile()->state(), AdapterState::available);
    EXPECT_EQ(model.notificationCenter()->state(), AdapterState::available);
    EXPECT_EQ(model.logout()->state(), AdapterState::available);
    EXPECT_EQ(model.diagnostics()->state(), AdapterState::enabled);

    EXPECT_TRUE(model.requestNotificationCenter());
    EXPECT_EQ(model.notificationCenter()->state(), AdapterState::pending);
    model.observeNotificationCenterVisible(true);
    EXPECT_EQ(model.notificationCenter()->state(), AdapterState::available);
    EXPECT_EQ(model.notificationCenter()->valueText(), QStringLiteral("Open"));

    EXPECT_TRUE(model.requestLogout());
    EXPECT_EQ(model.logout()->state(), AdapterState::pending);
    model.observeSessionState(SessionPresentationState::confirmationRequested);
    EXPECT_EQ(model.logout()->state(), AdapterState::pending);
    model.observeSessionState(SessionPresentationState::idle);
    EXPECT_EQ(model.logout()->state(), AdapterState::available);
}

TEST(QuickSettingsModelTest, ServiceLossInvalidatesOnlyDependentAdapterState) {
    DndPort dndPort;
    CenterPort centerPort;
    LogoutPort logoutPort;
    QuickSettingsModel model;
    model.setDoNotDisturbControlPort(&dndPort);
    model.setNotificationCenterPort(&centerPort);
    model.setLogoutControlPort(&logoutPort);
    model.setNotificationCenterAvailable(true);
    model.setSessionAvailable(true);
    model.applyDiagnosticSummary(DiagnosticSummary::degraded);
    ASSERT_TRUE(model.applyDoNotDisturbPublication(dnd(8U, 2U, true)));

    model.clearNotificationOwner();
    EXPECT_EQ(model.doNotDisturb()->state(), AdapterState::unavailable);
    EXPECT_EQ(model.notificationCenter()->state(), AdapterState::unavailable);
    EXPECT_EQ(model.logout()->state(), AdapterState::available);
    EXPECT_EQ(model.diagnostics()->state(), AdapterState::enabled);
}

} // namespace
} // namespace prismdrake::shell::quicksettings
