#include "RootEventStream.hpp"

#include "RandrTopology.hpp"
#include "X11ConnectionPrivate.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#include <xcb/randr.h>
#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct EventDeleter final {
    void operator()(void *event) const noexcept { std::free(event); }
};

using EventHandle = std::unique_ptr<xcb_generic_event_t, EventDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, EventDeleter>;
using WindowAttributes = std::unique_ptr<xcb_get_window_attributes_reply_t, EventDeleter>;

static_assert(rootEventSelectionMask ==
              (XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
               XCB_EVENT_MASK_PROPERTY_CHANGE));
static_assert((rootEventSelectionMask & XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT) == 0U);
static_assert(syntheticEventBit == 0x80U);
static_assert(coreEventTypeMask == 0x7fU);
static_assert(createNotifyEventType == XCB_CREATE_NOTIFY);
static_assert(destroyNotifyEventType == XCB_DESTROY_NOTIFY);
static_assert(configureNotifyEventType == XCB_CONFIGURE_NOTIFY);
static_assert(propertyNotifyEventType == XCB_PROPERTY_NOTIFY);

[[nodiscard]] Result<std::optional<RootEvent>> ignoredEvent() {
    return Result<std::optional<RootEvent>>::success(std::nullopt);
}

[[nodiscard]] Result<std::optional<RootEvent>> malformedEvent() {
    return Result<std::optional<RootEvent>>::failure(
        {ErrorCode::validation_error, "An X11 root event is malformed.",
         "Discard the event and refresh state only from validated protocol properties."});
}

[[nodiscard]] Result<RootEventStream> streamFailure() {
    return Result<RootEventStream>::failure(
        {ErrorCode::io_error, "The X11 root event subscription failed.",
         "Reconnect to the development X server and rebuild mirrored state."});
}

[[nodiscard]] Result<RootEventStream> provenanceFailure() {
    return Result<RootEventStream>::failure(
        {ErrorCode::invalid_argument,
         "The RandR protocol and root event stream connection do not match.",
         "Negotiate RandR and create the event stream on the same X11 connection."});
}

[[nodiscard]] Result<RootEventBatch> drainFailure() {
    return Result<RootEventBatch>::failure(
        {ErrorCode::io_error, "The X11 root event stream failed.",
         "Reconnect to the development X server and rebuild mirrored state."});
}

[[nodiscard]] bool expectedResponseType(std::uint8_t responseType, std::uint8_t expected) noexcept {
    return (responseType & coreEventTypeMask) == expected;
}

[[nodiscard]] bool synthetic(std::uint8_t responseType) noexcept {
    return (responseType & syntheticEventBit) != 0U;
}

[[nodiscard]] Result<std::optional<RootEvent>> decodeCreate(const CreateNotifyFields &fields,
                                                            WindowId root) {
    if (!expectedResponseType(fields.responseType, createNotifyEventType)) {
        return malformedEvent();
    }
    if (fields.parent != root.value()) {
        return ignoredEvent();
    }
    if (fields.window == 0U || fields.window == root.value() || fields.width == 0U ||
        fields.height == 0U) {
        return malformedEvent();
    }
    auto window = WindowId::fromProtocol(fields.window);
    if (!window) {
        return malformedEvent();
    }
    return Result<std::optional<RootEvent>>::success(RootEvent{ClientTopologyHint{
        window.value(), ClientTopologyChange::created, synthetic(fields.responseType)}});
}

[[nodiscard]] Result<std::optional<RootEvent>> decodeDestroy(const DestroyNotifyFields &fields,
                                                             WindowId root) {
    if (!expectedResponseType(fields.responseType, destroyNotifyEventType)) {
        return malformedEvent();
    }
    if (fields.eventWindow != root.value()) {
        return ignoredEvent();
    }
    if (fields.window == 0U || fields.window == root.value()) {
        return malformedEvent();
    }
    auto window = WindowId::fromProtocol(fields.window);
    if (!window) {
        return malformedEvent();
    }
    return Result<std::optional<RootEvent>>::success(RootEvent{ClientTopologyHint{
        window.value(), ClientTopologyChange::destroyed, synthetic(fields.responseType)}});
}

[[nodiscard]] Result<std::optional<RootEvent>> decodeConfigure(const ConfigureNotifyFields &fields,
                                                               WindowId root) {
    if (!expectedResponseType(fields.responseType, configureNotifyEventType)) {
        return malformedEvent();
    }
    if (fields.eventWindow != root.value() || fields.window != root.value()) {
        return ignoredEvent();
    }
    if (fields.width == 0U || fields.height == 0U) {
        return malformedEvent();
    }
    return Result<std::optional<RootEvent>>::success(
        RootEvent{RootGeometryHint{synthetic(fields.responseType)}});
}

[[nodiscard]] Result<std::optional<RootEvent>> decodeProperty(const PropertyNotifyFields &fields,
                                                              WindowId root) {
    if (!expectedResponseType(fields.responseType, propertyNotifyEventType)) {
        return malformedEvent();
    }
    if (!fields.atom ||
        (fields.state != XCB_PROPERTY_NEW_VALUE && fields.state != XCB_PROPERTY_DELETE)) {
        return malformedEvent();
    }
    const auto state = fields.state == XCB_PROPERTY_NEW_VALUE ? RootPropertyState::newValue
                                                              : RootPropertyState::deleted;
    if (fields.window == root.value()) {
        return Result<std::optional<RootEvent>>::success(RootEvent{
            RootPropertyHint{fields.atom.value(), state, synthetic(fields.responseType)}});
    }
    auto window = WindowId::fromProtocol(fields.window);
    if (!window) {
        return malformedEvent();
    }
    return Result<std::optional<RootEvent>>::success(RootEvent{ClientPropertyHint{
        window.value(), fields.atom.value(), state, synthetic(fields.responseType)}});
}

[[nodiscard]] Result<std::optional<RootEvent>>
decodeProtocolError(const ProtocolErrorFields &fields) {
    if (fields.responseType != 0U || fields.errorCode == 0U) {
        return malformedEvent();
    }
    return Result<std::optional<RootEvent>>::success(RootEvent{ProtocolErrorHint{}});
}

[[nodiscard]] CoreRootEventFields copyCreate(const xcb_generic_event_t &event) {
    xcb_create_notify_event_t value{};
    static_assert(sizeof(value) <= sizeof(event));
    std::memcpy(&value, &event, sizeof(value));
    return CreateNotifyFields{value.response_type,
                              value.parent,
                              value.window,
                              value.x,
                              value.y,
                              value.width,
                              value.height,
                              value.border_width,
                              value.override_redirect != 0U};
}

[[nodiscard]] CoreRootEventFields copyDestroy(const xcb_generic_event_t &event) {
    xcb_destroy_notify_event_t value{};
    static_assert(sizeof(value) <= sizeof(event));
    std::memcpy(&value, &event, sizeof(value));
    return DestroyNotifyFields{value.response_type, value.event, value.window};
}

[[nodiscard]] CoreRootEventFields copyConfigure(const xcb_generic_event_t &event) {
    xcb_configure_notify_event_t value{};
    static_assert(sizeof(value) <= sizeof(event));
    std::memcpy(&value, &event, sizeof(value));
    return ConfigureNotifyFields{
        value.response_type, value.event,  value.window,      value.x, value.y,
        value.width,         value.height, value.border_width};
}

[[nodiscard]] CoreRootEventFields copyProperty(const xcb_generic_event_t &event) {
    xcb_property_notify_event_t value{};
    static_assert(sizeof(value) <= sizeof(event));
    std::memcpy(&value, &event, sizeof(value));
    std::optional<AtomId> atom;
    auto convertedAtom = AtomId::fromProtocol(value.atom);
    if (convertedAtom) {
        atom = convertedAtom.value();
    }
    return PropertyNotifyFields{value.response_type, value.window, atom, value.state};
}

[[nodiscard]] CoreRootEventFields copyProtocolError(const xcb_generic_event_t &event) {
    xcb_generic_error_t value{};
    static_assert(sizeof(value) <= sizeof(event));
    std::memcpy(&value, &event, sizeof(value));
    return ProtocolErrorFields{value.response_type, value.error_code, value.major_code,
                               value.minor_code};
}

[[nodiscard]] RandrEventFields copyRandrEvent(const xcb_generic_event_t &event) noexcept {
    std::uint8_t fields[2]{};
    static_assert(sizeof(fields) <= sizeof(event));
    std::memcpy(fields, &event, sizeof(fields));
    return RandrEventFields{fields[0], fields[1]};
}

} // namespace

Result<std::optional<RootEvent>> decodeRootEvent(const CoreRootEventFields &fields, WindowId root) {
    return std::visit(
        [root](const auto &event) -> Result<std::optional<RootEvent>> {
            using Event = std::remove_cvref_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, CreateNotifyFields>) {
                return decodeCreate(event, root);
            } else if constexpr (std::is_same_v<Event, DestroyNotifyFields>) {
                return decodeDestroy(event, root);
            } else if constexpr (std::is_same_v<Event, ConfigureNotifyFields>) {
                return decodeConfigure(event, root);
            } else if constexpr (std::is_same_v<Event, PropertyNotifyFields>) {
                return decodeProperty(event, root);
            } else {
                return decodeProtocolError(event);
            }
        },
        fields);
}

Result<RootEventStream> RootEventStream::create(X11Connection &connection) {
    if (!connection.healthy()) {
        return streamFailure();
    }
    auto implementation = connection.implementation_;
    if (!implementation || !implementation->tryAcquireRootEventStream()) {
        return streamFailure();
    }
    auto *native = implementation->connection_.get();
    if (native == nullptr) {
        implementation->releaseRootEventStream();
        return streamFailure();
    }

    xcb_generic_error_t *attributesError = nullptr;
    const auto attributesCookie =
        xcb_get_window_attributes(native, implementation->screen_.rootWindow.value());
    WindowAttributes attributes{
        xcb_get_window_attributes_reply(native, attributesCookie, &attributesError)};
    ProtocolError attributesProtocolError{attributesError};
    if (attributesProtocolError || !attributes || xcb_connection_has_error(native) != 0) {
        implementation->releaseRootEventStream();
        return streamFailure();
    }

    // Preserve subscriptions already owned by this connection, but never acquire or perpetuate
    // window-manager redirect authority through the observational adapter.
    if ((attributes->your_event_mask & XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT) != 0U) {
        implementation->releaseRootEventStream();
        return streamFailure();
    }
    const std::uint32_t addedMask = rootEventSelectionMask & ~attributes->your_event_mask;
    const std::uint32_t mask = attributes->your_event_mask | rootEventSelectionMask;
    const auto cookie = xcb_change_window_attributes_checked(
        native, implementation->screen_.rootWindow.value(), XCB_CW_EVENT_MASK, &mask);
    ProtocolError error{xcb_request_check(native, cookie)};
    if (error || xcb_connection_has_error(native) != 0) {
        implementation->releaseRootEventStream();
        return streamFailure();
    }
    return Result<RootEventStream>::success(RootEventStream{std::move(implementation), addedMask});
}

Result<RootEventStream> RootEventStream::create(X11Connection &connection,
                                                const RandrTopologyProtocol &randr) {
    if (!randr.belongsTo(connection.identity())) {
        return provenanceFailure();
    }

    auto stream = create(connection);
    if (!stream) {
        return Result<RootEventStream>::failure(stream.error());
    }
    auto selected = randr.selectTopologyEvents(connection);
    if (!selected) {
        return Result<RootEventStream>::failure(selected.error());
    }
    if (selected.value()) {
        stream.value().randrFirstEvent_ = randr.first_event_;
        stream.value().randrSupportsResourceChange_ =
            randr.status_ == RandrTopologyStatus::randr_1_4;
        stream.value().randrEventsSelected_ = true;
    }
    return stream;
}

RootEventStream::RootEventStream(RootEventStream &&other) noexcept
    : implementation_(std::move(other.implementation_)),
      addedMask_(std::exchange(other.addedMask_, 0U)),
      randrFirstEvent_(std::exchange(other.randrFirstEvent_, 0U)),
      randrSupportsResourceChange_(std::exchange(other.randrSupportsResourceChange_, false)),
      randrEventsSelected_(std::exchange(other.randrEventsSelected_, false)) {}

RootEventStream::~RootEventStream() { releaseSubscription(); }

void RootEventStream::releaseSubscription() noexcept {
    if (!implementation_) {
        return;
    }

    auto *native = implementation_->connection_.get();
    if (native != nullptr && randrEventsSelected_ && xcb_connection_has_error(native) == 0) {
        const auto cookie =
            xcb_randr_select_input_checked(native, implementation_->screen_.rootWindow.value(), 0U);
        ProtocolError error{xcb_request_check(native, cookie)};
        (void)error;
    }
    if (native != nullptr && addedMask_ != 0U && xcb_connection_has_error(native) == 0) {
        xcb_generic_error_t *attributesError = nullptr;
        const auto attributesCookie =
            xcb_get_window_attributes(native, implementation_->screen_.rootWindow.value());
        WindowAttributes attributes{
            xcb_get_window_attributes_reply(native, attributesCookie, &attributesError)};
        ProtocolError attributesProtocolError{attributesError};
        if (!attributesProtocolError && attributes && xcb_connection_has_error(native) == 0) {
            const std::uint32_t mask = attributes->your_event_mask & ~addedMask_;
            const auto cookie = xcb_change_window_attributes_checked(
                native, implementation_->screen_.rootWindow.value(), XCB_CW_EVENT_MASK, &mask);
            ProtocolError error{xcb_request_check(native, cookie)};
            (void)error;
        }
    }
    implementation_->releaseRootEventStream();
    implementation_.reset();
    addedMask_ = 0U;
    randrFirstEvent_ = 0U;
    randrSupportsResourceChange_ = false;
    randrEventsSelected_ = false;
}

Result<RootEventBatch> RootEventStream::drain(std::size_t examinationLimit) {
    if (!implementation_ || examinationLimit == 0U ||
        examinationLimit > maximumRootEventsPerDrain) {
        return Result<RootEventBatch>::failure(
            {ErrorCode::invalid_argument, "The X11 root event drain limit is invalid.",
             "Use a nonzero limit no larger than the documented event budget."});
    }
    auto *native = implementation_->connection_.get();
    if (native == nullptr || xcb_connection_has_error(native) != 0) {
        return drainFailure();
    }

    RootEventBatch batch;
    batch.events.reserve(std::min(examinationLimit, std::size_t{32U}));
    std::size_t examined = 0U;
    while (examined < examinationLimit) {
        EventHandle event{xcb_poll_for_event(native)};
        if (!event) {
            break;
        }
        ++examined;
        const std::uint8_t eventType = event->response_type & coreEventTypeMask;
        Result<std::optional<RootEvent>> decoded = ignoredEvent();
        switch (eventType) {
        case 0U:
            decoded =
                decodeRootEvent(copyProtocolError(*event), implementation_->screen_.rootWindow);
            break;
        case createNotifyEventType:
            decoded = decodeRootEvent(copyCreate(*event), implementation_->screen_.rootWindow);
            break;
        case destroyNotifyEventType:
            decoded = decodeRootEvent(copyDestroy(*event), implementation_->screen_.rootWindow);
            break;
        case configureNotifyEventType:
            decoded = decodeRootEvent(copyConfigure(*event), implementation_->screen_.rootWindow);
            break;
        case propertyNotifyEventType:
            decoded = decodeRootEvent(copyProperty(*event), implementation_->screen_.rootWindow);
            break;
        default:
            if (randrEventsSelected_ &&
                randrEventRequiresFullRequery(randrFirstEvent_, copyRandrEvent(*event),
                                              randrSupportsResourceChange_)) {
                decoded = Result<std::optional<RootEvent>>::success(
                    RootEvent{OutputTopologyRefreshHint{synthetic(event->response_type)}});
                break;
            }
            continue;
        }

        if (!decoded) {
            return Result<RootEventBatch>::failure(decoded.error());
        }
        auto decodedEvent = std::move(decoded).value();
        if (decodedEvent) {
            batch.events.push_back(std::move(decodedEvent).value());
        }
    }
    if (xcb_connection_has_error(native) != 0) {
        return drainFailure();
    }
    batch.examinationLimitReached = examined == examinationLimit;
    return Result<RootEventBatch>::success(std::move(batch));
}

} // namespace prismdrake::x11
