#include "SettingsProfileControlClient.hpp"

#include <QTimer>

#include <limits>
#include <string_view>
#include <utility>

namespace prismdrake::shell::controlclients {
namespace {

using quicksettings::CommandCompletion;
using quicksettings::CommandFailure;
using quicksettings::CommandResult;

constexpr std::string_view serviceName = "org.prismdrake.Settings1";
constexpr std::string_view objectPath = "/org/prismdrake/Settings1";
constexpr std::string_view interfaceName = "org.prismdrake.Settings1";
constexpr std::uint64_t profileTimeoutUsec = 6'000'000U;

[[nodiscard]] foundation::Error transportError() {
    return {foundation::ErrorCode::io_error, "The settings control connection failed.",
            "Retry after the settings service is available."};
}

[[nodiscard]] foundation::Error callbackError() {
    return {foundation::ErrorCode::invalid_argument,
            "The settings control completion callback is missing.",
            "Supply a C++ completion callback before starting the client."};
}

[[nodiscard]] bool validUniqueOwner(std::string_view owner) noexcept {
    return owner.empty() || (owner.size() <= 255U && owner.front() == ':');
}

[[nodiscard]] bool isMethodError(sd_bus_message *message) noexcept {
    return message == nullptr || sd_bus_message_is_method_error(message, nullptr) > 0;
}

[[nodiscard]] CommandFailure methodFailure(sd_bus_message *message) noexcept {
    const auto *error = message == nullptr ? nullptr : sd_bus_message_get_error(message);
    const std::string_view name = error == nullptr || error->name == nullptr ? "" : error->name;
    if (name == "org.prismdrake.Settings1.Error.Busy") {
        return CommandFailure::busy;
    }
    if (name == "org.prismdrake.Settings1.Error.NotAuthorized") {
        return CommandFailure::denied;
    }
    if (name == "org.prismdrake.Settings1.Error.ServiceStopping" ||
        name == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
        name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
        name == "org.freedesktop.DBus.Error.Disconnected") {
        return CommandFailure::unavailable;
    }
    return CommandFailure::failed;
}

[[nodiscard]] const char *profileName(config::Profile profile) noexcept {
    switch (profile) {
    case config::Profile::lustre:
        return "lustre";
    case config::Profile::forge:
        return "forge";
    }
    return nullptr;
}

} // namespace

SettingsProfileControlClient::SettingsProfileControlClient(CompletionCallback completionCallback,
                                                           QObject *parent)
    : QObject(parent), completion_callback_(std::move(completionCallback)),
      dispatcher_([this]() noexcept { handleBusFailure(); }) {}

SettingsProfileControlClient::~SettingsProfileControlClient() {
    destroying_ = true;
    stop();
}

foundation::Result<void> SettingsProfileControlClient::start() {
    if (state_ != State::stopped) {
        return foundation::Result<void>::failure({foundation::ErrorCode::invalid_argument,
                                                  "The settings profile client is already running.",
                                                  "Stop the client before starting it again."});
    }
    if (!completion_callback_) {
        return foundation::Result<void>::failure(callbackError());
    }
    auto opened = dispatcher_.start();
    if (!opened) {
        return opened;
    }
    setState(State::connecting);
    auto installed = installMatch();
    if (!installed) {
        const auto error = installed.error();
        stop();
        return foundation::Result<void>::failure(error);
    }
    dispatcher_.refresh();
    return foundation::Result<void>::success();
}

void SettingsProfileControlClient::stop() noexcept {
    owner_match_.reset();
    owner_query_.reset();
    profile_call_.reset();
    match_pending_ = false;
    owner_.clear();
    if (pending_) {
        finishPending({CommandFailure::unavailable, std::nullopt, std::nullopt});
    }
    dispatcher_.stop();
    setState(State::stopped);
}

foundation::Result<void> SettingsProfileControlClient::installMatch() {
    constexpr auto ownerRule =
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='org.prismdrake.Settings1'";
    if (sd_bus_add_match_async(dispatcher_.get(), owner_match_.put(), ownerRule, &nameOwnerChanged,
                               &matchInstalled, this) < 0) {
        return foundation::Result<void>::failure(transportError());
    }
    match_pending_ = true;
    return foundation::Result<void>::success();
}

foundation::Result<void> SettingsProfileControlClient::queryOwner() {
    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(dispatcher_.get(), request.put(), "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                       "GetNameOwner") < 0 ||
        sd_bus_message_append(request.get(), "s", serviceName.data()) < 0 ||
        sd_bus_call_async(dispatcher_.get(), owner_query_.put(), request.get(), &ownerReply, this,
                          profileTimeoutUsec) < 0) {
        return foundation::Result<void>::failure(transportError());
    }
    dispatcher_.refresh();
    return foundation::Result<void>::success();
}

CommandResult
SettingsProfileControlClient::requestProfile(config::Profile profile,
                                             quicksettings::CommandRequestId requestId) {
    if (state_ != State::available || owner_.empty() || !dispatcher_) {
        return {CommandFailure::unavailable};
    }
    if (pending_) {
        return {CommandFailure::busy};
    }
    const auto *profileValue = profileName(profile);
    if (profileValue == nullptr || requestId.value == 0U) {
        return {CommandFailure::failed};
    }

    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(dispatcher_.get(), request.put(), serviceName.data(),
                                       objectPath.data(), interfaceName.data(),
                                       "RequestProfileChange") < 0 ||
        sd_bus_message_append(request.get(), "s", profileValue) < 0) {
        return {CommandFailure::failed};
    }
    pending_ = PendingRequest{requestId, owner_epoch_, owner_};
    if (sd_bus_call_async(dispatcher_.get(), profile_call_.put(), request.get(), &profileReply,
                          this, profileTimeoutUsec) < 0) {
        pending_.reset();
        return {CommandFailure::failed};
    }
    dispatcher_.refresh();
    return {CommandFailure::none};
}

int SettingsProfileControlClient::matchInstalled(sd_bus_message *message, void *userdata,
                                                 sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsProfileControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    self->match_pending_ = false;
    try {
        if (isMethodError(message)) {
            self->invalidateOwner(CommandFailure::unavailable, State::failed);
            return 0;
        }
        auto queried = self->queryOwner();
        if (!queried) {
            self->invalidateOwner(CommandFailure::unavailable, State::failed);
        }
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SettingsProfileControlClient::nameOwnerChanged(sd_bus_message *message, void *userdata,
                                                   sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsProfileControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    try {
        const char *name = nullptr;
        const char *oldOwner = nullptr;
        const char *newOwner = nullptr;
        if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) <= 0 ||
            name == nullptr || std::string_view{name} != serviceName || oldOwner == nullptr ||
            newOwner == nullptr || !validUniqueOwner(oldOwner) || !validUniqueOwner(newOwner) ||
            sd_bus_message_at_end(message, 1) <= 0) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        self->owner_query_.reset();
        self->applyOwner(newOwner);
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SettingsProfileControlClient::ownerReply(sd_bus_message *message, void *userdata,
                                             sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsProfileControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    self->owner_query_.reset();
    try {
        if (sd_bus_message_is_method_error(message, "org.freedesktop.DBus.Error.NameHasNoOwner") >
            0) {
            self->applyOwner({});
            return 0;
        }
        const char *owner = nullptr;
        if (isMethodError(message) || sd_bus_message_read(message, "s", &owner) <= 0 ||
            owner == nullptr || !validUniqueOwner(owner) || std::string_view{owner}.empty() ||
            sd_bus_message_at_end(message, 1) <= 0) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        self->applyOwner(owner);
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SettingsProfileControlClient::profileReply(sd_bus_message *message, void *userdata,
                                               sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsProfileControlClient *>(userdata);
    if (self == nullptr || !self->pending_) {
        return 0;
    }
    self->profile_call_.reset();
    try {
        if (self->owner_ != self->pending_->owner ||
            self->owner_epoch_ != self->pending_->ownerEpoch) {
            self->finishPending({CommandFailure::unavailable, std::nullopt, std::nullopt});
            return 0;
        }
        if (isMethodError(message)) {
            const auto *sender = message == nullptr ? nullptr : sd_bus_message_get_sender(message);
            const auto *error = message == nullptr ? nullptr : sd_bus_message_get_error(message);
            const std::string_view errorName =
                error == nullptr || error->name == nullptr ? "" : error->name;
            if (errorName.starts_with("org.prismdrake.Settings1.Error.") &&
                (sender == nullptr || self->pending_->owner != sender)) {
                self->finishPending({CommandFailure::failed, std::nullopt, std::nullopt});
                return 0;
            }
            self->finishPending({methodFailure(message), std::nullopt, std::nullopt});
            return 0;
        }
        const auto *sender = sd_bus_message_get_sender(message);
        std::uint64_t generationValue = 0U;
        if (sender == nullptr || self->pending_->owner != sender ||
            sd_bus_message_read(message, "t", &generationValue) <= 0 ||
            sd_bus_message_at_end(message, 1) <= 0) {
            self->finishPending({CommandFailure::failed, std::nullopt, std::nullopt});
            return 0;
        }
        auto generation = foundation::Generation::fromPublished(generationValue);
        if (!generation) {
            self->finishPending({CommandFailure::failed, std::nullopt, std::nullopt});
            return 0;
        }
        self->finishPending({CommandFailure::none, std::move(generation).value(), std::nullopt});
    } catch (...) {
        self->finishPending({CommandFailure::failed, std::nullopt, std::nullopt});
    }
    return 0;
}

void SettingsProfileControlClient::handleBusFailure() noexcept {
    owner_match_.reset();
    owner_query_.reset();
    profile_call_.reset();
    match_pending_ = false;
    invalidateOwner(CommandFailure::unavailable, State::failed);
}

void SettingsProfileControlClient::applyOwner(std::string owner) noexcept {
    if (!validUniqueOwner(owner)) {
        invalidateOwner(CommandFailure::failed, State::failed);
        return;
    }
    if (owner_ == owner) {
        setState(owner.empty() ? State::unavailable : State::available);
        return;
    }
    if (!advanceOwnerEpoch()) {
        invalidateOwner(CommandFailure::failed, State::failed);
        return;
    }
    profile_call_.reset();
    if (pending_) {
        finishPending({CommandFailure::unavailable, std::nullopt, std::nullopt});
    }
    owner_ = std::move(owner);
    setState(owner_.empty() ? State::unavailable : State::available);
}

void SettingsProfileControlClient::invalidateOwner(CommandFailure failure, State state) noexcept {
    profile_call_.reset();
    owner_query_.reset();
    (void)advanceOwnerEpoch();
    owner_.clear();
    if (pending_) {
        finishPending({failure, std::nullopt, std::nullopt});
    }
    setState(state);
}

void SettingsProfileControlClient::finishPending(CommandCompletion completion) noexcept {
    if (!pending_) {
        return;
    }
    const auto requestId = pending_->requestId;
    pending_.reset();
    if (destroying_) {
        return;
    }
    QTimer::singleShot(0, this, [this, requestId, completion = std::move(completion)]() mutable {
        try {
            completion_callback_(requestId, std::move(completion));
        } catch (...) {
        }
    });
}

void SettingsProfileControlClient::setState(State state) noexcept {
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged();
}

bool SettingsProfileControlClient::advanceOwnerEpoch() noexcept {
    if (owner_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++owner_epoch_;
    return true;
}

} // namespace prismdrake::shell::controlclients
