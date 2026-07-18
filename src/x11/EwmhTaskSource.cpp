#include "EwmhTaskSource.hpp"

#include "EwmhCapabilities.hpp"
#include "PropertyReader.hpp"
#include "X11Connection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view changingRootSnapshotMessage =
    "The mandatory EWMH task membership changed during one observation.";
constexpr std::string_view ownerUnavailableMessage =
    "A verified EWMH task-list owner is unavailable.";
constexpr std::string_view ownerMalformedMessage =
    "The EWMH task-list owner property is malformed.";
constexpr std::string_view capabilitiesMalformedMessage =
    "The EWMH task-list owner contract is malformed.";
constexpr std::string_view clientListUnsupportedMessage =
    "The verified EWMH owner does not advertise a client list.";
constexpr std::string_view clientListUnavailableMessage =
    "The mandatory EWMH client list is unavailable.";
constexpr std::string_view clientListMalformedMessage =
    "The mandatory EWMH client list is malformed.";
constexpr std::string_view stackingMalformedMessage =
    "The optional EWMH stacking list is malformed.";
constexpr std::string_view activeWindowMalformedMessage =
    "The optional EWMH active-window property is malformed.";
constexpr std::string_view ownerChangedMessage =
    "The EWMH task-list owner changed during one observation.";

struct FreeDeleter final {
    void operator()(void *value) const noexcept { std::free(value); }
};

using AttributesReply = std::unique_ptr<xcb_get_window_attributes_reply_t, FreeDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

template <typename Value> [[nodiscard]] Result<Value> invalidSource() {
    return Result<Value>::failure(
        {ErrorCode::invalid_argument, "The EWMH task source does not match the X11 connection.",
         "Recreate the task source after reconnecting to the development X server."});
}

template <typename Value> [[nodiscard]] Result<Value> transportFailure() {
    return Result<Value>::failure(
        {ErrorCode::io_error, "The X11 connection failed while refreshing the task mirror.",
         "Reconnect and rebuild the complete task snapshot from the window manager."});
}

template <typename Value> [[nodiscard]] Result<Value> unavailableWindowManager() {
    return Result<Value>::failure(
        {ErrorCode::not_found, std::string{ownerUnavailableMessage},
         "Retain the previous snapshot and retry after the window-manager owner changes."});
}

template <typename Value> [[nodiscard]] Result<Value> malformedWindowManagerCapabilities() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, std::string{capabilitiesMalformedMessage},
         "Retain the previous snapshot until a valid window-manager owner replaces it."});
}

template <typename Value> [[nodiscard]] Result<Value> unsupportedClientList() {
    return Result<Value>::failure({ErrorCode::unsupported,
                                   std::string{clientListUnsupportedMessage},
                                   "Keep task presentation unavailable with this window manager."});
}

[[nodiscard]] foundation::Error ownerReadFailure(const foundation::Error &error) {
    return {error.code,
            std::string{error.code == ErrorCode::not_found ? ownerUnavailableMessage
                                                           : ownerMalformedMessage},
            "Retain the previous snapshot until a valid owner property is available."};
}

[[nodiscard]] foundation::Error clientListReadFailure(const foundation::Error &error) {
    return {error.code,
            std::string{error.code == ErrorCode::not_found ? clientListUnavailableMessage
                                                           : clientListMalformedMessage},
            "Retain the previous snapshot until a valid mandatory client list is available."};
}

[[nodiscard]] foundation::Error stackingReadFailure(const foundation::Error &error) {
    return {error.code, std::string{stackingMalformedMessage},
            "Reject the malformed optional property and retain the previous snapshot."};
}

[[nodiscard]] foundation::Error activeWindowReadFailure(const foundation::Error &error) {
    return {error.code, std::string{activeWindowMalformedMessage},
            "Reject the malformed optional property and retain the previous snapshot."};
}

template <typename Value> [[nodiscard]] Result<Value> ownerChangedFailure() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, std::string{ownerChangedMessage},
         "Retain the previous snapshot while a bounded event-loop retry observes one owner."});
}

template <typename Value> [[nodiscard]] Result<Value> malformedRootSnapshot() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "The EWMH root task snapshot changed or is malformed.",
         "Retain the previous snapshot and retry one complete bounded refresh."});
}

template <typename Value> [[nodiscard]] Result<Value> changingRootSnapshot() {
    return Result<Value>::failure({ErrorCode::validation_error,
                                   std::string{changingRootSnapshotMessage},
                                   "Retain the previous task snapshot while a bounded event-loop "
                                   "retry stabilizes membership."});
}

template <typename Value> [[nodiscard]] Result<Value> exhaustedIncarnations() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "The task-source incarnation identifier space is exhausted.",
         "Reconnect and rebuild task state with a fresh source instance."});
}

[[nodiscard]] Result<std::optional<PropertyValue>> readOptional(X11Connection &connection,
                                                                const AtomCache &atoms,
                                                                WindowId window, AtomName property,
                                                                const PropertySpec &spec) {
    auto value = PropertyReader::read(connection, atoms, window, property, spec);
    if (value) {
        return Result<std::optional<PropertyValue>>::success(
            std::optional<PropertyValue>{std::move(value).value()});
    }
    if (value.error().code == ErrorCode::not_found) {
        return Result<std::optional<PropertyValue>>::success(std::nullopt);
    }
    return Result<std::optional<PropertyValue>>::failure(value.error());
}

[[nodiscard]] Result<std::vector<std::uint32_t>> requiredWords(PropertyValue &value) {
    auto words = value.uint32Items();
    if (!words) {
        return Result<std::vector<std::uint32_t>>::failure(words.error());
    }
    return words;
}

[[nodiscard]] Result<std::optional<std::vector<std::uint32_t>>>
optionalWords(std::optional<PropertyValue> &value) {
    if (!value) {
        return Result<std::optional<std::vector<std::uint32_t>>>::success(std::nullopt);
    }
    auto words = value->uint32Items();
    if (!words) {
        return Result<std::optional<std::vector<std::uint32_t>>>::failure(words.error());
    }
    return Result<std::optional<std::vector<std::uint32_t>>>::success(
        std::optional<std::vector<std::uint32_t>>{std::move(words).value()});
}

[[nodiscard]] std::string copyString(const PropertyValue &value) {
    const auto bytes = value.bytes();
    std::string copied(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(copied.data(), bytes.data(), bytes.size());
    }
    return copied;
}

[[nodiscard]] std::vector<std::uint8_t> copyBytes(const PropertyValue &value) {
    const auto bytes = value.bytes();
    std::vector<std::uint8_t> copied(bytes.size());
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        copied[index] = std::to_integer<std::uint8_t>(bytes[index]);
    }
    return copied;
}

[[nodiscard]] Result<WindowId> readOwner(X11Connection &connection, const AtomCache &atoms) {
    constexpr PropertySpec ownerSpec{AtomName::window, PropertyFormat::bits_32, 1U,
                                     sizeof(std::uint32_t)};
    auto value = PropertyReader::read(connection, atoms, connection.screen().rootWindow,
                                      AtomName::net_supporting_wm_check, ownerSpec);
    if (!value) {
        return Result<WindowId>::failure(ownerReadFailure(value.error()));
    }
    auto words = value.value().uint32Items();
    if (!words || words.value().size() != 1U) {
        return Result<WindowId>::failure(
            {ErrorCode::validation_error, std::string{ownerMalformedMessage},
             "Retain the previous snapshot until a valid owner property is available."});
    }
    auto owner = WindowId::fromProtocol(words.value().front());
    if (!owner) {
        return Result<WindowId>::failure(
            {ErrorCode::validation_error, std::string{ownerMalformedMessage},
             "Retain the previous snapshot until a valid owner property is available."});
    }
    return owner;
}

[[nodiscard]] Result<EwmhTaskListSnapshot>
readRootSnapshot(X11Connection &connection, const AtomCache &atoms, bool readActiveWindow) {
    constexpr PropertySpec listSpec{AtomName::window, PropertyFormat::bits_32,
                                    maximumEwmhTaskWindows,
                                    maximumEwmhTaskWindows * sizeof(std::uint32_t)};
    constexpr PropertySpec activeSpec{AtomName::window, PropertyFormat::bits_32, 1U,
                                      sizeof(std::uint32_t)};

    auto clients = PropertyReader::read(connection, atoms, connection.screen().rootWindow,
                                        AtomName::net_client_list, listSpec);
    if (!clients) {
        return Result<EwmhTaskListSnapshot>::failure(clientListReadFailure(clients.error()));
    }
    auto clientWords = requiredWords(clients.value());
    if (!clientWords) {
        return Result<EwmhTaskListSnapshot>::failure(clientWords.error());
    }

    auto stacking = readOptional(connection, atoms, connection.screen().rootWindow,
                                 AtomName::net_client_list_stacking, listSpec);
    if (!stacking) {
        return Result<EwmhTaskListSnapshot>::failure(stackingReadFailure(stacking.error()));
    }
    auto stackingWords = optionalWords(stacking.value());
    if (!stackingWords) {
        return Result<EwmhTaskListSnapshot>::failure(stackingWords.error());
    }

    std::optional<std::uint32_t> activeWindow;
    if (readActiveWindow) {
        auto active = readOptional(connection, atoms, connection.screen().rootWindow,
                                   AtomName::net_active_window, activeSpec);
        if (!active) {
            return Result<EwmhTaskListSnapshot>::failure(activeWindowReadFailure(active.error()));
        }
        auto activeWords = optionalWords(active.value());
        if (!activeWords) {
            return Result<EwmhTaskListSnapshot>::failure(activeWords.error());
        }
        if (activeWords.value()) {
            if (activeWords.value()->size() != 1U) {
                return Result<EwmhTaskListSnapshot>::failure(
                    {ErrorCode::validation_error, std::string{activeWindowMalformedMessage},
                     "Reject the malformed optional property and retain the previous snapshot."});
            }
            if (activeWords.value()->front() != XCB_WINDOW_NONE) {
                activeWindow = activeWords.value()->front();
            }
        }
    }

    return buildEwmhTaskListSnapshot(
        {std::move(clientWords).value(), std::move(stackingWords).value(), activeWindow});
}

[[nodiscard]] std::optional<WindowType> decodeWindowType(std::uint32_t raw,
                                                         const AtomCache &atoms) {
    constexpr std::array mappings{
        std::pair{AtomName::net_wm_window_type_normal, WindowType::normal},
        std::pair{AtomName::net_wm_window_type_dialog, WindowType::dialog},
        std::pair{AtomName::net_wm_window_type_utility, WindowType::utility},
        std::pair{AtomName::net_wm_window_type_toolbar, WindowType::toolbar},
        std::pair{AtomName::net_wm_window_type_menu, WindowType::menu},
        std::pair{AtomName::net_wm_window_type_splash, WindowType::splash},
        std::pair{AtomName::net_wm_window_type_dropdown_menu, WindowType::dropdownMenu},
        std::pair{AtomName::net_wm_window_type_popup_menu, WindowType::popupMenu},
        std::pair{AtomName::net_wm_window_type_tooltip, WindowType::tooltip},
        std::pair{AtomName::net_wm_window_type_notification, WindowType::notification},
        std::pair{AtomName::net_wm_window_type_combo, WindowType::combo},
        std::pair{AtomName::net_wm_window_type_dnd, WindowType::dragAndDrop},
        std::pair{AtomName::net_wm_window_type_dock, WindowType::dock},
        std::pair{AtomName::net_wm_window_type_desktop, WindowType::desktop},
    };
    for (const auto &[name, type] : mappings) {
        const auto atom = atoms.atom(name);
        if (atom && atom->value() == raw) {
            return type;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<WindowState> decodeWindowState(std::uint32_t raw,
                                                           const AtomCache &atoms) {
    constexpr std::array mappings{
        std::pair{AtomName::net_wm_state_hidden, WindowState::hidden},
        std::pair{AtomName::net_wm_state_fullscreen, WindowState::fullscreen},
        std::pair{AtomName::net_wm_state_modal, WindowState::modal},
        std::pair{AtomName::net_wm_state_skip_taskbar, WindowState::skipTaskbar},
        std::pair{AtomName::net_wm_state_demands_attention, WindowState::demandsAttention},
    };
    for (const auto &[name, state] : mappings) {
        const auto atom = atoms.atom(name);
        if (atom && atom->value() == raw) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Result<WindowMetadata> readMetadataStrict(X11Connection &connection,
                                                        const AtomCache &atoms, WindowId window) {
    constexpr PropertySpec utf8TitleSpec{AtomName::utf8_string, PropertyFormat::bits_8,
                                         maximumWindowTitleBytes, maximumWindowTitleBytes};
    constexpr PropertySpec legacyTitleSpec{AtomName::string, PropertyFormat::bits_8,
                                           maximumWindowTitleBytes / 2U,
                                           maximumWindowTitleBytes / 2U};
    constexpr PropertySpec classSpec{AtomName::string, PropertyFormat::bits_8, maximumWmClassBytes,
                                     maximumWmClassBytes};
    constexpr PropertySpec typeSpec{AtomName::atom, PropertyFormat::bits_32, maximumWindowTypes,
                                    maximumWindowTypes * sizeof(std::uint32_t)};
    constexpr PropertySpec stateSpec{AtomName::atom, PropertyFormat::bits_32, maximumWindowStates,
                                     maximumWindowStates * sizeof(std::uint32_t)};
    constexpr PropertySpec cardinalSpec{AtomName::cardinal, PropertyFormat::bits_32, 1U,
                                        sizeof(std::uint32_t)};
    constexpr PropertySpec hintsSpec{AtomName::wm_hints, PropertyFormat::bits_32, 9U,
                                     9U * sizeof(std::uint32_t)};
    constexpr PropertySpec windowSpec{AtomName::window, PropertyFormat::bits_32, 1U,
                                      sizeof(std::uint32_t)};
    constexpr PropertySpec iconSpec{AtomName::cardinal, PropertyFormat::bits_32,
                                    maximumNetWmIconBytes / sizeof(std::uint32_t),
                                    maximumNetWmIconBytes};

    auto utf8Title = readOptional(connection, atoms, window, AtomName::net_wm_name, utf8TitleSpec);
    auto legacyTitle = readOptional(connection, atoms, window, AtomName::wm_name, legacyTitleSpec);
    auto windowClass = readOptional(connection, atoms, window, AtomName::wm_class, classSpec);
    auto types = readOptional(connection, atoms, window, AtomName::net_wm_window_type, typeSpec);
    auto states = readOptional(connection, atoms, window, AtomName::net_wm_state, stateSpec);
    auto workspace =
        readOptional(connection, atoms, window, AtomName::net_wm_desktop, cardinalSpec);
    auto hints = readOptional(connection, atoms, window, AtomName::wm_hints, hintsSpec);
    auto transient =
        readOptional(connection, atoms, window, AtomName::wm_transient_for, windowSpec);
    auto icon = readOptional(connection, atoms, window, AtomName::net_wm_icon, iconSpec);
    const std::array<const Result<std::optional<PropertyValue>> *, 9U> reads{
        &utf8Title, &legacyTitle, &windowClass, &types, &states,
        &workspace, &hints,       &transient,   &icon,
    };
    for (const auto *read : reads) {
        if (!*read) {
            return Result<WindowMetadata>::failure(read->error());
        }
    }

    WindowMetadataObservation observation{window,       std::nullopt, std::nullopt, std::nullopt,
                                          {},           {},           std::nullopt, std::nullopt,
                                          std::nullopt, std::nullopt};
    if (utf8Title.value()) {
        observation.utf8Title = copyString(utf8Title.value().value());
    }
    if (legacyTitle.value()) {
        observation.legacyTitle = copyString(legacyTitle.value().value());
    }
    if (windowClass.value()) {
        observation.wmClass = copyBytes(windowClass.value().value());
    }

    auto typeWords = optionalWords(types.value());
    auto stateWords = optionalWords(states.value());
    auto workspaceWords = optionalWords(workspace.value());
    auto hintWords = optionalWords(hints.value());
    auto transientWords = optionalWords(transient.value());
    auto iconWords = optionalWords(icon.value());
    if (!typeWords || !stateWords || !workspaceWords || !hintWords || !transientWords ||
        !iconWords) {
        return Result<WindowMetadata>::failure(!typeWords        ? typeWords.error()
                                               : !stateWords     ? stateWords.error()
                                               : !workspaceWords ? workspaceWords.error()
                                               : !hintWords      ? hintWords.error()
                                               : !transientWords ? transientWords.error()
                                                                 : iconWords.error());
    }
    if (typeWords.value()) {
        for (const auto raw : typeWords.value().value()) {
            if (const auto type = decodeWindowType(raw, atoms)) {
                observation.windowTypes.push_back(type.value());
            }
        }
    }
    if (stateWords.value()) {
        for (const auto raw : stateWords.value().value()) {
            if (const auto state = decodeWindowState(raw, atoms)) {
                observation.windowStates.push_back(state.value());
            }
        }
    }
    if (workspaceWords.value()) {
        if (workspaceWords.value()->size() != 1U) {
            return malformedRootSnapshot<WindowMetadata>();
        }
        observation.workspace = workspaceWords.value()->front();
    }
    if (hintWords.value()) {
        if (hintWords.value()->empty()) {
            return malformedRootSnapshot<WindowMetadata>();
        }
        observation.wmHintsFlags = hintWords.value()->front();
    }
    if (transientWords.value()) {
        if (transientWords.value()->size() != 1U) {
            return malformedRootSnapshot<WindowMetadata>();
        }
        auto transientWindow = WindowId::fromProtocol(transientWords.value()->front());
        if (!transientWindow) {
            return Result<WindowMetadata>::failure(transientWindow.error());
        }
        observation.transientFor = transientWindow.value();
    }
    observation.netWmIcon = std::move(iconWords).value();

    return decodeWindowMetadata(observation);
}

[[nodiscard]] Result<std::optional<WindowMetadata>>
readMetadata(X11Connection &connection, const AtomCache &atoms, WindowId window) {
    auto metadata = readMetadataStrict(connection, atoms, window);
    if (metadata) {
        metadata.value().icons.clear();
        return Result<std::optional<WindowMetadata>>::success(
            std::optional<WindowMetadata>{std::move(metadata).value()});
    }
    if (!connection.healthy()) {
        return transportFailure<std::optional<WindowMetadata>>();
    }
    return Result<std::optional<WindowMetadata>>::success(std::nullopt);
}

} // namespace

Result<EwmhTaskSource> EwmhTaskSource::create(X11Connection &connection) {
    if (!connection.healthy() || connection.identity() == 0U) {
        return invalidSource<EwmhTaskSource>();
    }
    auto atoms = AtomCache::create(connection);
    if (!atoms) {
        return Result<EwmhTaskSource>::failure(atoms.error());
    }
    return Result<EwmhTaskSource>::success(
        EwmhTaskSource{std::move(atoms).value(), connection.identity()});
}

EwmhTaskSource::EwmhTaskSource(EwmhTaskSource &&other) noexcept
    : atoms_(std::move(other.atoms_)), connection_identity_(other.connection_identity_),
      current_owner_(other.current_owner_), incarnations_(std::move(other.incarnations_)),
      next_incarnation_(other.next_incarnation_) {
    other.connection_identity_ = 0U;
    other.current_owner_.reset();
    other.incarnations_.clear();
    other.next_incarnation_ = 0U;
}

Result<bool> EwmhTaskSource::selectClientPropertyEvents(X11Connection &connection,
                                                        WindowId window) {
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    xcb_generic_error_t *rawError = nullptr;
    AttributesReply attributes{xcb_get_window_attributes_reply(
        native, xcb_get_window_attributes(native, window.value()), &rawError)};
    ProtocolError error{rawError};
    if (!attributes || error) {
        if (!connection.healthy()) {
            return transportFailure<bool>();
        }
        return Result<bool>::success(false);
    }
    if (attributes->override_redirect != 0U) {
        return Result<bool>::success(false);
    }
    const std::uint32_t selected = attributes->your_event_mask | XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (selected != attributes->your_event_mask) {
        const auto cookie = xcb_change_window_attributes_checked(native, window.value(),
                                                                 XCB_CW_EVENT_MASK, &selected);
        ProtocolError selectError{xcb_request_check(native, cookie)};
        if (selectError) {
            if (!connection.healthy()) {
                return transportFailure<bool>();
            }
            return Result<bool>::success(false);
        }
    }
    return Result<bool>::success(true);
}

Result<TaskModelObservation> EwmhTaskSource::refresh(X11Connection &connection) {
    if (!connection.healthy() || connection.identity() != connection_identity_) {
        return invalidSource<TaskModelObservation>();
    }

    bool observedOwnerChange = false;
    bool observedClientListChange = false;
    for (std::size_t attempt = 0U; taskSnapshotAttemptAllowed(attempt); ++attempt) {
        const auto capabilities = discoverEwmhCapabilities(connection);
        if (!capabilities) {
            return Result<TaskModelObservation>::failure(capabilities.error());
        }
        if (capabilities.value().status == EwmhDiscoveryStatus::unavailable) {
            return unavailableWindowManager<TaskModelObservation>();
        }
        if (capabilities.value().status == EwmhDiscoveryStatus::malformed) {
            return malformedWindowManagerCapabilities<TaskModelObservation>();
        }
        if (!capabilities.value().flags.clientList) {
            return unsupportedClientList<TaskModelObservation>();
        }

        auto owner = readOwner(connection, atoms_);
        auto initial =
            readRootSnapshot(connection, atoms_, capabilities.value().flags.activeWindow);
        if (!owner || !initial) {
            if (!connection.healthy()) {
                return transportFailure<TaskModelObservation>();
            }
            const auto &error = !owner ? owner.error() : initial.error();
            return Result<TaskModelObservation>::failure(error);
        }

        auto candidateIncarnations = incarnations_;
        auto candidateNextIncarnation = next_incarnation_;
        if (!current_owner_ || current_owner_.value() != owner.value()) {
            candidateIncarnations.clear();
        }

        std::vector<DecodedTaskObservation> windows;
        windows.reserve(initial.value().clientList().size());
        bool exhausted = false;
        for (const auto window : initial.value().clientList()) {
            if (window == connection.screen().rootWindow) {
                return Result<TaskModelObservation>::failure(
                    {ErrorCode::validation_error, std::string{clientListMalformedMessage},
                     "Reject the malformed mandatory list and retain the previous snapshot."});
            }
            auto incarnation = candidateIncarnations.find(window.value());
            if (incarnation == candidateIncarnations.end()) {
                if (candidateNextIncarnation == 0U) {
                    exhausted = true;
                    break;
                }
                auto observed = WindowIncarnationId::fromObserved(candidateNextIncarnation);
                if (!observed) {
                    exhausted = true;
                    break;
                }
                incarnation = candidateIncarnations.emplace(window.value(), observed.value()).first;
                candidateNextIncarnation =
                    candidateNextIncarnation == std::numeric_limits<std::uint64_t>::max()
                        ? 0U
                        : candidateNextIncarnation + 1U;
            }
            auto selected = selectClientPropertyEvents(connection, window);
            if (!selected) {
                return Result<TaskModelObservation>::failure(selected.error());
            }
            auto metadata = selected.value()
                                ? readMetadata(connection, atoms_, window)
                                : Result<std::optional<WindowMetadata>>::success(std::nullopt);
            if (!metadata) {
                return Result<TaskModelObservation>::failure(metadata.error());
            }
            auto stillReadable = selectClientPropertyEvents(connection, window);
            if (!stillReadable) {
                return Result<TaskModelObservation>::failure(stillReadable.error());
            }
            if (!stillReadable.value()) {
                metadata = Result<std::optional<WindowMetadata>>::success(std::nullopt);
            }
            const bool hasMetadata = metadata.value().has_value();
            windows.push_back({window, incarnation->second, std::move(metadata).value(),
                               hasMetadata ? "application-x-executable" : ""});
        }
        if (exhausted) {
            return exhaustedIncarnations<TaskModelObservation>();
        }

        auto confirmedOwner = readOwner(connection, atoms_);
        auto confirmed =
            readRootSnapshot(connection, atoms_, capabilities.value().flags.activeWindow);
        if (!confirmedOwner || !confirmed) {
            if (!connection.healthy()) {
                return transportFailure<TaskModelObservation>();
            }
            const auto &error = !confirmedOwner ? confirmedOwner.error() : confirmed.error();
            return Result<TaskModelObservation>::failure(error);
        }
        if (confirmedOwner.value() != owner.value()) {
            observedOwnerChange = true;
            continue;
        }
        if (!sameEwmhTaskMembership(initial.value(), confirmed.value())) {
            observedClientListChange = true;
            continue;
        }

        std::map<WindowId::Value, WindowIncarnationId> retained;
        for (const auto window : confirmed.value().clientList()) {
            const auto found = candidateIncarnations.find(window.value());
            if (found != candidateIncarnations.end()) {
                retained.emplace(found->first, found->second);
            }
        }
        current_owner_ = owner.value();
        incarnations_ = std::move(retained);
        next_incarnation_ = candidateNextIncarnation;
        return Result<TaskModelObservation>::success(
            TaskModelObservation{std::move(confirmed).value(), std::move(windows)});
    }
    return observedClientListChange || !observedOwnerChange
               ? changingRootSnapshot<TaskModelObservation>()
               : ownerChangedFailure<TaskModelObservation>();
}

void EwmhTaskSource::invalidateClient(WindowId window) noexcept {
    incarnations_.erase(window.value());
}

bool taskRefreshFailureCanStabilize(const foundation::Error &error) noexcept {
    const auto kind = classifyTaskRefreshFailure(error);
    return kind == EwmhTaskRefreshFailureKind::ownerUnavailable ||
           kind == EwmhTaskRefreshFailureKind::clientListUnavailable ||
           kind == EwmhTaskRefreshFailureKind::ownerChanged ||
           kind == EwmhTaskRefreshFailureKind::clientListChanged;
}

EwmhTaskRefreshFailureKind classifyTaskRefreshFailure(const foundation::Error &error) noexcept {
    if (error.code == ErrorCode::io_error) {
        return EwmhTaskRefreshFailureKind::transport;
    }
    const std::string_view message{error.message};
    if (message == ownerUnavailableMessage) {
        return EwmhTaskRefreshFailureKind::ownerUnavailable;
    }
    if (message == ownerMalformedMessage) {
        return EwmhTaskRefreshFailureKind::ownerMalformed;
    }
    if (message == capabilitiesMalformedMessage) {
        return EwmhTaskRefreshFailureKind::capabilitiesMalformed;
    }
    if (message == clientListUnsupportedMessage) {
        return EwmhTaskRefreshFailureKind::clientListUnsupported;
    }
    if (message == clientListUnavailableMessage) {
        return EwmhTaskRefreshFailureKind::clientListUnavailable;
    }
    if (message == clientListMalformedMessage ||
        message == "The mandatory EWMH client list exceeds its window bound.") {
        return EwmhTaskRefreshFailureKind::clientListMalformed;
    }
    if (message == stackingMalformedMessage ||
        message == "The optional EWMH stacking list exceeds its window bound.") {
        return EwmhTaskRefreshFailureKind::stackingMalformed;
    }
    if (message == activeWindowMalformedMessage) {
        return EwmhTaskRefreshFailureKind::activeWindowMalformed;
    }
    if (message == ownerChangedMessage) {
        return EwmhTaskRefreshFailureKind::ownerChanged;
    }
    if (message == changingRootSnapshotMessage) {
        return EwmhTaskRefreshFailureKind::clientListChanged;
    }
    return EwmhTaskRefreshFailureKind::other;
}

std::string_view ewmhTaskRefreshFailureKindId(EwmhTaskRefreshFailureKind kind) noexcept {
    switch (kind) {
    case EwmhTaskRefreshFailureKind::ownerUnavailable:
        return "owner_unavailable";
    case EwmhTaskRefreshFailureKind::ownerMalformed:
        return "owner_malformed";
    case EwmhTaskRefreshFailureKind::capabilitiesMalformed:
        return "capabilities_malformed";
    case EwmhTaskRefreshFailureKind::clientListUnsupported:
        return "client_list_unsupported";
    case EwmhTaskRefreshFailureKind::clientListUnavailable:
        return "client_list_unavailable";
    case EwmhTaskRefreshFailureKind::clientListMalformed:
        return "client_list_malformed";
    case EwmhTaskRefreshFailureKind::stackingMalformed:
        return "stacking_malformed";
    case EwmhTaskRefreshFailureKind::activeWindowMalformed:
        return "active_window_malformed";
    case EwmhTaskRefreshFailureKind::ownerChanged:
        return "owner_changed";
    case EwmhTaskRefreshFailureKind::clientListChanged:
        return "client_list_changed";
    case EwmhTaskRefreshFailureKind::transport:
        return "transport";
    case EwmhTaskRefreshFailureKind::other:
        return "other";
    }
    return "other";
}

} // namespace prismdrake::x11
