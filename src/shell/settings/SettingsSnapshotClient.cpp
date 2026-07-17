#include "SettingsSnapshotClient.hpp"

#include "RuntimeSnapshot.hpp"
#include "RuntimeSnapshotParser.hpp"

#include <QSocketNotifier>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>

namespace prismdrake::shell::settings {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view serviceName = "org.prismdrake.Settings1";
constexpr std::string_view objectPath = "/org/prismdrake/Settings1";
constexpr std::string_view snapshotInterface = "org.prismdrake.SettingsSnapshot1";
constexpr std::uint64_t methodTimeoutUsec = 2'000'000U;
constexpr int maximumDispatchBatch = 64;

[[nodiscard]] Error transportError() {
    return {ErrorCode::io_error, "The settings service connection failed.",
            "Continue with the fallback theme and retry when the session bus is available."};
}

[[nodiscard]] Error protocolError() {
    return {ErrorCode::validation_error, "The settings service returned an invalid reply.",
            "Retain the prior complete generation and retry after the next service change."};
}

[[nodiscard]] Error unavailableError() {
    return {ErrorCode::not_found, "The settings service is unavailable.",
            "Continue with the fallback theme until the service acquires its bus name."};
}

[[nodiscard]] Error oversizedError() {
    return {ErrorCode::too_large, "The settings service snapshot exceeds the transport limit.",
            "Retain the prior complete generation and wait for a bounded service snapshot."};
}

[[nodiscard]] bool isMethodError(sd_bus_message *message) noexcept {
    return message == nullptr || sd_bus_message_is_method_error(message, nullptr) > 0;
}

[[nodiscard]] bool validUniqueOwner(std::string_view owner) noexcept {
    return owner.empty() || (owner.size() <= 255U && owner.front() == ':');
}

template <std::size_t Size>
[[nodiscard]] bool readClosedStringArray(sd_bus_message *message,
                                         const std::array<std::string_view, Size> &allowed,
                                         std::size_t maximumLength) noexcept {
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") < 0) {
        return false;
    }
    std::array<bool, Size> seen{};
    for (;;) {
        const char *value = nullptr;
        const auto read = sd_bus_message_read(message, "s", &value);
        if (read < 0) {
            return false;
        }
        if (read == 0) {
            break;
        }
        if (value == nullptr || std::strlen(value) > maximumLength) {
            return false;
        }
        const auto found = std::find(allowed.begin(), allowed.end(), value);
        if (found == allowed.end()) {
            return false;
        }
        const auto index = static_cast<std::size_t>(std::distance(allowed.begin(), found));
        if (seen[index]) {
            return false;
        }
        seen[index] = true;
    }
    return sd_bus_message_exit_container(message) >= 0;
}

} // namespace

SettingsSnapshotClient::SettingsSnapshotClient(QObject *parent) : QObject(parent) {
    busTimer_.setSingleShot(true);
    QObject::connect(&busTimer_, &QTimer::timeout, this, [this]() noexcept { dispatchBus(); });
}

SettingsSnapshotClient::~SettingsSnapshotClient() { stop(); }

Result<void> SettingsSnapshotClient::start() {
    if (bus_) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The settings client is already running.",
                                      "Stop the client before starting it again."});
    }

    sd_bus *opened = nullptr;
    if (sd_bus_open_user(&opened) < 0 || opened == nullptr) {
        return Result<void>::failure(transportError());
    }
    bus_.reset(opened);
    (void)sd_bus_set_exit_on_disconnect(bus_.get(), 0);
    setState(State::connecting);

    auto installed = installMatches();
    if (!installed) {
        const auto error = installed.error();
        stop();
        return Result<void>::failure(error);
    }

    const auto descriptor = sd_bus_get_fd(bus_.get());
    if (descriptor < 0) {
        stop();
        return Result<void>::failure(transportError());
    }
    readNotifier_ = std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Read, this);
    writeNotifier_ = std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Write, this);
    QObject::connect(readNotifier_.get(), &QSocketNotifier::activated, this,
                     [this]() noexcept { dispatchBus(); });
    QObject::connect(writeNotifier_.get(), &QSocketNotifier::activated, this,
                     [this]() noexcept { dispatchBus(); });
    updateEventSources();
    return Result<void>::success();
}

void SettingsSnapshotClient::stop() noexcept {
    busTimer_.stop();
    readNotifier_.reset();
    writeNotifier_.reset();
    ownerQuery_.reset();
    snapshotCall_.reset();
    ownerMatch_.reset();
    generationMatch_.reset();
    bus_.reset();
    pendingMatchInstalls_ = 0U;
    fetchPending_ = false;
    dispatchQueued_ = false;
    owner_.clear();
    requestedOwner_.clear();
    clearSnapshot();
    lastError_.reset();
    setState(State::stopped);
}

Result<void> SettingsSnapshotClient::installMatches() {
    constexpr auto ownerRule =
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='org.prismdrake.Settings1'";
    constexpr auto generationRule =
        "type='signal',path='/org/prismdrake/Settings1',"
        "interface='org.prismdrake.Settings1',member='SettingsGenerationChanged'";

    if (sd_bus_add_match_async(bus_.get(), ownerMatch_.put(), ownerRule, &nameOwnerChanged,
                               &matchInstalled, this) < 0) {
        return Result<void>::failure(transportError());
    }
    ++pendingMatchInstalls_;
    if (sd_bus_add_match_async(bus_.get(), generationMatch_.put(), generationRule,
                               &generationChanged, &matchInstalled, this) < 0) {
        return Result<void>::failure(transportError());
    }
    ++pendingMatchInstalls_;
    return Result<void>::success();
}

Result<void> SettingsSnapshotClient::queryOwner() {
    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(bus_.get(), request.put(), "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                       "GetNameOwner") < 0 ||
        sd_bus_message_append(request.get(), "s", serviceName.data()) < 0 ||
        sd_bus_call_async(bus_.get(), ownerQuery_.put(), request.get(), &ownerReply, this,
                          methodTimeoutUsec) < 0) {
        return Result<void>::failure(transportError());
    }
    return Result<void>::success();
}

Result<void> SettingsSnapshotClient::requestSnapshot() {
    if (owner_.empty()) {
        return Result<void>::failure(unavailableError());
    }
    if (snapshotCall_) {
        fetchPending_ = true;
        return Result<void>::success();
    }

    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(bus_.get(), request.put(), serviceName.data(),
                                       objectPath.data(), snapshotInterface.data(),
                                       "GetCurrentSnapshot") < 0 ||
        sd_bus_message_append(request.get(), "u",
                              prismdrake::settings::runtimeSnapshotSchemaVersion) < 0) {
        return Result<void>::failure(transportError());
    }
    requestedOwner_ = owner_;
    if (sd_bus_call_async(bus_.get(), snapshotCall_.put(), request.get(), &snapshotReply, this,
                          methodTimeoutUsec) < 0) {
        requestedOwner_.clear();
        return Result<void>::failure(transportError());
    }
    setState(State::fetching);
    return Result<void>::success();
}

int SettingsSnapshotClient::matchInstalled(sd_bus_message *message, void *userdata,
                                           sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsSnapshotClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    try {
        if (isMethodError(message)) {
            self->setFailure(transportError(), false);
            return 0;
        }
        if (self->pendingMatchInstalls_ > 0U) {
            --self->pendingMatchInstalls_;
        }
        if (self->pendingMatchInstalls_ == 0U) {
            auto queried = self->queryOwner();
            if (!queried) {
                self->setFailure(queried.error(), false);
            }
        }
    } catch (...) {
        self->setFailure(protocolError(), false);
    }
    return 0;
}

int SettingsSnapshotClient::nameOwnerChanged(sd_bus_message *message, void *userdata,
                                             sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsSnapshotClient *>(userdata);
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
            self->invalidateOwnerEpoch(protocolError());
            return 0;
        }
        // The owner-transition signal is authoritative for the query epoch. Cancelling a
        // concurrent initial reply prevents a stale NameHasNoOwner result from clearing it.
        self->ownerQuery_.reset();
        self->applyOwner(newOwner);
    } catch (...) {
        self->invalidateOwnerEpoch(protocolError());
    }
    return 0;
}

int SettingsSnapshotClient::generationChanged(sd_bus_message *message, void *userdata,
                                              sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsSnapshotClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    try {
        const auto *sender = sd_bus_message_get_sender(message);
        std::uint64_t generation = 0U;
        const char *activeProfile = nullptr;
        int restartRequired = 0;
        constexpr std::array changedDomains{
            std::string_view{"profile"},       std::string_view{"appearance"},
            std::string_view{"panel"},         std::string_view{"launcher"},
            std::string_view{"notifications"}, std::string_view{"desktop"},
            std::string_view{"integration"},   std::string_view{"accessibility"},
            std::string_view{"keyboard"},      std::string_view{"developer"},
            std::string_view{"theme"}};
        constexpr std::array validationWarnings{
            std::string_view{"invalid_user_configuration"},
            std::string_view{"invalid_last_known_valid_configuration"},
            std::string_view{"last_known_valid_persistence_failed"}};
        if (sender == nullptr || self->owner_ != sender) {
            return 0;
        }
        if (sd_bus_message_read(message, "t", &generation) <= 0 || generation == 0U ||
            !readClosedStringArray(message, changedDomains, 64U) ||
            sd_bus_message_read(message, "sb", &activeProfile, &restartRequired) <= 0 ||
            activeProfile == nullptr ||
            (std::string_view{activeProfile} != "lustre" &&
             std::string_view{activeProfile} != "forge") ||
            restartRequired != 0 || !readClosedStringArray(message, validationWarnings, 64U) ||
            sd_bus_message_at_end(message, 1) <= 0) {
            self->setFailure(protocolError(), true);
            return 0;
        }
        auto requested = self->requestSnapshot();
        if (!requested) {
            self->setFailure(requested.error(), true);
        }
    } catch (...) {
        self->setFailure(protocolError(), true);
    }
    return 0;
}

int SettingsSnapshotClient::ownerReply(sd_bus_message *message, void *userdata,
                                       sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsSnapshotClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    self->ownerQuery_.reset();
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
            self->setFailure(protocolError(), false);
            return 0;
        }
        self->applyOwner(owner);
    } catch (...) {
        self->setFailure(protocolError(), false);
    }
    return 0;
}

int SettingsSnapshotClient::snapshotReply(sd_bus_message *message, void *userdata,
                                          sd_bus_error *) noexcept {
    auto *self = static_cast<SettingsSnapshotClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    self->snapshotCall_.reset();
    const auto expectedOwner = std::exchange(self->requestedOwner_, {});
    try {
        const auto *sender = sd_bus_message_get_sender(message);
        std::uint64_t generation = 0U;
        const void *bytes = nullptr;
        std::size_t byteCount = 0U;
        if (isMethodError(message) || sender == nullptr || self->owner_ != expectedOwner ||
            expectedOwner != sender || sd_bus_message_read(message, "t", &generation) <= 0 ||
            generation == 0U ||
            sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes, &byteCount) < 0 ||
            bytes == nullptr || byteCount == 0U || sd_bus_message_at_end(message, 1) <= 0) {
            self->setFailure(protocolError(), true);
            self->schedulePendingFetch();
            return 0;
        }
        if (byteCount > prismdrake::settings::maximumRuntimeSnapshotBytes) {
            self->setFailure(oversizedError(), true);
            self->schedulePendingFetch();
            return 0;
        }

        const auto view = std::string_view{static_cast<const char *>(bytes), byteCount};
        auto parsed = parseRuntimeSnapshot(generation, view);
        if (!parsed) {
            self->setFailure(parsed.error(), true);
            self->schedulePendingFetch();
            return 0;
        }
        if (self->snapshot_) {
            const auto currentGeneration = self->snapshot_->generation.value();
            const auto receivedGeneration = parsed.value()->generation.value();
            if (receivedGeneration < currentGeneration ||
                (receivedGeneration == currentGeneration &&
                 parsed.value()->serializedJson != self->snapshot_->serializedJson)) {
                self->setFailure(protocolError(), true);
                self->schedulePendingFetch();
                return 0;
            }
        }
        if (!self->snapshot_ || parsed.value()->serializedJson != self->snapshot_->serializedJson) {
            self->snapshot_ = std::move(parsed).value();
            emit self->snapshotChanged();
        }
        self->lastError_.reset();
        self->setState(State::ready);
        self->schedulePendingFetch();
    } catch (...) {
        self->setFailure(protocolError(), true);
        self->schedulePendingFetch();
    }
    return 0;
}

void SettingsSnapshotClient::dispatchBus() noexcept {
    if (!bus_) {
        return;
    }
    if (readNotifier_) {
        readNotifier_->setEnabled(false);
    }
    if (writeNotifier_) {
        writeNotifier_->setEnabled(false);
    }

    int processed = 0;
    while (processed < maximumDispatchBatch) {
        const auto result = sd_bus_process(bus_.get(), nullptr);
        if (result < 0) {
            handleBusFailure();
            return;
        }
        if (result == 0) {
            break;
        }
        ++processed;
    }
    if (processed == maximumDispatchBatch && !dispatchQueued_) {
        dispatchQueued_ = true;
        QTimer::singleShot(0, this, [this]() noexcept {
            dispatchQueued_ = false;
            dispatchBus();
        });
    }
    updateEventSources();
}

void SettingsSnapshotClient::updateEventSources() noexcept {
    if (!bus_) {
        return;
    }
    const auto events = sd_bus_get_events(bus_.get());
    if (events < 0) {
        handleBusFailure();
        return;
    }
    if (readNotifier_) {
        readNotifier_->setEnabled((events & POLLIN) != 0);
    }
    if (writeNotifier_) {
        writeNotifier_->setEnabled((events & POLLOUT) != 0);
    }

    std::uint64_t timeout = 0U;
    if (sd_bus_get_timeout(bus_.get(), &timeout) < 0) {
        handleBusFailure();
        return;
    }
    if (timeout == std::numeric_limits<std::uint64_t>::max()) {
        busTimer_.stop();
        return;
    }
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0 || now.tv_nsec < 0) {
        handleBusFailure();
        return;
    }
    const auto seconds = static_cast<std::uint64_t>(now.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(now.tv_nsec);
    if (seconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000U) {
        handleBusFailure();
        return;
    }
    const auto nowUsec = seconds * 1'000'000U + nanoseconds / 1'000U;
    const auto delayUsec = timeout > nowUsec ? timeout - nowUsec : 0U;
    const auto delayMs =
        std::min<std::uint64_t>((delayUsec + 999U) / 1000U, std::numeric_limits<int>::max());
    busTimer_.start(static_cast<int>(delayMs));
}

void SettingsSnapshotClient::handleBusFailure() noexcept {
    busTimer_.stop();
    if (readNotifier_) {
        readNotifier_->setEnabled(false);
    }
    if (writeNotifier_) {
        writeNotifier_->setEnabled(false);
    }
    ownerQuery_.reset();
    snapshotCall_.reset();
    ownerMatch_.reset();
    generationMatch_.reset();
    bus_.reset();
    owner_.clear();
    requestedOwner_.clear();
    pendingMatchInstalls_ = 0U;
    fetchPending_ = false;
    dispatchQueued_ = false;
    clearSnapshot();
    setFailure(transportError(), false);
}

void SettingsSnapshotClient::invalidateOwnerEpoch(Error error) noexcept {
    ownerQuery_.reset();
    snapshotCall_.reset();
    owner_.clear();
    requestedOwner_.clear();
    fetchPending_ = false;
    clearSnapshot();
    setFailure(std::move(error), false);
}

void SettingsSnapshotClient::applyOwner(std::string owner) noexcept {
    if (!validUniqueOwner(owner)) {
        setFailure(protocolError(), true);
        return;
    }
    if (owner_ == owner) {
        if (!owner.empty() && !snapshot_ && !snapshotCall_) {
            auto requested = requestSnapshot();
            if (!requested) {
                setFailure(requested.error(), false);
            }
        }
        return;
    }

    snapshotCall_.reset();
    requestedOwner_.clear();
    fetchPending_ = false;
    owner_ = std::move(owner);
    clearSnapshot();
    lastError_.reset();
    if (owner_.empty()) {
        lastError_ = unavailableError();
        setState(State::unavailable);
        return;
    }
    auto requested = requestSnapshot();
    if (!requested) {
        setFailure(requested.error(), false);
    }
}

void SettingsSnapshotClient::clearSnapshot() noexcept {
    if (snapshot_) {
        snapshot_.reset();
        emit snapshotChanged();
    }
}

void SettingsSnapshotClient::setState(State state) noexcept {
    if (state_ != state) {
        state_ = state;
        emit stateChanged();
    }
}

void SettingsSnapshotClient::setFailure(Error error, bool retainSnapshot) noexcept {
    if (!retainSnapshot) {
        clearSnapshot();
    }
    lastError_ = std::move(error);
    setState(State::failed);
}

void SettingsSnapshotClient::schedulePendingFetch() noexcept {
    if (!fetchPending_) {
        return;
    }
    fetchPending_ = false;
    if (!owner_.empty()) {
        auto requested = requestSnapshot();
        if (!requested) {
            setFailure(requested.error(), true);
        }
    }
}

} // namespace prismdrake::shell::settings
