#include "SettingsService.hpp"

#include "RuntimeSnapshot.hpp"
#include "SdBusApi.hpp"
#include "ServiceWorker.hpp"
#include "SettingsSnapshot.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace prismdrake::settingsd {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr char serviceName[] = "org.prismdrake.Settings1";
constexpr char objectPath[] = "/org/prismdrake/Settings1";
constexpr char settingsInterface[] = "org.prismdrake.Settings1";
constexpr char snapshotInterface[] = "org.prismdrake.SettingsSnapshot1";
constexpr std::size_t maximumProfileBytes = 64U;

constexpr char invalidProfileError[] = "org.prismdrake.Settings1.Error.InvalidProfile";
constexpr char validationFailedError[] = "org.prismdrake.Settings1.Error.ValidationFailed";
constexpr char tooLargeError[] = "org.prismdrake.Settings1.Error.TooLarge";
constexpr char unsupportedSnapshotVersionError[] =
    "org.prismdrake.Settings1.Error.UnsupportedSnapshotVersion";
constexpr char noSnapshotError[] = "org.prismdrake.Settings1.Error.NoSnapshot";
constexpr char busyError[] = "org.prismdrake.Settings1.Error.Busy";
constexpr char notAuthorizedError[] = "org.prismdrake.Settings1.Error.NotAuthorized";
constexpr char serviceStoppingError[] = "org.prismdrake.Settings1.Error.ServiceStopping";
constexpr char internalError[] = "org.prismdrake.Settings1.Error.Internal";

constexpr char invalidProfileMessage[] = "The requested profile is not supported.";
constexpr char validationFailedMessage[] = "The settings candidate was rejected.";
constexpr char tooLargeMessage[] = "The bounded request or reply is too large.";
constexpr char unsupportedSnapshotVersionMessage[] =
    "The requested snapshot schema version is not supported.";
constexpr char noSnapshotMessage[] = "No complete settings snapshot is available.";
constexpr char busyMessage[] = "The settings service worker is busy.";
constexpr char notAuthorizedMessage[] = "The settings request is not authorized.";
constexpr char serviceStoppingMessage[] = "The settings service is stopping.";
constexpr char internalMessage[] = "The settings service could not complete the request.";

[[nodiscard]] Error serviceError(ErrorCode code, std::string message, std::string recovery) {
    return {code, std::move(message), std::move(recovery)};
}

[[nodiscard]] int setBusError(sd_bus_error *error, const char *name, const char *message) {
    return sd_bus_error_set_const(error, name, message);
}

[[nodiscard]] int replyBusError(sd_bus_message *call, const char *name, const char *message) {
    return sd_bus_reply_method_errorf(call, name, "%s", message);
}

enum class PendingOperation : std::uint8_t {
    profile_change,
    reload,
    candidate_validation,
};

struct PendingCall final {
    WorkerRequestId requestId;
    PendingOperation operation;
    sdbus::Message call;
};

struct ErrorReply final {
    const char *name;
    const char *message;
};

[[nodiscard]] ErrorReply mapWorkerError(PendingOperation operation, const Error &error) {
    if (error.code == ErrorCode::too_large) {
        return {tooLargeError, tooLargeMessage};
    }
    if (error.code == ErrorCode::cancelled) {
        return {serviceStoppingError, serviceStoppingMessage};
    }
    if (operation == PendingOperation::profile_change &&
        error.code == ErrorCode::invalid_argument) {
        return {invalidProfileError, invalidProfileMessage};
    }
    if (operation == PendingOperation::reload &&
        (error.code == ErrorCode::syntax_error || error.code == ErrorCode::validation_error ||
         error.code == ErrorCode::unsupported || error.code == ErrorCode::not_found)) {
        return {validationFailedError, validationFailedMessage};
    }
    return {internalError, internalMessage};
}

[[nodiscard]] int appendStringArray(sd_bus_message *message,
                                    const std::vector<std::string> &values) {
    int result = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
    if (result < 0) {
        return result;
    }
    for (const auto &value : values) {
        result = sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, value.c_str());
        if (result < 0) {
            return result;
        }
    }
    return sd_bus_message_close_container(message);
}

class SettingsService final {
  public:
    SettingsService(sd_bus *bus, ServiceWorker &worker) : bus_(bus), worker_(worker) {}

    [[nodiscard]] Result<void> attach() {
        int result = sd_bus_add_object_vtable(bus_, settings_slot_.put(), objectPath,
                                              settingsInterface, settingsVtable(), this);
        if (result < 0) {
            return Result<void>::failure(serviceError(
                ErrorCode::io_error, "Could not register the Experimental settings interface.",
                "Reconnect to the session bus and retry."));
        }
        result = sd_bus_add_object_vtable(bus_, snapshot_slot_.put(), objectPath, snapshotInterface,
                                          snapshotVtable(), this);
        if (result < 0) {
            return Result<void>::failure(serviceError(
                ErrorCode::io_error, "Could not register the Experimental snapshot interface.",
                "Reconnect to the session bus and retry."));
        }
        return Result<void>::success();
    }

    [[nodiscard]] int notificationFileDescriptor() const noexcept {
        return worker_.notificationFileDescriptor();
    }

    [[nodiscard]] Result<void> dispatchCompletion() {
        auto completion = worker_.takeCompletion();
        if (!completion) {
            return Result<void>::success();
        }
        if (!pending_ || pending_->requestId != completion->requestId) {
            return Result<void>::failure(serviceError(
                ErrorCode::io_error, "The settings worker returned an unmatched completion.",
                "Restart prismdrake-settingsd to begin a fresh owner epoch."));
        }

        auto pending = std::move(*pending_);
        pending_.reset();
        int result = 0;
        if (const auto *publication = std::get_if<PublicationResult>(&completion->result)) {
            if (!publication->hasValue()) {
                const auto mapped = mapWorkerError(pending.operation, publication->error());
                result = replyBusError(pending.call.get(), mapped.name, mapped.message);
            } else {
                const auto &outcome = publication->value();
                if (outcome.published) {
                    result = emitGenerationChanged(outcome);
                }
                if (result >= 0) {
                    result = sd_bus_reply_method_return(pending.call.get(), "t",
                                                        outcome.snapshot->generation.value());
                }
            }
        } else {
            const auto &validation = std::get<CandidateValidationResult>(completion->result);
            if (!validation) {
                const auto mapped = mapWorkerError(pending.operation, validation.error());
                result = replyBusError(pending.call.get(), mapped.name, mapped.message);
            } else {
                result = replyCandidateValidation(pending.call.get(), validation.value());
            }
        }

        if (result < 0) {
            return Result<void>::failure(
                serviceError(ErrorCode::io_error, "Could not send a settings method result.",
                             "Reconnect to the session bus and refetch the complete snapshot."));
        }
        return Result<void>::success();
    }

    void rejectPendingForShutdown() noexcept {
        if (pending_) {
            (void)replyBusError(pending_->call.get(), serviceStoppingError, serviceStoppingMessage);
            pending_.reset();
        }
    }

  private:
    [[nodiscard]] static const sd_bus_vtable *settingsVtable() {
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetCurrentProfile", "", "st", handleGetCurrentProfile,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("RequestProfileChange", "s", "t", handleRequestProfileChange,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Reload", "", "t", handleReload, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ValidateCandidate", "ay", "ba(sssss)", handleValidateCandidate,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_SIGNAL("SettingsGenerationChanged", "tassbas", 0),
            SD_BUS_VTABLE_END,
        };
        return vtable;
    }

    [[nodiscard]] static const sd_bus_vtable *snapshotVtable() {
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetCurrentSnapshot", "u", "tay", handleGetCurrentSnapshot,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END,
        };
        return vtable;
    }

    [[nodiscard]] static SettingsService &fromUserData(void *userData) {
        return *static_cast<SettingsService *>(userData);
    }

    [[nodiscard]] static int authorize(sd_bus_message *message, sd_bus_error *error) {
        sdbus::Credentials credentials;
        int result = sd_bus_query_sender_creds(message, SD_BUS_CREDS_EUID, credentials.put());
        uid_t effectiveUserId = 0;
        if (result < 0 || !credentials ||
            sd_bus_creds_get_euid(credentials.get(), &effectiveUserId) < 0 ||
            effectiveUserId != ::geteuid()) {
            return setBusError(error, notAuthorizedError, notAuthorizedMessage);
        }
        return 0;
    }

    [[nodiscard]] static int handleGetCurrentProfile(sd_bus_message *message, void *userData,
                                                     sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        const auto snapshot = fromUserData(userData).worker_.currentSnapshot();
        if (!snapshot) {
            return setBusError(error, noSnapshotError, noSnapshotMessage);
        }
        const std::string profile{settings::profileId(snapshot->candidate.configuration.profile)};
        return sd_bus_reply_method_return(message, "st", profile.c_str(),
                                          snapshot->generation.value());
    }

    [[nodiscard]] static int handleRequestProfileChange(sd_bus_message *message, void *userData,
                                                        sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        const char *profile = nullptr;
        if (sd_bus_message_read(message, "s", &profile) < 0 || profile == nullptr) {
            return -EINVAL;
        }
        const std::string_view requested{profile};
        if (requested.size() > maximumProfileBytes ||
            (requested != "lustre" && requested != "forge")) {
            return setBusError(error, invalidProfileError, invalidProfileMessage);
        }
        return fromUserData(userData).submit(message, error, PendingOperation::profile_change,
                                             ProfileChangeJob{std::string{requested}});
    }

    [[nodiscard]] static int handleReload(sd_bus_message *message, void *userData,
                                          sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        return fromUserData(userData).submit(message, error, PendingOperation::reload, ReloadJob{});
    }

    [[nodiscard]] static int handleValidateCandidate(sd_bus_message *message, void *userData,
                                                     sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        const void *candidateBytes = nullptr;
        std::size_t candidateSize = 0;
        if (sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &candidateBytes, &candidateSize) <
            0) {
            return -EINVAL;
        }
        if (candidateSize > config::maximumConfigurationBytes) {
            return setBusError(error, tooLargeError, tooLargeMessage);
        }
        const auto *bytes = static_cast<const char *>(candidateBytes);
        std::string candidate;
        if (candidateSize != 0U) {
            candidate.assign(bytes, candidateSize);
        }
        return fromUserData(userData).submit(message, error, PendingOperation::candidate_validation,
                                             CandidateValidationJob{std::move(candidate)});
    }

    [[nodiscard]] static int handleGetCurrentSnapshot(sd_bus_message *message, void *userData,
                                                      sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        std::uint32_t requestedVersion = 0;
        if (sd_bus_message_read(message, "u", &requestedVersion) < 0) {
            return -EINVAL;
        }
        if (requestedVersion != settings::runtimeSnapshotSchemaVersion) {
            return setBusError(error, unsupportedSnapshotVersionError,
                               unsupportedSnapshotVersionMessage);
        }
        const auto snapshot = fromUserData(userData).worker_.currentSnapshot();
        if (!snapshot) {
            return setBusError(error, noSnapshotError, noSnapshotMessage);
        }
        if (snapshot->serializedJson.size() > settings::maximumRuntimeSnapshotBytes) {
            return setBusError(error, tooLargeError, tooLargeMessage);
        }

        sdbus::Message reply;
        int result = sd_bus_message_new_method_return(message, reply.put());
        if (result >= 0) {
            result = sd_bus_message_append(reply.get(), "t", snapshot->generation.value());
        }
        if (result >= 0) {
            result = sd_bus_message_append_array(reply.get(), SD_BUS_TYPE_BYTE,
                                                 snapshot->serializedJson.data(),
                                                 snapshot->serializedJson.size());
        }
        if (result >= 0) {
            result = sd_bus_send(fromUserData(userData).bus_, reply.get(), nullptr);
        }
        return result;
    }

    template <typename Operation>
    [[nodiscard]] int submit(sd_bus_message *message, sd_bus_error *error,
                             PendingOperation operation, Operation workerOperation) {
        if (pending_) {
            return setBusError(error, busyError, busyMessage);
        }
        if (next_request_id_ == 0U) {
            return setBusError(error, internalError, internalMessage);
        }

        const auto requestId = next_request_id_++;
        sdbus::Message retained{sd_bus_message_ref(message)};
        if (!retained || !worker_.trySubmit(WorkerJob{requestId, std::move(workerOperation)})) {
            return setBusError(error, busyError, busyMessage);
        }
        pending_.emplace(PendingCall{requestId, operation, std::move(retained)});
        return 1;
    }

    [[nodiscard]] int
    replyCandidateValidation(sd_bus_message *call,
                             const settings::CandidateValidation &validation) const {
        sdbus::Message reply;
        int result = sd_bus_message_new_method_return(call, reply.put());
        const int valid = validation.valid ? 1 : 0;
        if (result >= 0) {
            result = sd_bus_message_append(reply.get(), "b", valid);
        }
        if (result >= 0) {
            result = sd_bus_message_open_container(reply.get(), SD_BUS_TYPE_ARRAY, "(sssss)");
        }
        for (const auto &diagnostic : validation.diagnostics) {
            if (result < 0) {
                break;
            }
            result = sd_bus_message_open_container(reply.get(), SD_BUS_TYPE_STRUCT, "sssss");
            if (result >= 0) {
                result = sd_bus_message_append(
                    reply.get(), "sssss", diagnostic.logicalSourceId.c_str(),
                    diagnostic.fieldPath.c_str(), diagnostic.diagnosticCode.c_str(),
                    diagnostic.expectedId.c_str(), diagnostic.recoveryId.c_str());
            }
            if (result >= 0) {
                result = sd_bus_message_close_container(reply.get());
            }
        }
        if (result >= 0) {
            result = sd_bus_message_close_container(reply.get());
        }
        if (result >= 0) {
            result = sd_bus_send(bus_, reply.get(), nullptr);
        }
        return result;
    }

    [[nodiscard]] int emitGenerationChanged(const settings::PublicationOutcome &outcome) const {
        sdbus::Message signal;
        int result = sd_bus_message_new_signal(bus_, signal.put(), objectPath, settingsInterface,
                                               "SettingsGenerationChanged");
        if (result >= 0) {
            result = sd_bus_message_append(signal.get(), "t", outcome.snapshot->generation.value());
        }

        std::vector<std::string> domains;
        domains.reserve(outcome.changedDomains.size());
        for (const auto domain : outcome.changedDomains) {
            domains.emplace_back(settings::settingsDomainId(domain));
        }
        if (result >= 0) {
            result = appendStringArray(signal.get(), domains);
        }
        const std::string profile{
            settings::profileId(outcome.snapshot->candidate.configuration.profile)};
        constexpr int restartRequired = 0;
        if (result >= 0) {
            result = sd_bus_message_append(signal.get(), "sb", profile.c_str(), restartRequired);
        }

        std::vector<std::string> warnings;
        warnings.reserve(outcome.snapshot->candidate.warnings.size() +
                         outcome.operationWarnings.size());
        const auto appendWarning = [&warnings](settings::SettingsWarning warning) {
            const std::string id{settings::settingsWarningId(warning)};
            if (std::find(warnings.begin(), warnings.end(), id) == warnings.end()) {
                warnings.push_back(id);
            }
        };
        for (const auto warning : outcome.snapshot->candidate.warnings) {
            appendWarning(warning);
        }
        for (const auto warning : outcome.operationWarnings) {
            appendWarning(warning);
        }
        if (result >= 0) {
            result = appendStringArray(signal.get(), warnings);
        }
        if (result >= 0) {
            result = sd_bus_send(bus_, signal.get(), nullptr);
        }
        return result;
    }

    sd_bus *bus_;
    ServiceWorker &worker_;
    sdbus::Slot settings_slot_;
    sdbus::Slot snapshot_slot_;
    std::optional<PendingCall> pending_;
    WorkerRequestId next_request_id_ = 1U;
};

[[nodiscard]] bool busDisconnected(short revents) noexcept {
    return (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

} // namespace

Result<ServiceEpochOutcome> runServiceEpoch(const settings::SettingsEngineOptions &options,
                                            const volatile std::sig_atomic_t &stopRequested) {
    auto engine = settings::SettingsEngine::start(options);
    if (!engine) {
        return Result<ServiceEpochOutcome>::failure(engine.error());
    }
    auto worker = ServiceWorker::create(std::move(engine).value());
    if (!worker) {
        return Result<ServiceEpochOutcome>::failure(worker.error());
    }

    sdbus::Bus bus;
    if (sd_bus_open_user(bus.put()) < 0) {
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
    }
    if (sd_bus_set_exit_on_disconnect(bus.get(), 0) < 0) {
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
    }

    SettingsService service{bus.get(), *worker.value()};
    auto attached = service.attach();
    if (!attached) {
        return Result<ServiceEpochOutcome>::failure(attached.error());
    }
    const int nameResult = sd_bus_request_name(bus.get(), serviceName, 0);
    if (nameResult == -EEXIST) {
        return Result<ServiceEpochOutcome>::failure(serviceError(
            ErrorCode::invalid_environment, "The settings service name is already owned.",
            "Stop the competing service before starting prismdrake-settingsd."));
    }
    if (nameResult < 0) {
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
    }

    while (stopRequested == 0) {
        int processResult = 0;
        do {
            processResult = sd_bus_process(bus.get(), nullptr);
        } while (processResult > 0);
        if (processResult < 0) {
            service.rejectPendingForShutdown();
            worker.value()->stop();
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }

        std::array<pollfd, 2> descriptors{{
            {sd_bus_get_fd(bus.get()), static_cast<short>(sd_bus_get_events(bus.get())), 0},
            {service.notificationFileDescriptor(), POLLIN, 0},
        }};
        const int pollResult = ::poll(descriptors.data(), descriptors.size(), 100);
        if (pollResult < 0 && errno != EINTR) {
            service.rejectPendingForShutdown();
            worker.value()->stop();
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }
        if (busDisconnected(descriptors[0].revents)) {
            service.rejectPendingForShutdown();
            worker.value()->stop();
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            auto completion = service.dispatchCompletion();
            if (!completion) {
                service.rejectPendingForShutdown();
                worker.value()->stop();
                return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
            }
        }
    }

    service.rejectPendingForShutdown();
    worker.value()->stop();
    (void)sd_bus_release_name(bus.get(), serviceName);
    (void)sd_bus_flush(bus.get());
    return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::stopped);
}

} // namespace prismdrake::settingsd
