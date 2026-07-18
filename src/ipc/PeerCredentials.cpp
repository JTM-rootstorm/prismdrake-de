#include "PeerCredentials.hpp"

#include <optional>

#include <unistd.h>

namespace prismdrake::ipc {
namespace {

[[nodiscard]] std::optional<EffectiveUserId>
senderEffectiveUserId(sd_bus_message *message) noexcept {
    if (message == nullptr) {
        return std::nullopt;
    }

    sdbus::Credentials credentials;
    if (sd_bus_query_sender_creds(message, SD_BUS_CREDS_EUID, credentials.put()) < 0 ||
        !credentials) {
        return std::nullopt;
    }

    uid_t effectiveUserId = 0;
    if (sd_bus_creds_get_euid(credentials.get(), &effectiveUserId) < 0) {
        return std::nullopt;
    }
    return static_cast<EffectiveUserId>(effectiveUserId);
}

} // namespace

bool isSameEffectiveUser(std::optional<EffectiveUserId> peerEffectiveUserId,
                         EffectiveUserId processEffectiveUserId) noexcept {
    return peerEffectiveUserId && *peerEffectiveUserId == processEffectiveUserId;
}

bool isSameUserPeer(sd_bus_message *message) noexcept {
    return isSameEffectiveUser(senderEffectiveUserId(message),
                               static_cast<EffectiveUserId>(::geteuid()));
}

} // namespace prismdrake::ipc
