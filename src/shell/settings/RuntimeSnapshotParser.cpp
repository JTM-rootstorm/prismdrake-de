#include "RuntimeSnapshotParser.hpp"

#include "RuntimeSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::shell::settings {
namespace {

using Json = nlohmann::ordered_json;
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::size_t maximumSnapshotNesting = 24U;
constexpr std::size_t maximumSnapshotNodes = 8192U;
constexpr std::size_t maximumContainerItems = 512U;
constexpr std::size_t maximumStringCodePoints = 256U;

[[nodiscard]] Error invalidSnapshot(ErrorCode code = ErrorCode::validation_error) {
    return {code, "The complete settings snapshot is invalid.",
            "Retain the prior complete generation and refetch after the next owner or generation "
            "change."};
}

[[nodiscard]] std::size_t utf8CodePointCount(std::string_view value) noexcept {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
}

class SnapshotBoundsSax final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t value, const string_t &) override {
        return std::isfinite(value) && scalar();
    }
    bool string(string_t &value) override {
        return utf8CodePointCount(value) <= maximumStringCodePoints && scalar();
    }
    bool binary(binary_t &) override { return false; }

    bool start_object(std::size_t elements) override { return startContainer(true, elements); }
    bool key(string_t &value) override {
        if (stack_.empty() || !stack_.back().object ||
            utf8CodePointCount(value) > maximumStringCodePoints) {
            return false;
        }
        auto &frame = stack_.back();
        ++frame.items;
        return frame.items <= maximumContainerItems && frame.keys.insert(value).second;
    }
    bool end_object() override { return endContainer(true); }

    bool start_array(std::size_t elements) override { return startContainer(false, elements); }
    bool end_array() override { return endContainer(false); }

    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }

  private:
    struct Frame final {
        bool object;
        std::size_t items;
        std::set<std::string, std::less<>> keys;
    };

    bool noteValue() {
        ++nodes_;
        if (nodes_ > maximumSnapshotNodes || stack_.size() > maximumSnapshotNesting) {
            return false;
        }
        if (!stack_.empty() && !stack_.back().object) {
            ++stack_.back().items;
            if (stack_.back().items > maximumContainerItems) {
                return false;
            }
        }
        return true;
    }

    bool scalar() { return noteValue(); }

    bool startContainer(bool object, std::size_t elements) {
        if (!noteValue() || stack_.size() >= maximumSnapshotNesting ||
            (elements != std::numeric_limits<std::size_t>::max() &&
             elements > maximumContainerItems)) {
            return false;
        }
        stack_.push_back({object, 0U, {}});
        return true;
    }

    bool endContainer(bool object) {
        if (stack_.empty() || stack_.back().object != object) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    std::size_t nodes_{0U};
    std::vector<Frame> stack_;
};

[[nodiscard]] const Json &object(const Json &parent, std::string_view key) {
    const auto &value = parent.at(key);
    if (!value.is_object()) {
        throw std::domain_error("object expected");
    }
    return value;
}

[[nodiscard]] std::string stringValue(const Json &parent, std::string_view key,
                                      std::size_t maximum = maximumStringCodePoints) {
    const auto &value = parent.at(key);
    if (!value.is_string()) {
        throw std::domain_error("string expected");
    }
    auto result = value.get<std::string>();
    if (result.empty() || utf8CodePointCount(result) > maximum) {
        throw std::domain_error("bounded string expected");
    }
    return result;
}

[[nodiscard]] bool booleanValue(const Json &parent, std::string_view key) {
    const auto &value = parent.at(key);
    if (!value.is_boolean()) {
        throw std::domain_error("boolean expected");
    }
    return value.get<bool>();
}

[[nodiscard]] double numberValue(const Json &parent, std::string_view key, double minimum,
                                 double maximum) {
    const auto &value = parent.at(key);
    if (!value.is_number() || value.is_boolean()) {
        throw std::domain_error("number expected");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result) || result < minimum || result > maximum) {
        throw std::domain_error("bounded number expected");
    }
    return result;
}

template <typename Integer>
[[nodiscard]] Integer integerValue(const Json &parent, std::string_view key, std::uint64_t minimum,
                                   std::uint64_t maximum) {
    const auto &value = parent.at(key);
    if ((!value.is_number_unsigned() && !value.is_number_integer()) || value.is_boolean()) {
        throw std::domain_error("integer expected");
    }
    std::uint64_t result = 0U;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else {
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue < 0) {
            throw std::domain_error("nonnegative integer expected");
        }
        result = static_cast<std::uint64_t>(signedValue);
    }
    if (result < minimum || result > maximum ||
        result > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
        throw std::domain_error("bounded integer expected");
    }
    return static_cast<Integer>(result);
}

template <typename Name, typename Enum, std::size_t Size>
[[nodiscard]] Enum enumValue(const Json &parent, std::string_view key,
                             const std::array<std::pair<Name, Enum>, Size> &values) {
    const auto text = stringValue(parent, key);
    const auto found = std::find_if(values.begin(), values.end(), [&text](const auto &candidate) {
        return candidate.first == text;
    });
    if (found == values.end()) {
        throw std::domain_error("closed identifier expected");
    }
    return found->second;
}

[[nodiscard]] bool validMapKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > 64U || key.front() < 'a' || key.front() > 'z') {
        return false;
    }
    return std::all_of(key.begin() + 1, key.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_';
    });
}

[[nodiscard]] theme::NumberMap numberMap(const Json &parent, std::string_view key, double minimum,
                                         double maximum) {
    const auto &source = object(parent, key);
    if (source.empty() || source.size() > maximumContainerItems) {
        throw std::domain_error("bounded map expected");
    }
    theme::NumberMap result;
    for (const auto &[name, value] : source.items()) {
        if (!validMapKey(name) || !value.is_number() || value.is_boolean()) {
            throw std::domain_error("number map expected");
        }
        const auto number = value.get<double>();
        if (!std::isfinite(number) || number < minimum || number > maximum) {
            throw std::domain_error("bounded number map expected");
        }
        result.emplace(name, number);
    }
    return result;
}

[[nodiscard]] theme::NumberMap
exactNumberMap(const Json &parent, std::string_view key,
               std::initializer_list<std::pair<std::string_view, double>> minimums) {
    auto result = numberMap(parent, key, 0.0, 65535.0);
    if (result.size() != minimums.size()) {
        throw std::domain_error("exact metric map expected");
    }
    for (const auto &[name, minimum] : minimums) {
        const auto found = result.find(name);
        if (found == result.end() || found->second < minimum) {
            throw std::domain_error("exact bounded metric map expected");
        }
    }
    return result;
}

[[nodiscard]] theme::Color color(std::string_view value) {
    if (value.size() != 9U || value.front() != '#') {
        throw std::domain_error("color expected");
    }
    std::uint32_t rgba = 0U;
    const auto converted = std::from_chars(value.data() + 1, value.data() + value.size(), rgba, 16);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
        throw std::domain_error("color expected");
    }
    return {rgba};
}

[[nodiscard]] theme::Color colorValue(const Json &parent, std::string_view key) {
    return color(stringValue(parent, key, 9U));
}

[[nodiscard]] theme::ColorMap colorMap(const Json &parent, std::string_view key) {
    const auto &source = object(parent, key);
    if (source.size() < 4U || source.size() > maximumContainerItems) {
        throw std::domain_error("bounded color map expected");
    }
    theme::ColorMap result;
    for (const auto &[name, value] : source.items()) {
        if (!validMapKey(name) || !value.is_string()) {
            throw std::domain_error("color map expected");
        }
        result.emplace(name, color(value.get_ref<const std::string &>()));
    }
    return result;
}

[[nodiscard]] std::vector<std::string> stringArray(const Json &parent, std::string_view key,
                                                   std::size_t minimum, std::size_t maximum,
                                                   std::size_t maximumCodePoints,
                                                   bool requireUnique = true) {
    const auto &source = parent.at(key);
    if (!source.is_array() || source.size() < minimum || source.size() > maximum) {
        throw std::domain_error("bounded array expected");
    }
    std::vector<std::string> result;
    std::set<std::string, std::less<>> unique;
    for (const auto &value : source) {
        if (!value.is_string()) {
            throw std::domain_error("string array expected");
        }
        auto text = value.get<std::string>();
        if (text.empty() || utf8CodePointCount(text) > maximumCodePoints ||
            (requireUnique && !unique.insert(text).second)) {
            throw std::domain_error("unique bounded string array expected");
        }
        result.push_back(std::move(text));
    }
    return result;
}

template <typename Name, typename Enum, std::size_t Size>
[[nodiscard]] std::vector<Enum> enumArray(const Json &parent, std::string_view key,
                                          std::size_t minimum, std::size_t maximum,
                                          const std::array<std::pair<Name, Enum>, Size> &values) {
    const auto texts = stringArray(parent, key, minimum, maximum, maximumStringCodePoints);
    std::vector<Enum> result;
    for (const auto &text : texts) {
        const auto found =
            std::find_if(values.begin(), values.end(),
                         [&text](const auto &candidate) { return candidate.first == text; });
        if (found == values.end()) {
            throw std::domain_error("closed identifier array expected");
        }
        result.push_back(found->second);
    }
    return result;
}

[[nodiscard]] config::Profile profile(const Json &document) {
    constexpr std::array values{std::pair{"lustre", config::Profile::lustre},
                                std::pair{"forge", config::Profile::forge}};
    return enumValue(document, "profile_id", values);
}

[[nodiscard]] config::Configuration configuration(const Json &document,
                                                  config::Profile activeProfile) {
    const auto &source = object(document, "settings");
    if (integerValue<std::uint32_t>(source, "schema_version", 1U, 1U) != 1U) {
        throw std::domain_error("settings version expected");
    }
    const auto &appearance = object(source, "appearance");
    constexpr std::array blurValues{std::pair{"off", config::BlurQuality::off},
                                    std::pair{"low", config::BlurQuality::low},
                                    std::pair{"balanced", config::BlurQuality::balanced},
                                    std::pair{"high", config::BlurQuality::high}};
    auto accent = stringValue(appearance, "accent", 7U);
    if (accent.size() != 7U || accent.front() != '#' ||
        !std::all_of(accent.begin() + 1, accent.end(),
                     [](unsigned char character) { return std::isxdigit(character) != 0; })) {
        throw std::domain_error("accent color expected");
    }
    config::Appearance appearanceValue{
        std::move(accent),
        booleanValue(appearance, "transparency_enabled"),
        enumValue(appearance, "blur_quality", blurValues),
        booleanValue(appearance, "reduced_motion"),
        booleanValue(appearance, "high_contrast"),
        numberValue(appearance, "text_scale", 0.75, 3.0),
        stringValue(appearance, "cursor_theme", 128U),
        integerValue<std::uint16_t>(appearance, "cursor_size_px", 16U, 256U),
        stringValue(appearance, "icon_theme", 128U)};

    const auto &panel = object(source, "panel");
    constexpr std::array edgeValues{
        std::pair{"top", config::PanelEdge::top}, std::pair{"right", config::PanelEdge::right},
        std::pair{"bottom", config::PanelEdge::bottom}, std::pair{"left", config::PanelEdge::left}};
    constexpr std::array autohideValues{std::pair{"never", config::AutohideMode::never},
                                        std::pair{"always", config::AutohideMode::always},
                                        std::pair{"overlap", config::AutohideMode::overlap}};
    constexpr std::array groupingValues{std::pair{"always", config::GroupingMode::always},
                                        std::pair{"when_full", config::GroupingMode::when_full},
                                        std::pair{"never", config::GroupingMode::never}};
    constexpr std::array clockValues{std::pair{"locale", config::ClockFormat::locale},
                                     std::pair{"12_hour", config::ClockFormat::hour_12},
                                     std::pair{"24_hour", config::ClockFormat::hour_24}};
    config::Panel panelValue{enumValue(panel, "edge", edgeValues),
                             integerValue<std::uint16_t>(panel, "size_px", 24U, 192U),
                             enumValue(panel, "autohide", autohideValues),
                             enumValue(panel, "grouping", groupingValues),
                             enumValue(panel, "clock_format", clockValues),
                             stringArray(panel, "outputs", 0U, 32U, 128U)};

    const auto &launcher = object(source, "launcher");
    constexpr std::array layoutValues{std::pair{"compact", config::LauncherLayout::compact},
                                      std::pair{"expanded", config::LauncherLayout::expanded}};
    constexpr std::array providerValues{
        std::pair{"applications", config::SearchProvider::applications},
        std::pair{"settings", config::SearchProvider::settings},
        std::pair{"recent_files", config::SearchProvider::recent_files}};
    constexpr std::array recentValues{
        std::pair{"disabled", config::RecentItemsPolicy::disabled},
        std::pair{"local_only", config::RecentItemsPolicy::local_only}};
    config::Launcher launcherValue{enumValue(launcher, "layout", layoutValues),
                                   enumArray(launcher, "search_providers", 0U, 32U, providerValues),
                                   enumValue(launcher, "recent_items", recentValues)};

    const auto &notifications = object(source, "notifications");
    constexpr std::array historyValues{
        std::pair{"disabled", config::NotificationHistory::disabled},
        std::pair{"session", config::NotificationHistory::session},
        std::pair{"persistent", config::NotificationHistory::persistent}};
    config::Notifications notificationValue{
        booleanValue(notifications, "enabled"), enumValue(notifications, "history", historyValues),
        integerValue<std::uint32_t>(notifications, "default_timeout_ms", 0U, 120000U),
        booleanValue(notifications, "do_not_disturb")};

    const auto &desktop = object(source, "desktop");
    constexpr std::array wallpaperValues{std::pair{"solid", config::WallpaperMode::solid},
                                         std::pair{"center", config::WallpaperMode::center},
                                         std::pair{"fit", config::WallpaperMode::fit},
                                         std::pair{"fill", config::WallpaperMode::fill},
                                         std::pair{"stretch", config::WallpaperMode::stretch},
                                         std::pair{"tile", config::WallpaperMode::tile}};
    config::Desktop desktopValue{enumValue(desktop, "wallpaper_mode", wallpaperValues),
                                 booleanValue(desktop, "icons_enabled")};

    const auto &integration = object(source, "integration");
    config::Integration integrationValue{
        booleanValue(integration, "export_gtk"), booleanValue(integration, "export_qt"),
        booleanValue(integration, "export_xsettings"), booleanValue(integration, "export_portal")};

    const auto &accessibility = object(source, "accessibility");
    constexpr std::array focusValues{std::pair{"standard", config::FocusEmphasis::standard},
                                     std::pair{"strong", config::FocusEmphasis::strong}};
    config::Accessibility accessibilityValue{
        enumValue(accessibility, "focus_emphasis", focusValues),
        numberValue(accessibility, "animation_scale", 0.0, 2.0),
        integerValue<std::uint16_t>(accessibility, "minimum_target_size_px", 24U, 96U)};

    const auto &keyboard = object(source, "keyboard");
    config::Keyboard keyboardValue{booleanValue(keyboard, "menu_key_opens_launcher"),
                                   booleanValue(keyboard, "focus_wraps")};

    const auto &developer = object(source, "developer");
    config::Developer developerValue{
        booleanValue(developer, "diagnostics_enabled"),
        stringArray(developer, "mock_capability_overrides", 0U, 64U, 128U)};

    return {1U,
            activeProfile,
            std::move(appearanceValue),
            std::move(panelValue),
            std::move(launcherValue),
            std::move(notificationValue),
            std::move(desktopValue),
            std::move(integrationValue),
            std::move(accessibilityValue),
            std::move(keyboardValue),
            std::move(developerValue)};
}

[[nodiscard]] theme::Material material(const Json &parent, std::string_view key) {
    const auto &source = object(parent, key);
    const auto &blur = object(source, "blur_request");
    const auto &fallback = object(source, "fallback");
    constexpr std::array fallbackValues{std::pair{"alpha", theme::FallbackKind::alpha},
                                        std::pair{"opaque", theme::FallbackKind::opaque}};
    return {colorValue(source, "tint"),
            numberValue(source, "opacity", 0.0, 1.0),
            {booleanValue(blur, "enabled"), numberValue(blur, "radius_px", 0.0, 128.0),
             numberValue(blur, "saturation", 0.0, 2.0)},
            {enumValue(fallback, "kind", fallbackValues), colorValue(fallback, "color"),
             numberValue(fallback, "opacity", 0.0, 1.0)}};
}

[[nodiscard]] theme::ComponentStyle component(const Json &parent, std::string_view key) {
    const auto &source = object(parent, key);
    return {numberValue(source, "radius_px", 0.0, 65535.0),
            numberValue(source, "padding_px", 0.0, 65535.0),
            numberValue(source, "border_width_px", 0.0, 65535.0)};
}

[[nodiscard]] theme::ResolvedMaterial resolvedMaterial(const Json &parent, std::string_view key) {
    const auto &source = object(parent, key);
    return {colorValue(source, "color"),
            numberValue(source, "opacity", 0.0, 1.0),
            booleanValue(source, "blur_requested"),
            numberValue(source, "blur_radius_px", 0.0, 128.0),
            numberValue(source, "saturation", 0.0, 2.0),
            booleanValue(source, "used_fallback")};
}

[[nodiscard]] theme::ResolvedThemeCandidate themeCandidate(const Json &document,
                                                           config::Profile activeProfile) {
    const auto &source = object(document, "theme");
    if (integerValue<std::uint32_t>(source, "schema_version", 1U, 1U) != 1U) {
        throw std::domain_error("theme version expected");
    }
    constexpr std::array profileValues{std::pair{"lustre", theme::Profile::lustre},
                                       std::pair{"forge", theme::Profile::forge}};
    const auto themeProfile = enumValue(source, "profile_id", profileValues);
    const auto expectedThemeProfile =
        activeProfile == config::Profile::lustre ? theme::Profile::lustre : theme::Profile::forge;
    if (themeProfile != expectedThemeProfile) {
        throw std::domain_error("coherent profile expected");
    }

    constexpr std::array sourceValues{
        std::pair{"packaged_base", theme::ThemeSource::packaged_base},
        std::pair{"packaged_lustre", theme::ThemeSource::packaged_lustre},
        std::pair{"packaged_forge", theme::ThemeSource::packaged_forge},
        std::pair{"packaged_accessibility", theme::ThemeSource::packaged_accessibility}};
    const auto sources = enumArray(source, "logical_source_ids", 2U, 3U, sourceValues);
    if (sources.front() != theme::ThemeSource::packaged_base ||
        sources[1] != (activeProfile == config::Profile::lustre
                           ? theme::ThemeSource::packaged_lustre
                           : theme::ThemeSource::packaged_forge) ||
        (sources.size() == 3U && sources[2] != theme::ThemeSource::packaged_accessibility)) {
        throw std::domain_error("coherent theme sources expected");
    }
    const auto displayName = stringValue(source, "profile_display_name");
    if (displayName !=
        (activeProfile == config::Profile::lustre ? "Prismdrake Lustre" : "Prismdrake Forge")) {
        throw std::domain_error("coherent profile name expected");
    }

    const auto &primitive = object(source, "primitive");
    theme::PrimitiveTokens primitiveValue{
        colorMap(primitive, "colors"),
        numberMap(primitive, "spacing_px", 0.0, 65535.0),
        stringArray(primitive, "font_families", 1U, 64U, 256U, false),
        numberMap(primitive, "font_size_px", 0.0, 196605.0),
        numberMap(primitive, "duration_ms", 0.0, 131070.0),
        numberMap(primitive, "radius_px", 0.0, 65535.0),
        numberMap(primitive, "opacity", 0.0, 1.0)};

    const auto &semantic = object(source, "semantic");
    const auto &colors = object(semantic, "colors");
    theme::SemanticColors colorValueSet{
        colorValue(colors, "panel_surface"),   colorValue(colors, "elevated_surface"),
        colorValue(colors, "window_frame"),    colorValue(colors, "border_active"),
        colorValue(colors, "border_inactive"), colorValue(colors, "text_primary"),
        colorValue(colors, "text_muted"),      colorValue(colors, "selection"),
        colorValue(colors, "focus_ring"),      colorValue(colors, "danger"),
        colorValue(colors, "warning"),         colorValue(colors, "success")};
    const auto &materials = object(semantic, "materials");
    theme::Materials materialValues{material(materials, "panel"), material(materials, "launcher"),
                                    material(materials, "notification"),
                                    material(materials, "menu")};
    const auto &border = object(semantic, "border");
    const auto &shadow = object(semantic, "shadow");
    const auto &typography = object(semantic, "typography");
    const auto &motion = object(semantic, "motion");
    constexpr std::array easingValues{std::pair{"linear", theme::MotionEasing::linear},
                                      std::pair{"ease_out", theme::MotionEasing::easeOut},
                                      std::pair{"ease_in_out", theme::MotionEasing::easeInOut},
                                      std::pair{"crisp", theme::MotionEasing::crisp}};
    theme::SemanticTokens semanticValue{
        std::move(colorValueSet),
        std::move(materialValues),
        {numberValue(border, "thickness_px", 1.0, 65535.0),
         numberValue(border, "minimum_contrast_ratio", 1.0, 21.0)},
        {numberValue(shadow, "offset_y_px", 0.0, 65535.0),
         numberValue(shadow, "blur_radius_px", 0.0, 65535.0),
         numberValue(shadow, "opacity", 0.0, 1.0)},
        {stringValue(typography, "body_family", 256U),
         numberValue(typography, "body_size_px", 8.0, 144.0),
         numberValue(typography, "title_size_px", 8.0, 192.0),
         integerValue<std::uint16_t>(typography, "weight_normal", 100U, 900U),
         integerValue<std::uint16_t>(typography, "weight_emphasis", 100U, 900U)},
        exactNumberMap(semantic, "spacing",
                       {{"unit_px", 0.0}, {"control_gap_px", 0.0}, {"content_margin_px", 0.0}}),
        exactNumberMap(semantic, "panel", {{"height_px", 0.0}, {"icon_size_px", 0.0}}),
        exactNumberMap(semantic, "decoration",
                       {{"titlebar_height_px", 0.0}, {"border_width_px", 0.0}}),
        exactNumberMap(semantic, "icon",
                       {{"small_px", 0.0}, {"medium_px", 0.0}, {"large_px", 0.0}}),
        exactNumberMap(semantic, "focus", {{"width_px", 1.0}, {"offset_px", 0.0}}),
        {integerValue<std::uint16_t>(motion, "fast_ms", 0U, 4000U),
         integerValue<std::uint16_t>(motion, "normal_ms", 0U, 4000U),
         enumValue(motion, "easing", easingValues)},
        exactNumberMap(semantic, "targets", {{"minimum_px", 24.0}})};

    const auto &components = object(source, "component");
    theme::ComponentTokens componentValue{
        component(components, "task_button"),     component(components, "launcher_tile"),
        component(components, "titlebar_button"), component(components, "notification_card"),
        component(components, "quick_setting"),   component(components, "tooltip"),
        component(components, "menu_item")};

    const auto &accessibility = object(source, "effective_accessibility");
    theme::EffectiveAccessibility accessibilityValue{
        booleanValue(accessibility, "high_contrast"),
        booleanValue(accessibility, "reduced_motion"),
        booleanValue(accessibility, "transparency_disabled"),
        numberValue(accessibility, "text_scale", 0.75, 3.0),
        numberValue(accessibility, "animation_scale", 0.0, 2.0),
        numberValue(accessibility, "focus_width_px", 1.0, 65535.0),
        numberValue(accessibility, "minimum_target_size_px", 24.0, 65535.0),
        numberValue(accessibility, "minimum_contrast_ratio", 1.0, 21.0)};

    const auto &fallbacks = object(source, "capability_fallbacks");
    theme::CapabilityFallbacks fallbackValues{stringValue(fallbacks, "blur", 256U),
                                              stringValue(fallbacks, "thumbnails", 256U)};
    const auto &resolved = object(source, "resolved_materials");
    theme::ResolvedMaterials resolvedValues{
        resolvedMaterial(resolved, "panel"), resolvedMaterial(resolved, "launcher"),
        resolvedMaterial(resolved, "notification"), resolvedMaterial(resolved, "menu")};

    constexpr std::array thumbnailValues{
        std::pair{"provided_thumbnail", theme::ThumbnailPresentation::provided_thumbnail},
        std::pair{"application_icon_title_state",
                  theme::ThumbnailPresentation::application_icon_title_state}};
    constexpr std::array warningValues{
        std::pair{"accent_suppressed_high_contrast",
                  theme::ThemeWarning::accent_suppressed_high_contrast},
        std::pair{"blur_fallback_active", theme::ThemeWarning::blur_fallback_active},
        std::pair{"thumbnail_fallback_active", theme::ThemeWarning::thumbnail_fallback_active},
        std::pair{"safe_mode_active", theme::ThemeWarning::safe_mode_active}};

    return {1U,
            themeProfile,
            displayName,
            sources,
            std::move(primitiveValue),
            std::move(semanticValue),
            std::move(componentValue),
            std::move(accessibilityValue),
            std::move(fallbackValues),
            std::move(resolvedValues),
            enumValue(source, "thumbnail_presentation", thumbnailValues),
            enumArray(source, "warnings", 0U, 4U, warningValues)};
}

[[nodiscard]] config::ConfigurationSource configurationSource(const Json &document) {
    constexpr std::array values{
        std::pair{"user", config::ConfigurationSource::user},
        std::pair{"last_known_valid", config::ConfigurationSource::last_known_valid},
        std::pair{"packaged_default", config::ConfigurationSource::packaged_default}};
    return enumValue(document, "configuration_source_id", values);
}

[[nodiscard]] std::vector<prismdrake::settings::SettingsWarning>
settingsWarnings(const Json &document) {
    constexpr std::array values{
        std::pair{"invalid_user_configuration",
                  prismdrake::settings::SettingsWarning::invalid_user_configuration},
        std::pair{"invalid_last_known_valid_configuration",
                  prismdrake::settings::SettingsWarning::invalid_last_known_valid_configuration},
        std::pair{"last_known_valid_persistence_failed",
                  prismdrake::settings::SettingsWarning::last_known_valid_persistence_failed}};
    return enumArray(document, "validation_warning_ids", 0U, 3U, values);
}

} // namespace

Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>
parseRuntimeSnapshot(std::uint64_t outerGeneration, std::string_view json) {
    if (outerGeneration == foundation::Generation::unpublishedValue || json.empty() ||
        json.size() > prismdrake::settings::maximumRuntimeSnapshotBytes) {
        return Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>::failure(
            invalidSnapshot(json.size() > prismdrake::settings::maximumRuntimeSnapshotBytes
                                ? ErrorCode::too_large
                                : ErrorCode::validation_error));
    }

    SnapshotBoundsSax bounds;
    if (!Json::sax_parse(json.begin(), json.end(), &bounds)) {
        return Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>::failure(
            invalidSnapshot(ErrorCode::syntax_error));
    }

    try {
        const auto document = Json::parse(json.begin(), json.end());
        if (!document.is_object() ||
            integerValue<std::uint32_t>(document, "schema_version", 1U, 1U) !=
                prismdrake::settings::runtimeSnapshotSchemaVersion) {
            return Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>::failure(
                invalidSnapshot(ErrorCode::unsupported));
        }
        const auto embeddedGeneration = integerValue<std::uint64_t>(
            document, "generation", 1U, std::numeric_limits<std::uint64_t>::max());
        if (embeddedGeneration != outerGeneration) {
            throw std::domain_error("matching generation expected");
        }
        const auto generation = foundation::Generation::fromPublished(outerGeneration);
        if (!generation) {
            throw std::domain_error("published generation expected");
        }

        const auto activeProfile = profile(document);
        prismdrake::settings::SettingsCandidate candidate{
            configuration(document, activeProfile),
            {configurationSource(document), booleanValue(document, "runtime_profile_override")},
            themeCandidate(document, activeProfile),
            settingsWarnings(document)};

        const auto &restartDomains = document.at("restart_required_domains");
        if (!restartDomains.is_array() || !restartDomains.empty()) {
            throw std::domain_error("empty restart domains expected");
        }

        auto canonical =
            prismdrake::settings::serializeRuntimeSnapshot(generation.value(), candidate);
        if (!canonical || canonical.value().generation != outerGeneration ||
            canonical.value().json != json) {
            throw std::domain_error("canonical snapshot expected");
        }

        return Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>::success(
            std::make_shared<const prismdrake::settings::SettingsSnapshot>(
                prismdrake::settings::SettingsSnapshot{
                    prismdrake::settings::runtimeSnapshotSchemaVersion, generation.value(),
                    std::move(candidate), std::string{json}}));
    } catch (const std::exception &) {
        return Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>::failure(
            invalidSnapshot());
    }
}

} // namespace prismdrake::shell::settings
