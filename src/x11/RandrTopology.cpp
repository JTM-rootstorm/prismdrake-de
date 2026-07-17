#include "RandrTopology.hpp"

#include "X11Connection.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <xcb/randr.h>
#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct ReplyDeleter final {
    void operator()(void *reply) const noexcept { std::free(reply); }
};

template <typename Reply> using ReplyHandle = std::unique_ptr<Reply, ReplyDeleter>;
using ProtocolError = ReplyHandle<xcb_generic_error_t>;
using GeometryReply = ReplyHandle<xcb_get_geometry_reply_t>;
using VersionReply = ReplyHandle<xcb_randr_query_version_reply_t>;
using Resources12Reply = ReplyHandle<xcb_randr_get_screen_resources_reply_t>;
using ResourcesCurrentReply = ReplyHandle<xcb_randr_get_screen_resources_current_reply_t>;
using OutputInfoReply = ReplyHandle<xcb_randr_get_output_info_reply_t>;
using CrtcInfoReply = ReplyHandle<xcb_randr_get_crtc_info_reply_t>;
using PrimaryReply = ReplyHandle<xcb_randr_get_output_primary_reply_t>;

static_assert(randrTopologyEventMask ==
              (XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
               XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE));
static_assert((randrTopologyEventMask & XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY) == 0U);
static_assert((randrTopologyEventMask & XCB_RANDR_NOTIFY_MASK_PROVIDER_CHANGE) == 0U);
static_assert((randrTopologyEventMask & XCB_RANDR_NOTIFY_MASK_PROVIDER_PROPERTY) == 0U);
static_assert((randrTopologyEventMask & XCB_RANDR_NOTIFY_MASK_LEASE) == 0U);

template <typename T> [[nodiscard]] Result<T> transportFailure() {
    return Result<T>::failure({ErrorCode::io_error,
                               "The X11 connection failed during RandR topology discovery.",
                               "Reconnect to the development X server and rebuild output state."});
}

template <typename T> [[nodiscard]] Result<T> malformedReply() {
    return Result<T>::failure(
        {ErrorCode::validation_error, "A RandR topology reply is malformed.",
         "Discard the complete RandR observation and use the checked core-root fallback."});
}

template <typename T> [[nodiscard]] Result<T> changedConfiguration() {
    return Result<T>::failure(
        {ErrorCode::cancelled, "The RandR configuration changed during topology discovery.",
         "Retry one complete topology query without publishing partial output state."});
}

template <typename T> [[nodiscard]] Result<T> invalidConnection() {
    return Result<T>::failure(
        {ErrorCode::invalid_argument, "The RandR protocol belongs to another X11 connection.",
         "Negotiate RandR again after reconnecting to the selected display."});
}

[[nodiscard]] bool replySize(std::uint32_t length, std::size_t minimumBytes,
                             std::size_t &totalBytes) noexcept {
    constexpr std::size_t headerBytes = 32U;
    constexpr std::size_t unitBytes = 4U;
    if (minimumBytes > maximumTopologyReplyBytes ||
        length > (maximumTopologyReplyBytes - headerBytes) / unitBytes) {
        return false;
    }
    totalBytes = headerBytes + static_cast<std::size_t>(length) * unitBytes;
    return totalBytes >= minimumBytes;
}

[[nodiscard]] bool replyLayoutMatches(int logicalBytes, std::size_t minimumBytes,
                                      std::size_t declaredBytes) noexcept {
    if (logicalBytes < 0) {
        return false;
    }
    const auto logicalSize = static_cast<std::size_t>(logicalBytes);
    return logicalSize >= minimumBytes && logicalSize <= declaredBytes &&
           declaredBytes - logicalSize < 4U;
}

template <typename Value>
[[nodiscard]] bool contains(const std::vector<Value> &values, Value value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

template <typename Value>
[[nodiscard]] bool contains(std::span<const Value> values, Value value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

template <typename Value>
[[nodiscard]] bool normalizeUniqueNonzero(std::span<Value> values) noexcept {
    std::sort(values.begin(), values.end());
    return (values.empty() || values.front() != 0U) &&
           std::adjacent_find(values.begin(), values.end()) == values.end();
}

template <typename Value>
[[nodiscard]] bool allContainedSorted(std::span<const Value> values,
                                      std::span<const Value> permitted) noexcept {
    return std::includes(permitted.begin(), permitted.end(), values.begin(), values.end());
}

template <typename Value>
[[nodiscard]] bool copyWireList(const Value *values, std::size_t count, std::vector<Value> &copy) {
    if (count == 0U) {
        copy.clear();
        return true;
    }
    if (values == nullptr) {
        return false;
    }
    copy.assign(values, values + count);
    return true;
}

template <typename Value>
[[nodiscard]] bool appendUniqueNonzero(std::vector<Value> &values, Value value) {
    if (value == 0U || contains(values, value)) {
        return false;
    }
    values.push_back(value);
    return true;
}

[[nodiscard]] Result<ScreenInfo> queryCoreScreen(X11Connection &connection,
                                                 xcb_connection_t *native) {
    xcb_generic_error_t *rawError = nullptr;
    GeometryReply reply{xcb_get_geometry_reply(
        native, xcb_get_geometry(native, connection.screen().rootWindow.value()), &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<ScreenInfo>();
    }
    if (error || !reply || reply->response_type != 1U || reply->length != 0U ||
        reply->root != connection.screen().rootWindow.value() || reply->width == 0U ||
        reply->height == 0U) {
        return malformedReply<ScreenInfo>();
    }
    return Result<ScreenInfo>::success(ScreenInfo{connection.screen().screenIndex,
                                                  connection.screen().rootWindow, reply->width,
                                                  reply->height});
}

struct ResourcesData final {
    std::uint32_t timestamp;
    std::uint32_t configTimestamp;
    std::vector<std::uint32_t> crtcs;
    std::vector<std::uint32_t> outputs;
    std::vector<std::uint32_t> modes;
};

template <typename Reply> struct ResourcesAccess;

template <> struct ResourcesAccess<xcb_randr_get_screen_resources_reply_t> final {
    [[nodiscard]] static int size(const void *reply) {
        return xcb_randr_get_screen_resources_sizeof(reply);
    }
    [[nodiscard]] static int crtcsLength(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_crtcs_length(reply);
    }
    [[nodiscard]] static int outputsLength(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_outputs_length(reply);
    }
    [[nodiscard]] static int modesLength(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_modes_length(reply);
    }
    [[nodiscard]] static int namesLength(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_names_length(reply);
    }
    [[nodiscard]] static const xcb_randr_crtc_t *
    crtcs(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_crtcs(reply);
    }
    [[nodiscard]] static const xcb_randr_output_t *
    outputs(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_outputs(reply);
    }
    [[nodiscard]] static xcb_randr_mode_info_iterator_t
    modes(const xcb_randr_get_screen_resources_reply_t *reply) {
        return xcb_randr_get_screen_resources_modes_iterator(reply);
    }
};

template <> struct ResourcesAccess<xcb_randr_get_screen_resources_current_reply_t> final {
    [[nodiscard]] static int size(const void *reply) {
        return xcb_randr_get_screen_resources_current_sizeof(reply);
    }
    [[nodiscard]] static int
    crtcsLength(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_crtcs_length(reply);
    }
    [[nodiscard]] static int
    outputsLength(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_outputs_length(reply);
    }
    [[nodiscard]] static int
    modesLength(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_modes_length(reply);
    }
    [[nodiscard]] static int
    namesLength(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_names_length(reply);
    }
    [[nodiscard]] static const xcb_randr_crtc_t *
    crtcs(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_crtcs(reply);
    }
    [[nodiscard]] static const xcb_randr_output_t *
    outputs(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_outputs(reply);
    }
    [[nodiscard]] static xcb_randr_mode_info_iterator_t
    modes(const xcb_randr_get_screen_resources_current_reply_t *reply) {
        return xcb_randr_get_screen_resources_current_modes_iterator(reply);
    }
};

template <typename Reply>
[[nodiscard]] Result<ResourcesData> validateResourcesReply(const Reply *reply) {
    using Access = ResourcesAccess<Reply>;
    if (reply == nullptr || reply->response_type != 1U ||
        reply->num_outputs > maximumTopologyOutputs || reply->num_crtcs > maximumTopologyCrtcs ||
        reply->num_modes > maximumTopologyModes) {
        return malformedReply<ResourcesData>();
    }

    std::size_t declaredBytes = 0U;
    const int expectedBytes = Access::size(reply);
    if (!replySize(reply->length, sizeof(Reply), declaredBytes) ||
        !replyLayoutMatches(expectedBytes, sizeof(Reply), declaredBytes) ||
        Access::crtcsLength(reply) != reply->num_crtcs ||
        Access::outputsLength(reply) != reply->num_outputs ||
        Access::modesLength(reply) != reply->num_modes ||
        Access::namesLength(reply) != reply->names_len) {
        return malformedReply<ResourcesData>();
    }

    ResourcesData data{reply->timestamp, reply->config_timestamp, {}, {}, {}};
    data.crtcs.reserve(reply->num_crtcs);
    const auto *crtcs = Access::crtcs(reply);
    for (std::size_t index = 0U; index < reply->num_crtcs; ++index) {
        if (crtcs == nullptr || !appendUniqueNonzero(data.crtcs, crtcs[index])) {
            return malformedReply<ResourcesData>();
        }
    }

    data.outputs.reserve(reply->num_outputs);
    const auto *outputs = Access::outputs(reply);
    for (std::size_t index = 0U; index < reply->num_outputs; ++index) {
        if (outputs == nullptr || !appendUniqueNonzero(data.outputs, outputs[index])) {
            return malformedReply<ResourcesData>();
        }
    }

    data.modes.reserve(reply->num_modes);
    std::vector<std::uint16_t> modeNameLengths;
    modeNameLengths.reserve(reply->num_modes);
    auto mode = Access::modes(reply);
    while (mode.rem > 0 && mode.data != nullptr) {
        if (!appendUniqueNonzero(data.modes, mode.data->id)) {
            return malformedReply<ResourcesData>();
        }
        modeNameLengths.push_back(mode.data->name_len);
        xcb_randr_mode_info_next(&mode);
    }
    if (data.modes.size() != reply->num_modes ||
        !randrModeNameLengthsMatch(modeNameLengths, reply->names_len)) {
        return malformedReply<ResourcesData>();
    }
    std::sort(data.crtcs.begin(), data.crtcs.end());
    std::sort(data.outputs.begin(), data.outputs.end());
    std::sort(data.modes.begin(), data.modes.end());
    return Result<ResourcesData>::success(std::move(data));
}

[[nodiscard]] Result<ResourcesData> readResources(xcb_connection_t *native, WindowId root,
                                                  bool supportsCurrent) {
    xcb_generic_error_t *rawError = nullptr;
    if (supportsCurrent) {
        ResourcesCurrentReply reply{xcb_randr_get_screen_resources_current_reply(
            native, xcb_randr_get_screen_resources_current(native, root.value()), &rawError)};
        ProtocolError error{rawError};
        if (xcb_connection_has_error(native) != 0) {
            return transportFailure<ResourcesData>();
        }
        if (error || !reply) {
            return malformedReply<ResourcesData>();
        }
        return validateResourcesReply(reply.get());
    }

    Resources12Reply reply{xcb_randr_get_screen_resources_reply(
        native, xcb_randr_get_screen_resources(native, root.value()), &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<ResourcesData>();
    }
    if (error || !reply) {
        return malformedReply<ResourcesData>();
    }
    return validateResourcesReply(reply.get());
}

struct OutputInfoData final {
    std::uint8_t connection;
    std::uint32_t crtc;
    std::vector<std::uint32_t> modes;
};

[[nodiscard]] Result<OutputInfoData> readOutputInfo(xcb_connection_t *native, std::uint32_t output,
                                                    std::uint32_t configTimestamp,
                                                    const ResourcesData &resources) {
    xcb_generic_error_t *rawError = nullptr;
    OutputInfoReply reply{xcb_randr_get_output_info_reply(
        native, xcb_randr_get_output_info(native, output, configTimestamp), &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<OutputInfoData>();
    }
    if (error || !reply || reply->response_type != 1U) {
        return malformedReply<OutputInfoData>();
    }
    if (reply->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
        return changedConfiguration<OutputInfoData>();
    }
    if (reply->num_crtcs > maximumTopologyCrtcs || reply->num_modes > maximumTopologyModes ||
        reply->num_clones > maximumTopologyOutputs || reply->num_preferred > reply->num_modes ||
        reply->name_len > maximumRandrOutputNameBytes ||
        reply->connection > XCB_RANDR_CONNECTION_UNKNOWN) {
        return malformedReply<OutputInfoData>();
    }

    std::size_t declaredBytes = 0U;
    const int expectedBytes = xcb_randr_get_output_info_sizeof(reply.get());
    if (!replySize(reply->length, sizeof(xcb_randr_get_output_info_reply_t), declaredBytes) ||
        !replyLayoutMatches(expectedBytes, sizeof(xcb_randr_get_output_info_reply_t),
                            declaredBytes) ||
        xcb_randr_get_output_info_crtcs_length(reply.get()) != reply->num_crtcs ||
        xcb_randr_get_output_info_modes_length(reply.get()) != reply->num_modes ||
        xcb_randr_get_output_info_clones_length(reply.get()) != reply->num_clones ||
        xcb_randr_get_output_info_name_length(reply.get()) != reply->name_len) {
        return malformedReply<OutputInfoData>();
    }
    std::vector<std::uint32_t> crtcs;
    std::vector<std::uint32_t> modes;
    std::vector<std::uint32_t> clones;
    if (!copyWireList(xcb_randr_get_output_info_crtcs(reply.get()), reply->num_crtcs, crtcs) ||
        !copyWireList(xcb_randr_get_output_info_modes(reply.get()), reply->num_modes, modes) ||
        !copyWireList(xcb_randr_get_output_info_clones(reply.get()), reply->num_clones, clones) ||
        !randrOutputInfoListsAreValid({reply->crtc, crtcs, modes, clones},
                                      {resources.crtcs, resources.outputs, resources.modes})) {
        return malformedReply<OutputInfoData>();
    }
    return Result<OutputInfoData>::success(
        OutputInfoData{reply->connection, reply->crtc, std::move(modes)});
}

struct CrtcInfoData final {
    std::int16_t x;
    std::int16_t y;
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t mode;
};

[[nodiscard]] Result<CrtcInfoData> readCrtcInfo(xcb_connection_t *native, std::uint32_t crtc,
                                                std::uint32_t output, std::uint32_t configTimestamp,
                                                std::span<const std::uint32_t> resourceOutputs) {
    xcb_generic_error_t *rawError = nullptr;
    CrtcInfoReply reply{xcb_randr_get_crtc_info_reply(
        native, xcb_randr_get_crtc_info(native, crtc, configTimestamp), &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<CrtcInfoData>();
    }
    if (error || !reply || reply->response_type != 1U) {
        return malformedReply<CrtcInfoData>();
    }
    if (reply->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
        return changedConfiguration<CrtcInfoData>();
    }
    if (reply->num_outputs > maximumTopologyOutputs ||
        reply->num_possible_outputs > maximumTopologyOutputs) {
        return malformedReply<CrtcInfoData>();
    }

    std::size_t declaredBytes = 0U;
    const int expectedBytes = xcb_randr_get_crtc_info_sizeof(reply.get());
    if (!replySize(reply->length, sizeof(xcb_randr_get_crtc_info_reply_t), declaredBytes) ||
        !replyLayoutMatches(expectedBytes, sizeof(xcb_randr_get_crtc_info_reply_t),
                            declaredBytes) ||
        xcb_randr_get_crtc_info_outputs_length(reply.get()) != reply->num_outputs ||
        xcb_randr_get_crtc_info_possible_length(reply.get()) != reply->num_possible_outputs) {
        return malformedReply<CrtcInfoData>();
    }

    std::vector<std::uint32_t> outputs;
    std::vector<std::uint32_t> possibleOutputs;
    if (!copyWireList(xcb_randr_get_crtc_info_outputs(reply.get()), reply->num_outputs, outputs) ||
        !copyWireList(xcb_randr_get_crtc_info_possible(reply.get()), reply->num_possible_outputs,
                      possibleOutputs) ||
        !randrCrtcInfoListsAreValid({output, outputs, possibleOutputs}, resourceOutputs)) {
        return malformedReply<CrtcInfoData>();
    }
    return Result<CrtcInfoData>::success(
        CrtcInfoData{reply->x, reply->y, reply->width, reply->height, reply->mode});
}

[[nodiscard]] Result<std::optional<OutputId>> readPrimary(xcb_connection_t *native, WindowId root) {
    xcb_generic_error_t *rawError = nullptr;
    PrimaryReply reply{xcb_randr_get_output_primary_reply(
        native, xcb_randr_get_output_primary(native, root.value()), &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<std::optional<OutputId>>();
    }
    if (error || !reply || reply->response_type != 1U || reply->length != 0U) {
        return malformedReply<std::optional<OutputId>>();
    }
    if (reply->output == XCB_NONE) {
        return Result<std::optional<OutputId>>::success(std::nullopt);
    }
    auto output = OutputId::fromProtocol(reply->output);
    if (!output) {
        return malformedReply<std::optional<OutputId>>();
    }
    return Result<std::optional<OutputId>>::success(output.value());
}

struct TopologyData final {
    std::size_t resourceOutputCount;
    std::size_t resourceCrtcCount;
    std::size_t resourceModeCount;
    std::vector<OutputCandidate> activeOutputs;
    std::optional<OutputId> primary;
};

[[nodiscard]] bool sameResources(const ResourcesData &left, const ResourcesData &right) noexcept {
    return left.timestamp == right.timestamp && left.configTimestamp == right.configTimestamp &&
           left.crtcs == right.crtcs && left.outputs == right.outputs && left.modes == right.modes;
}

[[nodiscard]] Result<TopologyData> queryTopologyAttempt(xcb_connection_t *native, WindowId root,
                                                        bool supportsCurrent,
                                                        bool supportsPrimary) {
    auto resources = readResources(native, root, supportsCurrent);
    if (!resources) {
        return Result<TopologyData>::failure(resources.error());
    }

    std::vector<OutputCandidate> activeOutputs;
    activeOutputs.reserve(resources.value().outputs.size());
    for (const auto rawOutput : resources.value().outputs) {
        auto output = OutputId::fromProtocol(rawOutput);
        if (!output) {
            return malformedReply<TopologyData>();
        }
        auto outputInfo =
            readOutputInfo(native, rawOutput, resources.value().configTimestamp, resources.value());
        if (!outputInfo) {
            return Result<TopologyData>::failure(outputInfo.error());
        }
        if (outputInfo.value().connection != XCB_RANDR_CONNECTION_CONNECTED ||
            outputInfo.value().crtc == XCB_NONE) {
            continue;
        }
        if (!contains(resources.value().crtcs, outputInfo.value().crtc)) {
            return changedConfiguration<TopologyData>();
        }
        auto crtc = CrtcId::fromProtocol(outputInfo.value().crtc);
        if (!crtc) {
            return malformedReply<TopologyData>();
        }
        auto crtcInfo = readCrtcInfo(native, outputInfo.value().crtc, rawOutput,
                                     resources.value().configTimestamp, resources.value().outputs);
        if (!crtcInfo) {
            return Result<TopologyData>::failure(crtcInfo.error());
        }
        if (crtcInfo.value().mode == XCB_NONE || crtcInfo.value().width == 0U ||
            crtcInfo.value().height == 0U) {
            continue;
        }
        if (!contains(resources.value().modes, crtcInfo.value().mode) ||
            !contains(outputInfo.value().modes, crtcInfo.value().mode)) {
            return changedConfiguration<TopologyData>();
        }
        activeOutputs.push_back(OutputCandidate{output.value(), crtc.value(), crtcInfo.value().x,
                                                crtcInfo.value().y, crtcInfo.value().width,
                                                crtcInfo.value().height});
    }

    std::optional<OutputId> primary;
    if (supportsPrimary) {
        auto discoveredPrimary = readPrimary(native, root);
        if (!discoveredPrimary) {
            return Result<TopologyData>::failure(discoveredPrimary.error());
        }
        primary = discoveredPrimary.value();
    }

    auto confirmedResources = readResources(native, root, supportsCurrent);
    if (!confirmedResources) {
        return Result<TopologyData>::failure(confirmedResources.error());
    }
    if (!sameResources(resources.value(), confirmedResources.value())) {
        return changedConfiguration<TopologyData>();
    }
    if (supportsPrimary) {
        auto confirmedPrimary = readPrimary(native, root);
        if (!confirmedPrimary) {
            return Result<TopologyData>::failure(confirmedPrimary.error());
        }
        if (confirmedPrimary.value() != primary) {
            return changedConfiguration<TopologyData>();
        }
    }
    return Result<TopologyData>::success(
        TopologyData{resources.value().outputs.size(), resources.value().crtcs.size(),
                     resources.value().modes.size(), std::move(activeOutputs), primary});
}

[[nodiscard]] RandrTopologySnapshot reducedSnapshot(RandrTopologyStatus status,
                                                    ScreenInfo coreScreen) {
    return RandrTopologySnapshot{status, coreScreen, 0U, 0U, 0U, {}, std::nullopt};
}

} // namespace

bool randrOutputInfoListsAreValid(RandrOutputInfoListsView output,
                                  RandrResourceIdsView resources) noexcept {
    const std::span<const std::uint32_t> allowedCrtcs = output.allowedCrtcs;
    const std::span<const std::uint32_t> supportedModes = output.supportedModes;
    const std::span<const std::uint32_t> clones = output.clones;
    if (!normalizeUniqueNonzero(output.allowedCrtcs) ||
        !normalizeUniqueNonzero(output.supportedModes) || !normalizeUniqueNonzero(output.clones) ||
        !allContainedSorted(allowedCrtcs, resources.crtcs) ||
        !allContainedSorted(supportedModes, resources.modes) ||
        !allContainedSorted(clones, resources.outputs)) {
        return false;
    }
    return output.activeCrtc == 0U || contains(allowedCrtcs, output.activeCrtc);
}

bool randrCrtcInfoListsAreValid(RandrCrtcInfoListsView crtc,
                                std::span<const std::uint32_t> resourceOutputs) noexcept {
    const std::span<const std::uint32_t> currentOutputs = crtc.currentOutputs;
    const std::span<const std::uint32_t> possibleOutputs = crtc.possibleOutputs;
    return crtc.requestedOutput != 0U && normalizeUniqueNonzero(crtc.currentOutputs) &&
           normalizeUniqueNonzero(crtc.possibleOutputs) &&
           allContainedSorted(currentOutputs, resourceOutputs) &&
           allContainedSorted(possibleOutputs, resourceOutputs) &&
           allContainedSorted(currentOutputs, possibleOutputs) &&
           contains(currentOutputs, crtc.requestedOutput);
}

bool randrModeNameLengthsMatch(std::span<const std::uint16_t> modeNameLengths,
                               std::uint16_t namesLength) noexcept {
    std::size_t total = 0U;
    for (const auto length : modeNameLengths) {
        if (length > std::numeric_limits<std::size_t>::max() - total) {
            return false;
        }
        total += length;
    }
    return total == namesLength;
}

std::string_view randrTopologyStatusId(RandrTopologyStatus status) noexcept {
    switch (status) {
    case RandrTopologyStatus::unavailable:
        return "randr_unavailable";
    case RandrTopologyStatus::malformed:
        return "randr_malformed";
    case RandrTopologyStatus::randr_1_2:
        return "randr_1_2";
    case RandrTopologyStatus::randr_1_3:
        return "randr_1_3";
    case RandrTopologyStatus::randr_1_4:
        return "randr_1_4";
    }
    return "randr_malformed";
}

bool randrEventRequiresFullRequery(std::uint8_t firstEvent, RandrEventFields event,
                                   bool supportsResourceChange) noexcept {
    if (firstEvent == 0U) {
        return false;
    }
    const auto eventType = static_cast<std::uint8_t>(event.responseType & 0x7fU);
    if (eventType == firstEvent + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        return true;
    }
    if (eventType != firstEvent + XCB_RANDR_NOTIFY) {
        return false;
    }
    return event.notifySubCode == XCB_RANDR_NOTIFY_CRTC_CHANGE ||
           event.notifySubCode == XCB_RANDR_NOTIFY_OUTPUT_CHANGE ||
           (supportsResourceChange && event.notifySubCode == XCB_RANDR_NOTIFY_RESOURCE_CHANGE);
}

Result<RandrTopologyProtocol> RandrTopologyProtocol::negotiate(X11Connection &connection) {
    if (!connection.healthy()) {
        return transportFailure<RandrTopologyProtocol>();
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (native == nullptr) {
        return transportFailure<RandrTopologyProtocol>();
    }
    const auto *extension = xcb_get_extension_data(native, &xcb_randr_id);
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<RandrTopologyProtocol>();
    }
    if (extension == nullptr || extension->present == 0U) {
        return Result<RandrTopologyProtocol>::success(RandrTopologyProtocol{
            RandrTopologyStatus::unavailable, 0U, 0U, 0U, connection.identity()});
    }

    xcb_generic_error_t *rawError = nullptr;
    VersionReply version{xcb_randr_query_version_reply(
        native,
        xcb_randr_query_version(native, requiredRandrMajorVersion, resourceChangeRandrMinorVersion),
        &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<RandrTopologyProtocol>();
    }
    if (error || !version || version->response_type != 1U || version->length != 0U ||
        extension->first_event == 0U || extension->first_event > 0x7eU) {
        return Result<RandrTopologyProtocol>::success(RandrTopologyProtocol{
            RandrTopologyStatus::malformed, 0U, 0U, extension->first_event, connection.identity()});
    }

    const bool supports12 = version->major_version > requiredRandrMajorVersion ||
                            (version->major_version == requiredRandrMajorVersion &&
                             version->minor_version >= requiredRandrMinorVersion);
    if (!supports12) {
        return Result<RandrTopologyProtocol>::success(RandrTopologyProtocol{
            RandrTopologyStatus::unavailable, version->major_version, version->minor_version,
            extension->first_event, connection.identity()});
    }
    const bool supports13 = version->major_version > requiredRandrMajorVersion ||
                            (version->major_version == requiredRandrMajorVersion &&
                             version->minor_version >= primaryOutputRandrMinorVersion);
    const bool supports14 = version->major_version > requiredRandrMajorVersion ||
                            (version->major_version == requiredRandrMajorVersion &&
                             version->minor_version >= resourceChangeRandrMinorVersion);
    const auto status = supports14   ? RandrTopologyStatus::randr_1_4
                        : supports13 ? RandrTopologyStatus::randr_1_3
                                     : RandrTopologyStatus::randr_1_2;
    return Result<RandrTopologyProtocol>::success(
        RandrTopologyProtocol{status, version->major_version, version->minor_version,
                              extension->first_event, connection.identity()});
}

Result<RandrTopologySnapshot> RandrTopologyProtocol::query(X11Connection &connection) const {
    if (connection.identity() == 0U || connection.identity() != connection_identity_) {
        return invalidConnection<RandrTopologySnapshot>();
    }
    if (!connection.healthy()) {
        return transportFailure<RandrTopologySnapshot>();
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (native == nullptr) {
        return transportFailure<RandrTopologySnapshot>();
    }
    auto coreScreen = queryCoreScreen(connection, native);
    if (!coreScreen) {
        return Result<RandrTopologySnapshot>::failure(coreScreen.error());
    }
    const auto freshMalformedSnapshot = [&]() -> Result<RandrTopologySnapshot> {
        auto freshCoreScreen = queryCoreScreen(connection, native);
        if (!freshCoreScreen) {
            return Result<RandrTopologySnapshot>::failure(freshCoreScreen.error());
        }
        return Result<RandrTopologySnapshot>::success(
            reducedSnapshot(RandrTopologyStatus::malformed, freshCoreScreen.value()));
    };
    if (status_ == RandrTopologyStatus::unavailable) {
        return Result<RandrTopologySnapshot>::success(reducedSnapshot(status_, coreScreen.value()));
    }
    if (status_ == RandrTopologyStatus::malformed) {
        return freshMalformedSnapshot();
    }

    for (std::size_t attempt = 0U; attempt < 2U; ++attempt) {
        const bool supportsCurrent =
            status_ == RandrTopologyStatus::randr_1_3 || status_ == RandrTopologyStatus::randr_1_4;
        auto topology = queryTopologyAttempt(native, connection.screen().rootWindow,
                                             supportsCurrent, supportsCurrent);
        if (topology) {
            auto confirmedCoreScreen = queryCoreScreen(connection, native);
            if (!confirmedCoreScreen) {
                return Result<RandrTopologySnapshot>::failure(confirmedCoreScreen.error());
            }
            if (confirmedCoreScreen.value().widthPx != coreScreen.value().widthPx ||
                confirmedCoreScreen.value().heightPx != coreScreen.value().heightPx ||
                confirmedCoreScreen.value().rootWindow != coreScreen.value().rootWindow) {
                if (attempt == 0U) {
                    coreScreen = std::move(confirmedCoreScreen);
                    continue;
                }
                return freshMalformedSnapshot();
            }
            return Result<RandrTopologySnapshot>::success(RandrTopologySnapshot{
                status_, confirmedCoreScreen.value(), topology.value().resourceOutputCount,
                topology.value().resourceCrtcCount, topology.value().resourceModeCount,
                std::move(topology.value().activeOutputs), topology.value().primary});
        }
        if (topology.error().code == ErrorCode::io_error || xcb_connection_has_error(native) != 0) {
            return transportFailure<RandrTopologySnapshot>();
        }
        if (topology.error().code != ErrorCode::cancelled || attempt != 0U) {
            return freshMalformedSnapshot();
        }
        coreScreen = queryCoreScreen(connection, native);
        if (!coreScreen) {
            return Result<RandrTopologySnapshot>::failure(coreScreen.error());
        }
    }
    return freshMalformedSnapshot();
}

Result<bool> RandrTopologyProtocol::selectTopologyEvents(X11Connection &connection) const {
    if (connection.identity() == 0U || connection.identity() != connection_identity_) {
        return invalidConnection<bool>();
    }
    if (!connection.healthy()) {
        return transportFailure<bool>();
    }
    if (status_ == RandrTopologyStatus::unavailable || status_ == RandrTopologyStatus::malformed) {
        return Result<bool>::success(false);
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (native == nullptr) {
        return transportFailure<bool>();
    }
    const auto eventMask = status_ == RandrTopologyStatus::randr_1_4 ? randrTopologyEventMask
                                                                     : randr12TopologyEventMask;
    const auto cookie =
        xcb_randr_select_input_checked(native, connection.screen().rootWindow.value(), eventMask);
    ProtocolError error{xcb_request_check(native, cookie)};
    if (error || xcb_connection_has_error(native) != 0) {
        return transportFailure<bool>();
    }
    return Result<bool>::success(true);
}

bool RandrTopologyProtocol::eventRequiresFullRequery(RandrEventFields event) const noexcept {
    if ((status_ != RandrTopologyStatus::randr_1_2 && status_ != RandrTopologyStatus::randr_1_3 &&
         status_ != RandrTopologyStatus::randr_1_4) ||
        first_event_ == 0U) {
        return false;
    }
    return randrEventRequiresFullRequery(first_event_, event,
                                         status_ == RandrTopologyStatus::randr_1_4);
}

} // namespace prismdrake::x11
