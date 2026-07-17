#include "ThemeParser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::theme {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using Json = nlohmann::json;

constexpr std::uint32_t supportedSchemaVersion = 1U;

[[nodiscard]] Error syntaxError() {
    return {ErrorCode::syntax_error, "Theme JSON syntax is invalid.",
            "Review the JSON structure, UTF-8 encoding, and duplicate object keys."};
}

// Canonical paths and recovery guidance are fixed by callers; input keys and values are never
// included in diagnostics.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Error validationError(std::string_view path, std::string_view recovery) {
    return {ErrorCode::validation_error, "Theme value is invalid at " + std::string(path) + '.',
            std::string(recovery)};
}

[[nodiscard]] Error unsupportedVersionError() {
    return {ErrorCode::unsupported, "Theme value is unsupported at $.schema_version.",
            "Use theme schema_version 1."};
}

[[nodiscard]] Error unknownKeyError(std::string_view path) {
    return validationError(
        path, "Remove the unknown key; theme document version 1 is closed to extensions.");
}

[[nodiscard]] Error missingFieldError(std::string_view path) {
    return validationError(path, "Add the required field using the documented version-1 type.");
}

[[nodiscard]] Error wrongTypeError(std::string_view path, std::string_view type) {
    return validationError(path, "Use the documented " + std::string(type) + " type.");
}

[[nodiscard]] std::size_t utf8CodePointCount(std::string_view value) noexcept {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
}

/// Allocation-bounding SAX pass run before constructing the DOM.
class ThemeBoundsSax final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return scalar(); }
    bool boolean(bool value) override {
        (void)value;
        return scalar();
    }
    bool number_integer(number_integer_t value) override {
        (void)value;
        return scalar();
    }
    bool number_unsigned(number_unsigned_t value) override {
        (void)value;
        return scalar();
    }
    bool number_float(number_float_t value, const string_t &token) override {
        (void)token;
        if (!std::isfinite(value)) {
            return fail(syntaxError());
        }
        return scalar();
    }
    bool string(string_t &value) override {
        if (utf8CodePointCount(value) > maximumThemeStringCodePoints) {
            return fail(
                validationError("$", "Reduce each string to at most 256 Unicode code points."));
        }
        return scalar();
    }
    bool binary(binary_t &value) override {
        (void)value;
        return fail(syntaxError());
    }

    bool start_object(std::size_t elements) override {
        if (!containerValue() || stack_.size() > maximumThemeNesting ||
            (elements != std::numeric_limits<std::size_t>::max() &&
             elements > maximumThemeObjectEntries)) {
            if (!failure_.has_value()) {
                return fail(validationError(
                    "$", "Reduce object nesting and entries to the documented limits."));
            }
            return false;
        }
        stack_.push_back(Frame{true, 0U, {}});
        return true;
    }

    bool key(string_t &value) override {
        if (stack_.empty() || !stack_.back().object) {
            return fail(syntaxError());
        }
        auto &frame = stack_.back();
        ++frame.items;
        if (frame.items > maximumThemeObjectEntries) {
            return fail(validationError("$", "Reduce each object to at most 256 entries."));
        }
        if (utf8CodePointCount(value) > maximumThemeStringCodePoints) {
            return fail(
                validationError("$", "Reduce each key to at most 256 Unicode code points."));
        }
        if (!frame.keys.insert(value).second) {
            return fail(syntaxError());
        }
        return true;
    }

    bool end_object() override { return endContainer(true); }

    bool start_array(std::size_t elements) override {
        if (!containerValue() || stack_.size() > maximumThemeNesting ||
            (elements != std::numeric_limits<std::size_t>::max() &&
             elements > maximumThemeArrayItems)) {
            if (!failure_.has_value()) {
                return fail(validationError(
                    "$", "Reduce array nesting and items to the documented limits."));
            }
            return false;
        }
        stack_.push_back(Frame{false, 0U, {}});
        return true;
    }

    bool end_array() override { return endContainer(false); }

    bool parse_error(std::size_t position, const std::string &lastToken,
                     const nlohmann::detail::exception &exception) override {
        (void)position;
        (void)lastToken;
        (void)exception;
        return false;
    }

    [[nodiscard]] const std::optional<Error> &failure() const noexcept { return failure_; }

  private:
    struct Frame final {
        bool object;
        std::size_t items;
        std::set<std::string, std::less<>> keys;
    };

    bool fail(Error error) {
        if (!failure_.has_value()) {
            failure_.emplace(std::move(error));
        }
        return false;
    }

    bool addNode() {
        ++nodes_;
        if (nodes_ > maximumThemeNodes) {
            return fail(validationError(
                "$", "Reduce the theme document to at most 4096 values and containers."));
        }
        return true;
    }

    bool noteArrayItem() {
        if (stack_.empty() || stack_.back().object) {
            return true;
        }
        auto &frame = stack_.back();
        ++frame.items;
        if (frame.items > maximumThemeArrayItems) {
            return fail(validationError("$", "Reduce each array to at most 64 entries."));
        }
        return true;
    }

    bool scalar() {
        if (stack_.size() > maximumThemeNesting) {
            return fail(
                validationError("$", "Reduce the theme document nesting to at most 16 levels."));
        }
        return addNode() && noteArrayItem();
    }
    bool containerValue() { return addNode() && noteArrayItem(); }

    bool endContainer(bool object) {
        if (stack_.empty() || stack_.back().object != object) {
            return fail(syntaxError());
        }
        stack_.pop_back();
        return true;
    }

    std::size_t nodes_ = 0U;
    std::vector<Frame> stack_;
    std::optional<Error> failure_;
};

[[nodiscard]] Result<void> validateTreeBounds(const Json &node, std::size_t depth,
                                              std::size_t &nodes) {
    ++nodes;
    if (nodes > maximumThemeNodes) {
        return Result<void>::failure(validationError(
            "$", "Reduce the theme document to at most 4096 values and containers."));
    }
    if (depth > maximumThemeNesting) {
        return Result<void>::failure(
            validationError("$", "Reduce the theme document nesting to at most 16 levels."));
    }
    if (node.is_object()) {
        if (node.size() > maximumThemeObjectEntries) {
            return Result<void>::failure(
                validationError("$", "Reduce each object to at most 256 entries."));
        }
        for (const auto &[unused, child] : node.items()) {
            (void)unused;
            auto result = validateTreeBounds(child, depth + 1U, nodes);
            if (!result) {
                return result;
            }
        }
    } else if (node.is_array()) {
        if (node.size() > maximumThemeArrayItems) {
            return Result<void>::failure(
                validationError("$", "Reduce each array to at most 64 entries."));
        }
        for (const auto &child : node) {
            auto result = validateTreeBounds(child, depth + 1U, nodes);
            if (!result) {
                return result;
            }
        }
    } else if (node.is_string()) {
        const auto &value = node.get_ref<const std::string &>();
        if (utf8CodePointCount(value) > maximumThemeStringCodePoints) {
            return Result<void>::failure(
                validationError("$", "Reduce each string to at most 256 Unicode code points."));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validateObjectShape(const Json &value, std::string_view path,
                                               std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) {
        return Result<void>::failure(wrongTypeError(path, "object"));
    }
    for (const auto &[key, unused] : value.items()) {
        (void)unused;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            return Result<void>::failure(unknownKeyError(path));
        }
    }
    for (const auto key : keys) {
        if (!value.contains(key)) {
            return Result<void>::failure(
                missingFieldError(std::string(path) + '.' + std::string(key)));
        }
    }
    return Result<void>::success();
}

// The internal readers pair a fixed schema key with its canonical path at every call site. Keeping
// both literals adjacent makes the complete contract auditable without reflecting input keys.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<std::string> readString(const Json &object, std::string_view key,
                                             std::string_view path) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return Result<std::string>::failure(missingFieldError(path));
    }
    if (!iterator->is_string()) {
        return Result<std::string>::failure(wrongTypeError(path, "string"));
    }
    const auto &value = iterator->get_ref<const std::string &>();
    if (value.empty() || utf8CodePointCount(value) > maximumThemeStringCodePoints) {
        return Result<std::string>::failure(
            validationError(path, "Use a non-empty string of at most 256 Unicode code points."));
    }
    return Result<std::string>::success(value);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<double> readNumber(const Json &object, std::string_view key,
                                        std::string_view path, double minimum,
                                        std::optional<double> maximum = std::nullopt) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return Result<double>::failure(missingFieldError(path));
    }
    if (!iterator->is_number() || iterator->is_boolean()) {
        return Result<double>::failure(wrongTypeError(path, "number"));
    }
    double value = 0.0;
    try {
        value = iterator->get<double>();
    } catch (const Json::exception &) {
        return Result<double>::failure(wrongTypeError(path, "finite number"));
    }
    if (!std::isfinite(value) || value < minimum ||
        (maximum.has_value() && value > maximum.value())) {
        return Result<double>::failure(
            validationError(path, "Use a finite number within the documented range."));
    }
    return Result<double>::success(value);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<std::uint16_t> readInteger(const Json &object, std::string_view key,
                                                std::string_view path, std::uint16_t minimum,
                                                std::uint16_t maximum) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return Result<std::uint16_t>::failure(missingFieldError(path));
    }
    if (!iterator->is_number() || iterator->is_boolean()) {
        return Result<std::uint16_t>::failure(wrongTypeError(path, "integer"));
    }
    double numericValue = 0.0;
    try {
        numericValue = iterator->get<double>();
    } catch (const Json::exception &) {
        return Result<std::uint16_t>::failure(wrongTypeError(path, "integer"));
    }
    if (!std::isfinite(numericValue) || std::trunc(numericValue) != numericValue ||
        numericValue < static_cast<double>(minimum) ||
        numericValue > static_cast<double>(maximum)) {
        return Result<std::uint16_t>::failure(
            validationError(path, "Use an integer within the documented range."));
    }
    return Result<std::uint16_t>::success(static_cast<std::uint16_t>(numericValue));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<bool> readBoolean(const Json &object, std::string_view key,
                                       std::string_view path) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return Result<bool>::failure(missingFieldError(path));
    }
    if (!iterator->is_boolean()) {
        return Result<bool>::failure(wrongTypeError(path, "boolean"));
    }
    return Result<bool>::success(iterator->get<bool>());
}

[[nodiscard]] bool isTokenKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > 64U || key.front() < 'a' || key.front() > 'z') {
        return false;
    }
    return std::all_of(key.begin() + 1, key.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_';
    });
}

[[nodiscard]] std::optional<std::uint8_t> parseHexByte(std::string_view value) noexcept {
    unsigned int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > 255U) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(parsed);
}

[[nodiscard]] Result<Color> parseColorValue(const Json &value, std::string_view path) {
    if (!value.is_string()) {
        return Result<Color>::failure(wrongTypeError(path, "#RRGGBBAA string"));
    }
    const auto &text = value.get_ref<const std::string &>();
    if (text.size() != 9U || text.front() != '#') {
        return Result<Color>::failure(validationError(path, "Use an eight-digit #RRGGBBAA color."));
    }
    const auto red = parseHexByte(std::string_view{text}.substr(1U, 2U));
    const auto green = parseHexByte(std::string_view{text}.substr(3U, 2U));
    const auto blue = parseHexByte(std::string_view{text}.substr(5U, 2U));
    const auto alpha = parseHexByte(std::string_view{text}.substr(7U, 2U));
    if (!red || !green || !blue || !alpha) {
        return Result<Color>::failure(validationError(path, "Use an eight-digit #RRGGBBAA color."));
    }
    const auto packed =
        (static_cast<std::uint32_t>(*red) << 24U) | (static_cast<std::uint32_t>(*green) << 16U) |
        (static_cast<std::uint32_t>(*blue) << 8U) | static_cast<std::uint32_t>(*alpha);
    return Result<Color>::success(Color{packed});
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<Color> readColor(const Json &object, std::string_view key,
                                      std::string_view path) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return Result<Color>::failure(missingFieldError(path));
    }
    return parseColorValue(*iterator, path);
}

[[nodiscard]] Result<NumberMap> parseNumberMap(const Json &value, std::string_view path,
                                               double minimum,
                                               std::optional<double> maximum = std::nullopt) {
    if (!value.is_object()) {
        return Result<NumberMap>::failure(wrongTypeError(path, "object"));
    }
    if (value.empty()) {
        return Result<NumberMap>::failure(
            validationError(path, "Declare at least one named token."));
    }
    NumberMap output;
    for (const auto &[key, number] : value.items()) {
        if (!isTokenKey(key)) {
            return Result<NumberMap>::failure(
                validationError(path, "Use lowercase snake_case token keys up to 64 bytes."));
        }
        Json wrapper{{"value", number}};
        auto parsed = readNumber(wrapper, "value", path, minimum, maximum);
        if (!parsed) {
            return Result<NumberMap>::failure(std::move(parsed).error());
        }
        output.emplace(key, parsed.value());
    }
    return Result<NumberMap>::success(std::move(output));
}

[[nodiscard]] Result<ColorMap> parseColorMap(const Json &value, std::string_view path) {
    if (!value.is_object()) {
        return Result<ColorMap>::failure(wrongTypeError(path, "object"));
    }
    if (value.size() < 4U) {
        return Result<ColorMap>::failure(
            validationError(path, "Declare at least four named color tokens."));
    }
    ColorMap output;
    for (const auto &[key, color] : value.items()) {
        if (!isTokenKey(key)) {
            return Result<ColorMap>::failure(
                validationError(path, "Use lowercase snake_case token keys up to 64 bytes."));
        }
        auto parsed = parseColorValue(color, path);
        if (!parsed) {
            return Result<ColorMap>::failure(std::move(parsed).error());
        }
        output.emplace(key, parsed.value());
    }
    return Result<ColorMap>::success(std::move(output));
}

[[nodiscard]] Result<NumberMap> parseExactMetricMap(const Json &value, std::string_view path,
                                                    std::initializer_list<std::string_view> keys) {
    auto shape = validateObjectShape(value, path, keys);
    if (!shape) {
        return Result<NumberMap>::failure(std::move(shape).error());
    }
    return parseNumberMap(value, path, 0.0, maximumThemeMetricValue);
}

[[nodiscard]] Result<PrimitiveTokens> parsePrimitive(const Json &value) {
    constexpr std::string_view path = "$.primitive";
    auto shape = validateObjectShape(value, path,
                                     {"colors", "spacing_px", "font_families", "font_size_px",
                                      "duration_ms", "radius_px", "opacity"});
    if (!shape) {
        return Result<PrimitiveTokens>::failure(std::move(shape).error());
    }
    auto colors = parseColorMap(value.at("colors"), "$.primitive.colors");
    auto spacing = parseNumberMap(value.at("spacing_px"), "$.primitive.spacing_px", 0.0,
                                  maximumThemeMetricValue);
    auto fontSize = parseNumberMap(value.at("font_size_px"), "$.primitive.font_size_px", 0.0,
                                   maximumThemeMetricValue);
    auto duration = parseNumberMap(value.at("duration_ms"), "$.primitive.duration_ms", 0.0,
                                   maximumThemeMetricValue);
    auto radius = parseNumberMap(value.at("radius_px"), "$.primitive.radius_px", 0.0,
                                 maximumThemeMetricValue);
    auto opacity = parseNumberMap(value.at("opacity"), "$.primitive.opacity", 0.0, 1.0);
    for (auto *result : {&spacing, &fontSize, &duration, &radius, &opacity}) {
        if (!*result) {
            return Result<PrimitiveTokens>::failure(std::move(*result).error());
        }
    }
    if (!colors) {
        return Result<PrimitiveTokens>::failure(std::move(colors).error());
    }
    const auto &familiesValue = value.at("font_families");
    if (!familiesValue.is_array() || familiesValue.empty()) {
        return Result<PrimitiveTokens>::failure(validationError(
            "$.primitive.font_families", "Use a non-empty array of font family strings."));
    }
    std::vector<std::string> families;
    families.reserve(familiesValue.size());
    for (const auto &familyValue : familiesValue) {
        if (!familyValue.is_string()) {
            return Result<PrimitiveTokens>::failure(
                wrongTypeError("$.primitive.font_families", "array of strings"));
        }
        const auto &family = familyValue.get_ref<const std::string &>();
        if (family.empty() || utf8CodePointCount(family) > maximumThemeStringCodePoints) {
            return Result<PrimitiveTokens>::failure(
                validationError("$.primitive.font_families",
                                "Use non-empty strings of at most 256 Unicode code points."));
        }
        families.push_back(family);
    }
    return Result<PrimitiveTokens>::success(
        PrimitiveTokens{std::move(colors).value(), std::move(spacing).value(), std::move(families),
                        std::move(fontSize).value(), std::move(duration).value(),
                        std::move(radius).value(), std::move(opacity).value()});
}

[[nodiscard]] Result<Material> parseMaterial(const Json &value, std::string_view path) {
    auto shape = validateObjectShape(value, path, {"tint", "opacity", "blur_request", "fallback"});
    if (!shape) {
        return Result<Material>::failure(std::move(shape).error());
    }
    auto tint = readColor(value, "tint", std::string(path) + ".tint");
    auto opacity = readNumber(value, "opacity", std::string(path) + ".opacity", 0.0, 1.0);
    if (!tint) {
        return Result<Material>::failure(std::move(tint).error());
    }
    if (!opacity) {
        return Result<Material>::failure(std::move(opacity).error());
    }
    const auto &blurValue = value.at("blur_request");
    auto blurShape = validateObjectShape(blurValue, std::string(path) + ".blur_request",
                                         {"enabled", "radius_px", "saturation"});
    if (!blurShape) {
        return Result<Material>::failure(std::move(blurShape).error());
    }
    auto enabled = readBoolean(blurValue, "enabled", std::string(path) + ".blur_request.enabled");
    auto radius = readNumber(blurValue, "radius_px", std::string(path) + ".blur_request.radius_px",
                             0.0, 128.0);
    auto saturation = readNumber(blurValue, "saturation",
                                 std::string(path) + ".blur_request.saturation", 0.0, 2.0);
    if (!enabled) {
        return Result<Material>::failure(std::move(enabled).error());
    }
    if (!radius) {
        return Result<Material>::failure(std::move(radius).error());
    }
    if (!saturation) {
        return Result<Material>::failure(std::move(saturation).error());
    }

    const auto &fallbackValue = value.at("fallback");
    auto fallbackShape = validateObjectShape(fallbackValue, std::string(path) + ".fallback",
                                             {"kind", "color", "opacity"});
    if (!fallbackShape) {
        return Result<Material>::failure(std::move(fallbackShape).error());
    }
    auto kind = readString(fallbackValue, "kind", std::string(path) + ".fallback.kind");
    auto color = readColor(fallbackValue, "color", std::string(path) + ".fallback.color");
    auto fallbackOpacity =
        readNumber(fallbackValue, "opacity", std::string(path) + ".fallback.opacity", 0.75, 1.0);
    if (!kind) {
        return Result<Material>::failure(std::move(kind).error());
    }
    if (!color) {
        return Result<Material>::failure(std::move(color).error());
    }
    if (!fallbackOpacity) {
        return Result<Material>::failure(std::move(fallbackOpacity).error());
    }
    FallbackKind fallbackKind{};
    if (kind.value() == "alpha") {
        fallbackKind = FallbackKind::alpha;
    } else if (kind.value() == "opaque") {
        fallbackKind = FallbackKind::opaque;
        if ((color.value().rgba & 0xffU) != 0xffU || fallbackOpacity.value() != 1.0) {
            return Result<Material>::failure(
                validationError(std::string(path) + ".fallback",
                                "Use full color alpha and opacity 1 for an opaque fallback."));
        }
    } else {
        return Result<Material>::failure(
            validationError(std::string(path) + ".fallback.kind", "Use alpha or opaque."));
    }
    return Result<Material>::success(
        Material{tint.value(), opacity.value(),
                 BlurRequest{enabled.value(), radius.value(), saturation.value()},
                 MaterialFallback{fallbackKind, color.value(), fallbackOpacity.value()}});
}

[[nodiscard]] Result<SemanticColors> parseSemanticColors(const Json &value) {
    constexpr std::string_view path = "$.semantic.colors";
    constexpr std::array keys{
        std::string_view{"panel_surface"},   std::string_view{"elevated_surface"},
        std::string_view{"window_frame"},    std::string_view{"border_active"},
        std::string_view{"border_inactive"}, std::string_view{"text_primary"},
        std::string_view{"text_muted"},      std::string_view{"selection"},
        std::string_view{"focus_ring"},      std::string_view{"danger"},
        std::string_view{"warning"},         std::string_view{"success"},
    };
    auto shape = validateObjectShape(
        value, path,
        {"panel_surface", "elevated_surface", "window_frame", "border_active", "border_inactive",
         "text_primary", "text_muted", "selection", "focus_ring", "danger", "warning", "success"});
    if (!shape) {
        return Result<SemanticColors>::failure(std::move(shape).error());
    }
    std::vector<Color> colors;
    colors.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto color =
            readColor(value, keys[index], std::string(path) + '.' + std::string(keys[index]));
        if (!color) {
            return Result<SemanticColors>::failure(std::move(color).error());
        }
        colors.push_back(color.value());
    }
    return Result<SemanticColors>::success(
        SemanticColors{colors[0], colors[1], colors[2], colors[3], colors[4], colors[5], colors[6],
                       colors[7], colors[8], colors[9], colors[10], colors[11]});
}

[[nodiscard]] Result<Materials> parseMaterials(const Json &value) {
    constexpr std::string_view path = "$.semantic.materials";
    auto shape = validateObjectShape(value, path, {"panel", "launcher", "notification", "menu"});
    if (!shape) {
        return Result<Materials>::failure(std::move(shape).error());
    }
    auto panel = parseMaterial(value.at("panel"), "$.semantic.materials.panel");
    auto launcher = parseMaterial(value.at("launcher"), "$.semantic.materials.launcher");
    auto notification =
        parseMaterial(value.at("notification"), "$.semantic.materials.notification");
    auto menu = parseMaterial(value.at("menu"), "$.semantic.materials.menu");
    for (auto *result : {&panel, &launcher, &notification, &menu}) {
        if (!*result) {
            return Result<Materials>::failure(std::move(*result).error());
        }
    }
    return Result<Materials>::success(
        Materials{std::move(panel).value(), std::move(launcher).value(),
                  std::move(notification).value(), std::move(menu).value()});
}

[[nodiscard]] Result<ComponentStyle> parseComponentStyle(const Json &value, std::string_view path) {
    auto shape = validateObjectShape(value, path, {"radius_px", "padding_px", "border_width_px"});
    if (!shape) {
        return Result<ComponentStyle>::failure(std::move(shape).error());
    }
    auto radius = readNumber(value, "radius_px", std::string(path) + ".radius_px", 0.0,
                             maximumThemeMetricValue);
    auto padding = readNumber(value, "padding_px", std::string(path) + ".padding_px", 0.0,
                              maximumThemeMetricValue);
    auto border = readNumber(value, "border_width_px", std::string(path) + ".border_width_px", 0.0,
                             maximumThemeMetricValue);
    for (auto *result : {&radius, &padding, &border}) {
        if (!*result) {
            return Result<ComponentStyle>::failure(std::move(*result).error());
        }
    }
    return Result<ComponentStyle>::success(
        ComponentStyle{radius.value(), padding.value(), border.value()});
}

[[nodiscard]] Result<ComponentTokens> parseComponents(const Json &value) {
    constexpr std::array keys{
        std::string_view{"task_button"},     std::string_view{"launcher_tile"},
        std::string_view{"titlebar_button"}, std::string_view{"notification_card"},
        std::string_view{"quick_setting"},   std::string_view{"tooltip"},
        std::string_view{"menu_item"},
    };
    auto shape =
        validateObjectShape(value, "$.component",
                            {"task_button", "launcher_tile", "titlebar_button", "notification_card",
                             "quick_setting", "tooltip", "menu_item"});
    if (!shape) {
        return Result<ComponentTokens>::failure(std::move(shape).error());
    }
    std::vector<ComponentStyle> styles;
    styles.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto style =
            parseComponentStyle(value.at(keys[index]), "$.component." + std::string(keys[index]));
        if (!style) {
            return Result<ComponentTokens>::failure(std::move(style).error());
        }
        styles.push_back(std::move(style).value());
    }
    return Result<ComponentTokens>::success(ComponentTokens{
        styles[0], styles[1], styles[2], styles[3], styles[4], styles[5], styles[6]});
}

[[nodiscard]] Result<SemanticTokens> parseSemantic(const Json &value) {
    constexpr std::string_view path = "$.semantic";
    auto shape =
        validateObjectShape(value, path,
                            {"colors", "materials", "border", "shadow", "typography", "spacing",
                             "panel", "decoration", "icon", "focus", "motion", "targets"});
    if (!shape) {
        return Result<SemanticTokens>::failure(std::move(shape).error());
    }
    auto colors = parseSemanticColors(value.at("colors"));
    auto materials = parseMaterials(value.at("materials"));
    if (!colors) {
        return Result<SemanticTokens>::failure(std::move(colors).error());
    }
    if (!materials) {
        return Result<SemanticTokens>::failure(std::move(materials).error());
    }

    const auto &borderValue = value.at("border");
    auto borderShape = validateObjectShape(borderValue, "$.semantic.border",
                                           {"thickness_px", "minimum_contrast_ratio"});
    if (!borderShape) {
        return Result<SemanticTokens>::failure(std::move(borderShape).error());
    }
    auto thickness =
        readNumber(borderValue, "thickness_px", "$.semantic.border.thickness_px", 1.0, 8.0);
    auto borderContrast = readNumber(borderValue, "minimum_contrast_ratio",
                                     "$.semantic.border.minimum_contrast_ratio", 1.0, 21.0);

    const auto &shadowValue = value.at("shadow");
    auto shadowShape = validateObjectShape(shadowValue, "$.semantic.shadow",
                                           {"offset_y_px", "blur_radius_px", "opacity"});
    if (!shadowShape) {
        return Result<SemanticTokens>::failure(std::move(shadowShape).error());
    }
    auto shadowOffset =
        readNumber(shadowValue, "offset_y_px", "$.semantic.shadow.offset_y_px", 0.0, 64.0);
    auto shadowBlur =
        readNumber(shadowValue, "blur_radius_px", "$.semantic.shadow.blur_radius_px", 0.0, 128.0);
    auto shadowOpacity = readNumber(shadowValue, "opacity", "$.semantic.shadow.opacity", 0.0, 1.0);

    const auto &typographyValue = value.at("typography");
    auto typographyShape = validateObjectShape(
        typographyValue, "$.semantic.typography",
        {"body_family", "body_size_px", "title_size_px", "weight_normal", "weight_emphasis"});
    if (!typographyShape) {
        return Result<SemanticTokens>::failure(std::move(typographyShape).error());
    }
    auto bodyFamily =
        readString(typographyValue, "body_family", "$.semantic.typography.body_family");
    auto bodySize = readNumber(typographyValue, "body_size_px",
                               "$.semantic.typography.body_size_px", 8.0, 48.0);
    auto titleSize = readNumber(typographyValue, "title_size_px",
                                "$.semantic.typography.title_size_px", 8.0, 64.0);
    auto weightNormal = readInteger(typographyValue, "weight_normal",
                                    "$.semantic.typography.weight_normal", 100U, 900U);
    auto weightEmphasis = readInteger(typographyValue, "weight_emphasis",
                                      "$.semantic.typography.weight_emphasis", 100U, 900U);

    auto spacing = parseExactMetricMap(value.at("spacing"), "$.semantic.spacing",
                                       {"content_margin_px", "control_gap_px", "unit_px"});
    auto panel =
        parseExactMetricMap(value.at("panel"), "$.semantic.panel", {"height_px", "icon_size_px"});
    auto decoration = parseExactMetricMap(value.at("decoration"), "$.semantic.decoration",
                                          {"border_width_px", "titlebar_height_px"});
    auto icon = parseExactMetricMap(value.at("icon"), "$.semantic.icon",
                                    {"large_px", "medium_px", "small_px"});
    auto focus =
        parseExactMetricMap(value.at("focus"), "$.semantic.focus", {"offset_px", "width_px"});
    auto targets = parseExactMetricMap(value.at("targets"), "$.semantic.targets", {"minimum_px"});

    const auto &motionValue = value.at("motion");
    auto motionShape =
        validateObjectShape(motionValue, "$.semantic.motion", {"fast_ms", "normal_ms", "easing"});
    if (!motionShape) {
        return Result<SemanticTokens>::failure(std::move(motionShape).error());
    }
    auto fast = readInteger(motionValue, "fast_ms", "$.semantic.motion.fast_ms", 0U, 2000U);
    auto normal = readInteger(motionValue, "normal_ms", "$.semantic.motion.normal_ms", 0U, 2000U);
    auto easingText = readString(motionValue, "easing", "$.semantic.motion.easing");

    const std::array scalarResults{
        static_cast<bool>(thickness),     static_cast<bool>(borderContrast),
        static_cast<bool>(shadowOffset),  static_cast<bool>(shadowBlur),
        static_cast<bool>(shadowOpacity), static_cast<bool>(bodyFamily),
        static_cast<bool>(bodySize),      static_cast<bool>(titleSize),
        static_cast<bool>(weightNormal),  static_cast<bool>(weightEmphasis),
        static_cast<bool>(fast),          static_cast<bool>(normal),
        static_cast<bool>(easingText)};
    if (std::find(scalarResults.begin(), scalarResults.end(), false) != scalarResults.end()) {
        // Return in deterministic schema order.
        if (!thickness)
            return Result<SemanticTokens>::failure(std::move(thickness).error());
        if (!borderContrast)
            return Result<SemanticTokens>::failure(std::move(borderContrast).error());
        if (!shadowOffset)
            return Result<SemanticTokens>::failure(std::move(shadowOffset).error());
        if (!shadowBlur)
            return Result<SemanticTokens>::failure(std::move(shadowBlur).error());
        if (!shadowOpacity)
            return Result<SemanticTokens>::failure(std::move(shadowOpacity).error());
        if (!bodyFamily)
            return Result<SemanticTokens>::failure(std::move(bodyFamily).error());
        if (!bodySize)
            return Result<SemanticTokens>::failure(std::move(bodySize).error());
        if (!titleSize)
            return Result<SemanticTokens>::failure(std::move(titleSize).error());
        if (!weightNormal)
            return Result<SemanticTokens>::failure(std::move(weightNormal).error());
        if (!weightEmphasis)
            return Result<SemanticTokens>::failure(std::move(weightEmphasis).error());
        if (!fast)
            return Result<SemanticTokens>::failure(std::move(fast).error());
        if (!normal)
            return Result<SemanticTokens>::failure(std::move(normal).error());
        return Result<SemanticTokens>::failure(std::move(easingText).error());
    }
    for (auto *result : {&spacing, &panel, &decoration, &icon, &focus, &targets}) {
        if (!*result) {
            return Result<SemanticTokens>::failure(std::move(*result).error());
        }
    }
    if (focus.value().at("width_px") < 1.0) {
        return Result<SemanticTokens>::failure(validationError(
            "$.semantic.focus.width_px", "Use a visible focus width from 1 through 65535 px."));
    }
    if (targets.value().at("minimum_px") < 24.0) {
        return Result<SemanticTokens>::failure(
            validationError("$.semantic.targets.minimum_px",
                            "Use a minimum target size from 24 through 65535 px."));
    }
    MotionEasing easing{};
    if (easingText.value() == "linear") {
        easing = MotionEasing::linear;
    } else if (easingText.value() == "ease_out") {
        easing = MotionEasing::easeOut;
    } else if (easingText.value() == "ease_in_out") {
        easing = MotionEasing::easeInOut;
    } else if (easingText.value() == "crisp") {
        easing = MotionEasing::crisp;
    } else {
        return Result<SemanticTokens>::failure(
            validationError("$.semantic.motion.easing", "Use a documented easing identifier."));
    }
    return Result<SemanticTokens>::success(SemanticTokens{
        std::move(colors).value(), std::move(materials).value(),
        Border{thickness.value(), borderContrast.value()},
        Shadow{shadowOffset.value(), shadowBlur.value(), shadowOpacity.value()},
        Typography{std::move(bodyFamily).value(), bodySize.value(), titleSize.value(),
                   weightNormal.value(), weightEmphasis.value()},
        std::move(spacing).value(), std::move(panel).value(), std::move(decoration).value(),
        std::move(icon).value(), std::move(focus).value(),
        Motion{fast.value(), normal.value(), easing}, std::move(targets).value()});
}

[[nodiscard]] Result<AccessibilityOverrides> parseAccessibility(const Json &value) {
    auto shape = validateObjectShape(
        value, "$.accessibility_overrides",
        {"reduced_motion", "transparency_disabled", "high_contrast", "minimum_target_size_px"});
    if (!shape) {
        return Result<AccessibilityOverrides>::failure(std::move(shape).error());
    }
    const auto &reducedValue = value.at("reduced_motion");
    auto reducedShape = validateObjectShape(
        reducedValue, "$.accessibility_overrides.reduced_motion", {"duration_scale"});
    if (!reducedShape) {
        return Result<AccessibilityOverrides>::failure(std::move(reducedShape).error());
    }
    auto durationScale =
        readNumber(reducedValue, "duration_scale",
                   "$.accessibility_overrides.reduced_motion.duration_scale", 0.0, 1.0);

    const auto &transparencyValue = value.at("transparency_disabled");
    auto transparencyShape = validateObjectShape(
        transparencyValue, "$.accessibility_overrides.transparency_disabled", {"force_opaque"});
    if (!transparencyShape) {
        return Result<AccessibilityOverrides>::failure(std::move(transparencyShape).error());
    }
    auto forceOpaque = readBoolean(transparencyValue, "force_opaque",
                                   "$.accessibility_overrides.transparency_disabled.force_opaque");

    const auto &contrastValue = value.at("high_contrast");
    auto contrastShape =
        validateObjectShape(contrastValue, "$.accessibility_overrides.high_contrast",
                            {"minimum_contrast_ratio", "focus_width_px"});
    if (!contrastShape) {
        return Result<AccessibilityOverrides>::failure(std::move(contrastShape).error());
    }
    auto contrast =
        readNumber(contrastValue, "minimum_contrast_ratio",
                   "$.accessibility_overrides.high_contrast.minimum_contrast_ratio", 1.0, 21.0);
    auto focusWidth =
        readNumber(contrastValue, "focus_width_px",
                   "$.accessibility_overrides.high_contrast.focus_width_px", 1.0, 16.0);
    auto target = readNumber(value, "minimum_target_size_px",
                             "$.accessibility_overrides.minimum_target_size_px", 24.0, 96.0);
    if (!durationScale)
        return Result<AccessibilityOverrides>::failure(std::move(durationScale).error());
    if (!forceOpaque)
        return Result<AccessibilityOverrides>::failure(std::move(forceOpaque).error());
    if (durationScale.value() != 0.0) {
        return Result<AccessibilityOverrides>::failure(
            validationError("$.accessibility_overrides.reduced_motion.duration_scale",
                            "Use zero duration for the version-1 reduced-motion override."));
    }
    if (!forceOpaque.value()) {
        return Result<AccessibilityOverrides>::failure(
            validationError("$.accessibility_overrides.transparency_disabled.force_opaque",
                            "Use the version-1 opaque transparency-disabled fallback."));
    }
    if (!contrast)
        return Result<AccessibilityOverrides>::failure(std::move(contrast).error());
    if (!focusWidth)
        return Result<AccessibilityOverrides>::failure(std::move(focusWidth).error());
    if (!target)
        return Result<AccessibilityOverrides>::failure(std::move(target).error());
    return Result<AccessibilityOverrides>::success(AccessibilityOverrides{
        ReducedMotionOverride{durationScale.value()}, TransparencyOverride{forceOpaque.value()},
        HighContrastOverride{contrast.value(), focusWidth.value()}, target.value()});
}

[[nodiscard]] Result<CapabilityFallbacks> parseCapabilityFallbacks(const Json &value) {
    auto shape = validateObjectShape(value, "$.capability_fallbacks", {"blur", "thumbnails"});
    if (!shape) {
        return Result<CapabilityFallbacks>::failure(std::move(shape).error());
    }
    auto blur = readString(value, "blur", "$.capability_fallbacks.blur");
    auto thumbnails = readString(value, "thumbnails", "$.capability_fallbacks.thumbnails");
    if (!blur) {
        return Result<CapabilityFallbacks>::failure(std::move(blur).error());
    }
    if (!thumbnails) {
        return Result<CapabilityFallbacks>::failure(std::move(thumbnails).error());
    }
    return Result<CapabilityFallbacks>::success(
        CapabilityFallbacks{std::move(blur).value(), std::move(thumbnails).value()});
}

[[nodiscard]] Result<ThemeDocument> validateDocument(const Json &document) {
    auto shape = validateObjectShape(
        document, "$",
        {"schema_version", "layer", "profile_id", "profile_display_name", "generation_model",
         "primitive", "semantic", "component", "accessibility_overrides", "capability_fallbacks"});
    if (!shape) {
        return Result<ThemeDocument>::failure(std::move(shape).error());
    }
    const auto &version = document.at("schema_version");
    if (!version.is_number() || version.is_boolean()) {
        return Result<ThemeDocument>::failure(wrongTypeError("$.schema_version", "integer"));
    }
    double numericVersion = 0.0;
    try {
        numericVersion = version.get<double>();
    } catch (const Json::exception &) {
        return Result<ThemeDocument>::failure(unsupportedVersionError());
    }
    if (!std::isfinite(numericVersion) || std::trunc(numericVersion) != numericVersion) {
        return Result<ThemeDocument>::failure(wrongTypeError("$.schema_version", "integer"));
    }
    if (numericVersion != static_cast<double>(supportedSchemaVersion)) {
        return Result<ThemeDocument>::failure(unsupportedVersionError());
    }

    auto layerText = readString(document, "layer", "$.layer");
    auto generation = readString(document, "generation_model", "$.generation_model");
    if (!layerText) {
        return Result<ThemeDocument>::failure(std::move(layerText).error());
    }
    if (!generation) {
        return Result<ThemeDocument>::failure(std::move(generation).error());
    }
    if (generation.value() != "immutable_snapshot") {
        return Result<ThemeDocument>::failure(validationError(
            "$.generation_model", "Use immutable_snapshot for version-1 theme documents."));
    }
    Layer layer{};
    if (layerText.value() == "base") {
        layer = Layer::base;
    } else if (layerText.value() == "profile") {
        layer = Layer::profile;
    } else if (layerText.value() == "accessibility") {
        layer = Layer::accessibility;
    } else {
        return Result<ThemeDocument>::failure(
            validationError("$.layer", "Use base, profile, or accessibility."));
    }

    std::optional<Profile> profile;
    std::optional<std::string> displayName;
    const auto &profileValue = document.at("profile_id");
    const auto &displayValue = document.at("profile_display_name");
    if (layer == Layer::profile) {
        if (!profileValue.is_string()) {
            return Result<ThemeDocument>::failure(wrongTypeError("$.profile_id", "string"));
        }
        if (!displayValue.is_string()) {
            return Result<ThemeDocument>::failure(
                wrongTypeError("$.profile_display_name", "string"));
        }
        const auto &profileText = profileValue.get_ref<const std::string &>();
        const auto &displayText = displayValue.get_ref<const std::string &>();
        if (profileText == "lustre" && displayText == "Prismdrake Lustre") {
            profile = Profile::lustre;
        } else if (profileText == "forge" && displayText == "Prismdrake Forge") {
            profile = Profile::forge;
        } else {
            return Result<ThemeDocument>::failure(validationError(
                "$.profile_id",
                "Use lustre with Prismdrake Lustre or forge with Prismdrake Forge."));
        }
        displayName = displayText;
    } else if (!profileValue.is_null() || !displayValue.is_null()) {
        return Result<ThemeDocument>::failure(
            validationError("$.profile_id", "Use null profile identity outside a profile layer."));
    }

    auto primitive = parsePrimitive(document.at("primitive"));
    auto semantic = parseSemantic(document.at("semantic"));
    auto component = parseComponents(document.at("component"));
    auto accessibility = parseAccessibility(document.at("accessibility_overrides"));
    auto fallbacks = parseCapabilityFallbacks(document.at("capability_fallbacks"));
    if (!primitive)
        return Result<ThemeDocument>::failure(std::move(primitive).error());
    if (!semantic)
        return Result<ThemeDocument>::failure(std::move(semantic).error());
    if (!component)
        return Result<ThemeDocument>::failure(std::move(component).error());
    if (!accessibility)
        return Result<ThemeDocument>::failure(std::move(accessibility).error());
    if (!fallbacks)
        return Result<ThemeDocument>::failure(std::move(fallbacks).error());
    return Result<ThemeDocument>::success(ThemeDocument{
        supportedSchemaVersion, layer, profile, std::move(displayName),
        std::move(primitive).value(), std::move(semantic).value(), std::move(component).value(),
        std::move(accessibility).value(), std::move(fallbacks).value()});
}

} // namespace

Result<ThemeDocument> parseThemeDocumentJson(std::string_view input) {
    if (input.size() > maximumThemeDocumentBytes) {
        return Result<ThemeDocument>::failure(
            {ErrorCode::too_large, "Theme JSON exceeds the 1 MiB input limit.",
             "Reduce the complete document to at most 1048576 bytes."});
    }
    if (input.empty()) {
        return Result<ThemeDocument>::failure(syntaxError());
    }

    ThemeBoundsSax boundsSax;
    if (!Json::sax_parse(input.begin(), input.end(), &boundsSax, Json::input_format_t::json, true,
                         false)) {
        if (boundsSax.failure().has_value()) {
            return Result<ThemeDocument>::failure(*boundsSax.failure());
        }
        return Result<ThemeDocument>::failure(syntaxError());
    }

    Json document;
    try {
        document = Json::parse(input.begin(), input.end(), nullptr, false, false);
    } catch (const Json::exception &) {
        return Result<ThemeDocument>::failure(syntaxError());
    } catch (const std::exception &) {
        return Result<ThemeDocument>::failure(syntaxError());
    }
    if (document.is_discarded()) {
        return Result<ThemeDocument>::failure(syntaxError());
    }
    std::size_t nodes = 0;
    auto bounds = validateTreeBounds(document, 0U, nodes);
    if (!bounds) {
        return Result<ThemeDocument>::failure(std::move(bounds).error());
    }
    return validateDocument(document);
}

} // namespace prismdrake::theme
