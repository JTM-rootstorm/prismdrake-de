#include "EwmhCapabilities.hpp"

#include "AtomCache.hpp"
#include "EwmhCapabilitiesInternal.hpp"
#include "PropertyReader.hpp"
#include "X11Connection.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<EwmhCapabilities> transportFailure() {
    return Result<EwmhCapabilities>::failure(
        {ErrorCode::io_error, "The X11 connection failed during EWMH discovery.",
         "Reconnect to the development X server and rebuild capability state."});
}

[[nodiscard]] Result<EwmhCapabilities> reduced(EwmhDiscoveryStatus status) {
    return Result<EwmhCapabilities>::success(EwmhCapabilities{status, {}});
}

[[nodiscard]] Result<EwmhCapabilities> reducedAfterReadFailure(X11Connection &connection,
                                                               const foundation::Error &error,
                                                               EwmhDiscoveryStatus missingStatus) {
    if (!connection.healthy()) {
        return transportFailure();
    }
    return reduced(error.code == ErrorCode::not_found ? missingStatus
                                                      : EwmhDiscoveryStatus::malformed);
}

[[nodiscard]] bool contains(std::span<const AtomId> values,
                            std::optional<AtomId> expected) noexcept {
    return expected && std::find(values.begin(), values.end(), expected.value()) != values.end();
}

[[nodiscard]] EwmhCapabilityFlags capabilityFlags(std::span<const AtomId> supported,
                                                  const AtomCache &atoms) noexcept {
    const bool clientList = contains(supported, atoms.atom(AtomName::net_client_list));
    const bool activeWindow = contains(supported, atoms.atom(AtomName::net_active_window));
    const bool basicWorkspaces =
        contains(supported, atoms.atom(AtomName::net_number_of_desktops)) &&
        contains(supported, atoms.atom(AtomName::net_current_desktop)) &&
        contains(supported, atoms.atom(AtomName::net_wm_desktop));
    const bool dockWindowType = contains(supported, atoms.atom(AtomName::net_wm_window_type)) &&
                                contains(supported, atoms.atom(AtomName::net_wm_window_type_dock));
    const bool dockStrutPartial =
        dockWindowType && contains(supported, atoms.atom(AtomName::net_wm_strut_partial));
    const bool closeWindow = contains(supported, atoms.atom(AtomName::net_close_window));
    return {clientList,     activeWindow,     basicWorkspaces,
            dockWindowType, dockStrutPartial, closeWindow};
}

} // namespace

std::string_view ewmhDiscoveryStatusId(EwmhDiscoveryStatus status) noexcept {
    switch (status) {
    case EwmhDiscoveryStatus::unavailable:
        return "ewmh_unavailable";
    case EwmhDiscoveryStatus::malformed:
        return "ewmh_malformed";
    case EwmhDiscoveryStatus::verified:
        return "ewmh_verified";
    }
    return "ewmh_malformed";
}

Result<EwmhCapabilities> detail::discoverEwmhCapabilitiesWithInterleave(
    X11Connection &connection, EwmhDiscoveryInterleaveHook interleave, void *context) {
    if (!connection.healthy()) {
        return transportFailure();
    }

    auto atoms = AtomCache::create(connection);
    if (!atoms) {
        return connection.healthy() ? reduced(EwmhDiscoveryStatus::malformed) : transportFailure();
    }
    constexpr PropertySpec ownerSpec{AtomName::window, PropertyFormat::bits_32, 1U,
                                     sizeof(std::uint32_t)};

    auto rootOwner = PropertyReader::read(connection, atoms.value(), connection.screen().rootWindow,
                                          AtomName::net_supporting_wm_check, ownerSpec);
    if (!rootOwner) {
        return reducedAfterReadFailure(connection, rootOwner.error(),
                                       EwmhDiscoveryStatus::unavailable);
    }
    auto rootOwnerItems = rootOwner.value().uint32Items();
    if (!rootOwnerItems || rootOwnerItems.value().size() != 1U) {
        return connection.healthy() ? reduced(EwmhDiscoveryStatus::malformed) : transportFailure();
    }
    auto supportingWindow = WindowId::fromProtocol(rootOwnerItems.value().front());
    if (!supportingWindow) {
        return reduced(EwmhDiscoveryStatus::malformed);
    }

    auto selfReference = PropertyReader::read(connection, atoms.value(), supportingWindow.value(),
                                              AtomName::net_supporting_wm_check, ownerSpec);
    if (!selfReference) {
        return reducedAfterReadFailure(connection, selfReference.error(),
                                       EwmhDiscoveryStatus::malformed);
    }
    auto selfReferenceItems = selfReference.value().uint32Items();
    if (!selfReferenceItems || selfReferenceItems.value().size() != 1U ||
        selfReferenceItems.value().front() != supportingWindow.value().value()) {
        return connection.healthy() ? reduced(EwmhDiscoveryStatus::malformed) : transportFailure();
    }

    constexpr PropertySpec supportedSpec{AtomName::atom, PropertyFormat::bits_32,
                                         maximumEwmhSupportedAtoms,
                                         maximumEwmhSupportedAtoms * sizeof(std::uint32_t)};
    auto supported = PropertyReader::read(connection, atoms.value(), connection.screen().rootWindow,
                                          AtomName::net_supported, supportedSpec);
    if (!supported) {
        return reducedAfterReadFailure(connection, supported.error(),
                                       EwmhDiscoveryStatus::malformed);
    }
    auto supportedItems = supported.value().uint32Items();
    if (!supportedItems) {
        return connection.healthy() ? reduced(EwmhDiscoveryStatus::malformed) : transportFailure();
    }
    std::vector<AtomId> supportedAtoms;
    supportedAtoms.reserve(supportedItems.value().size());
    for (const auto rawAtom : supportedItems.value()) {
        auto atom = AtomId::fromProtocol(rawAtom);
        if (!atom) {
            return reduced(EwmhDiscoveryStatus::malformed);
        }
        supportedAtoms.push_back(atom.value());
    }

    if (interleave != nullptr) {
        interleave(context);
    }
    auto confirmedRootOwner =
        PropertyReader::read(connection, atoms.value(), connection.screen().rootWindow,
                             AtomName::net_supporting_wm_check, ownerSpec);
    if (!confirmedRootOwner) {
        return reducedAfterReadFailure(connection, confirmedRootOwner.error(),
                                       EwmhDiscoveryStatus::malformed);
    }
    auto confirmedRootOwnerItems = confirmedRootOwner.value().uint32Items();
    if (!confirmedRootOwnerItems || confirmedRootOwnerItems.value().size() != 1U) {
        return connection.healthy() ? reduced(EwmhDiscoveryStatus::malformed) : transportFailure();
    }
    auto confirmedSupportingWindow =
        WindowId::fromProtocol(confirmedRootOwnerItems.value().front());
    if (!confirmedSupportingWindow ||
        confirmedSupportingWindow.value() != supportingWindow.value()) {
        return connection.healthy() ? reduced(EwmhDiscoveryStatus::malformed) : transportFailure();
    }

    return Result<EwmhCapabilities>::success(EwmhCapabilities{
        EwmhDiscoveryStatus::verified, capabilityFlags(supportedAtoms, atoms.value())});
}

Result<EwmhCapabilities> discoverEwmhCapabilities(X11Connection &connection) {
    return detail::discoverEwmhCapabilitiesWithInterleave(connection, nullptr, nullptr);
}

} // namespace prismdrake::x11
