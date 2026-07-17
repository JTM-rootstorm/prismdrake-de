#include "ConfigurationParser.hpp"

#include "BuildInfo.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::config {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::uint32_t supportedSchemaVersion = 1U;

[[nodiscard]] std::size_t utf8CodePointCount(std::string_view value) noexcept {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
}

[[nodiscard]] Error syntaxError(std::optional<toml::source_position> position = std::nullopt) {
    std::ostringstream message;
    message << "Configuration TOML syntax is invalid";
    if (position.has_value()) {
        message << " at line " << position->line << ", column " << position->column;
    }
    message << '.';
    return {ErrorCode::syntax_error, message.str(),
            "Review the TOML structure, UTF-8 encoding, and duplicate keys."};
}

// These internal helpers pair a TOML lookup name with its canonical schema path or recovery text;
// every call uses named literals, so retaining the compact signature keeps the schema readable.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Error semanticError(std::string_view path, std::string_view recovery) {
    const std::string canonical_path =
        path.starts_with('$') ? std::string(path) : "$." + std::string(path);
    return {ErrorCode::validation_error,
            "Configuration value is invalid at " + canonical_path + '.', std::string(recovery)};
}

[[nodiscard]] Error unsupportedVersionError() {
    return {ErrorCode::unsupported, "Configuration value is unsupported at $.schema_version.",
            "Use configuration schema_version 1."};
}

[[nodiscard]] Error missingFieldError(std::string_view path) {
    return semanticError(path, "Add the required field using the documented version-1 type.");
}

[[nodiscard]] Error wrongTypeError(std::string_view path, std::string_view type) {
    return semanticError(path,
                         "Use the documented " + std::string(type) + " type without coercion.");
}

[[nodiscard]] Error unknownKeyError(std::string_view table_path) {
    return semanticError(
        table_path.empty() ? "$" : table_path,
        "Remove the unknown key; configuration version 1 is closed to extensions.");
}

template <std::size_t Size>
[[nodiscard]] Result<void> validateTableShape(const toml::table &table, std::string_view path,
                                              const std::array<std::string_view, Size> &keys) {
    for (const auto &[key, unused] : table) {
        (void)unused;
        if (std::find(keys.begin(), keys.end(), key.str()) == keys.end()) {
            return Result<void>::failure(unknownKeyError(path));
        }
    }
    for (const auto key : keys) {
        if (!table.contains(key)) {
            const std::string field_path =
                path.empty() ? std::string(key) : std::string(path) + '.' + std::string(key);
            return Result<void>::failure(missingFieldError(field_path));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validateNodeBounds(const toml::node &node, std::size_t depth,
                                              std::size_t &nodes, std::string_view path) {
    ++nodes;
    if (nodes > maximumConfigurationNodes) {
        return Result<void>::failure(
            semanticError("$", "Reduce the configuration to at most 256 values and containers."));
    }
    if (const auto *table = node.as_table()) {
        if (depth > maximumConfigurationNesting) {
            return Result<void>::failure(
                semanticError(path, "Use only the documented top-level fields and domain tables."));
        }
        if (table->size() > maximumConfigurationTableEntries) {
            return Result<void>::failure(
                semanticError(path, "Reduce the table to at most 32 entries."));
        }
        for (const auto &[key, child] : *table) {
            (void)key;
            // Generic limit diagnostics intentionally name only the containing table. Unknown
            // user-controlled keys must never be reflected into diagnostic output.
            auto result = validateNodeBounds(child, depth + 1U, nodes, path);
            if (!result) {
                return result;
            }
        }
    } else if (const auto *array = node.as_array()) {
        if (depth > maximumConfigurationNesting) {
            return Result<void>::failure(
                semanticError(path, "Use only the documented top-level fields and domain tables."));
        }
        if (array->size() > maximumConfigurationArrayItems) {
            return Result<void>::failure(
                semanticError(path, "Reduce the array to at most 64 entries."));
        }
        for (const auto &child : *array) {
            auto result = validateNodeBounds(child, depth + 1U, nodes, path);
            if (!result) {
                return result;
            }
        }
    }

    return Result<void>::success();
}

[[nodiscard]] Result<const toml::table *> readTable(const toml::table &parent,
                                                    std::string_view key) {
    const auto *node = parent.get(key);
    if (node == nullptr) {
        return Result<const toml::table *>::failure(missingFieldError(key));
    }
    const auto *table = node->as_table();
    if (table == nullptr) {
        return Result<const toml::table *>::failure(wrongTypeError(key, "table"));
    }
    return Result<const toml::table *>::success(table);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<std::string> readString(const toml::table &table, std::string_view key,
                                             std::string_view path) {
    const auto *node = table.get(key);
    if (node == nullptr) {
        return Result<std::string>::failure(missingFieldError(path));
    }
    const auto *value = node->as_string();
    if (value == nullptr) {
        return Result<std::string>::failure(wrongTypeError(path, "string"));
    }
    const auto text = value->get();
    if (text.empty() || text.size() > maximumConfigurationStringBytes ||
        utf8CodePointCount(text) > maximumConfigurationStringCodePoints) {
        return Result<std::string>::failure(semanticError(
            path, "Use a non-empty UTF-8 string no longer than 128 Unicode code points."));
    }
    return Result<std::string>::success(text);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<bool> readBoolean(const toml::table &table, std::string_view key,
                                       std::string_view path) {
    const auto *node = table.get(key);
    if (node == nullptr) {
        return Result<bool>::failure(missingFieldError(path));
    }
    const auto *value = node->as_boolean();
    if (value == nullptr) {
        return Result<bool>::failure(wrongTypeError(path, "boolean"));
    }
    return Result<bool>::success(value->get());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<std::int64_t> readInteger(const toml::table &table, std::string_view key,
                                               std::string_view path, std::int64_t minimum,
                                               std::int64_t maximum) {
    const auto *node = table.get(key);
    if (node == nullptr) {
        return Result<std::int64_t>::failure(missingFieldError(path));
    }
    const auto *value = node->as_integer();
    if (value == nullptr) {
        return Result<std::int64_t>::failure(wrongTypeError(path, "integer"));
    }
    const auto number = value->get();
    if (number < minimum || number > maximum) {
        return Result<std::int64_t>::failure(
            semanticError(path, "Use a value within the documented version-1 range."));
    }
    return Result<std::int64_t>::success(number);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<double> readNumber(const toml::table &table, std::string_view key,
                                        std::string_view path, double minimum, double maximum) {
    const auto *node = table.get(key);
    if (node == nullptr) {
        return Result<double>::failure(missingFieldError(path));
    }

    double number = 0.0;
    if (const auto *floating = node->as_floating_point()) {
        number = floating->get();
    } else if (const auto *integer = node->as_integer()) {
        number = static_cast<double>(integer->get());
    } else {
        return Result<double>::failure(wrongTypeError(path, "number"));
    }

    if (!std::isfinite(number) || number < minimum || number > maximum) {
        return Result<double>::failure(
            semanticError(path, "Use a finite value within the documented version-1 range."));
    }
    return Result<double>::success(number);
}

template <typename Enum, std::size_t Size>
[[nodiscard]] Result<Enum>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
readEnum(const toml::table &table, std::string_view key, std::string_view path,
         const std::array<std::pair<std::string_view, Enum>, Size> &values) {
    auto text = readString(table, key, path);
    if (!text) {
        return Result<Enum>::failure(std::move(text).error());
    }
    const auto match = std::find_if(values.begin(), values.end(), [&](const auto &candidate) {
        return candidate.first == text.value();
    });
    if (match == values.end()) {
        return Result<Enum>::failure(
            semanticError(path, "Use one of the documented version-1 identifiers."));
    }
    return Result<Enum>::success(match->second);
}

template <typename Enum, std::size_t Size>
[[nodiscard]] Result<std::vector<Enum>>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
readEnumArray(const toml::table &table, std::string_view key, std::string_view path,
              std::size_t maximum_items,
              const std::array<std::pair<std::string_view, Enum>, Size> &values) {
    const auto *node = table.get(key);
    if (node == nullptr) {
        return Result<std::vector<Enum>>::failure(missingFieldError(path));
    }
    const auto *array = node->as_array();
    if (array == nullptr) {
        return Result<std::vector<Enum>>::failure(wrongTypeError(path, "array"));
    }
    if (array->size() > maximum_items) {
        return Result<std::vector<Enum>>::failure(
            semanticError(path, "Reduce the array to the documented item limit."));
    }

    std::vector<Enum> result;
    result.reserve(array->size());
    std::set<Enum> seen;
    for (const auto &item : *array) {
        const auto *string = item.as_string();
        if (string == nullptr || string->get().empty() ||
            string->get().size() > maximumConfigurationStringBytes ||
            utf8CodePointCount(string->get()) > maximumConfigurationStringCodePoints) {
            return Result<std::vector<Enum>>::failure(
                semanticError(path, "Use only documented string identifiers in the array."));
        }
        const auto match = std::find_if(values.begin(), values.end(), [&](const auto &candidate) {
            return candidate.first == string->get();
        });
        if (match == values.end()) {
            return Result<std::vector<Enum>>::failure(
                semanticError(path, "Use only documented version-1 identifiers."));
        }
        if (!seen.insert(match->second).second) {
            return Result<std::vector<Enum>>::failure(
                semanticError(path, "Remove duplicate array entries."));
        }
        result.push_back(match->second);
    }
    return Result<std::vector<Enum>>::success(std::move(result));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Result<std::vector<std::string>>
readStringArray(const toml::table &table,
                std::string_view key, // NOLINT(bugprone-easily-swappable-parameters)
                std::string_view path, std::size_t maximum_items) {
    const auto *node = table.get(key);
    if (node == nullptr) {
        return Result<std::vector<std::string>>::failure(missingFieldError(path));
    }
    const auto *array = node->as_array();
    if (array == nullptr) {
        return Result<std::vector<std::string>>::failure(wrongTypeError(path, "array"));
    }
    if (array->size() > maximum_items) {
        return Result<std::vector<std::string>>::failure(
            semanticError(path, "Reduce the array to the documented item limit."));
    }

    std::vector<std::string> result;
    result.reserve(array->size());
    std::set<std::string> seen;
    for (const auto &item : *array) {
        const auto *string = item.as_string();
        if (string == nullptr || string->get().empty() ||
            string->get().size() > maximumConfigurationStringBytes ||
            utf8CodePointCount(string->get()) > maximumConfigurationStringCodePoints) {
            return Result<std::vector<std::string>>::failure(semanticError(
                path, "Use non-empty UTF-8 strings no longer than 128 Unicode code points."));
        }
        std::string value = string->get();
        if (!seen.insert(value).second) {
            return Result<std::vector<std::string>>::failure(
                semanticError(path, "Remove duplicate array entries."));
        }
        result.push_back(std::move(value));
    }
    return Result<std::vector<std::string>>::success(std::move(result));
}

template <typename T> [[nodiscard]] Result<T> forwardFailure(const Error &error) {
    return Result<T>::failure(error);
}

} // namespace

struct ParsedConfiguration::Impl final {
    explicit Impl(toml::table parsed_table) : table(std::move(parsed_table)) {}

    toml::table table;
};

ParsedConfiguration::ParsedConfiguration(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

ParsedConfiguration::ParsedConfiguration(ParsedConfiguration &&) noexcept = default;
ParsedConfiguration &ParsedConfiguration::operator=(ParsedConfiguration &&) noexcept = default;
ParsedConfiguration::~ParsedConfiguration() = default;

Result<ParsedConfiguration> parseConfigurationSyntax(std::string_view input) {
    if (input.empty() || std::all_of(input.begin(), input.end(), [](char value) {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n';
        })) {
        return Result<ParsedConfiguration>::failure(syntaxError());
    }
    if (input.size() > maximumConfigurationBytes) {
        return Result<ParsedConfiguration>::failure(
            {ErrorCode::too_large, "Configuration input exceeds the 1 MiB limit.",
             "Reduce the configuration file to at most 1 MiB."});
    }

    try {
        auto table = toml::parse(input);
        return Result<ParsedConfiguration>::success(
            ParsedConfiguration(std::make_unique<ParsedConfiguration::Impl>(std::move(table))));
    } catch (const toml::parse_error &error) {
        return Result<ParsedConfiguration>::failure(syntaxError(error.source().begin));
    } catch (const std::exception &) {
        return Result<ParsedConfiguration>::failure(
            {ErrorCode::io_error, "Configuration parser failed safely.",
             "Retry validation or report the parser failure without attaching private input."});
    }
}

Result<Configuration> validateConfiguration(const ParsedConfiguration &document,
                                            ConfigurationParseOptions options) {
    if (document.implementation_ == nullptr) {
        return Result<Configuration>::failure(
            semanticError("$", "Parse the configuration before semantic validation."));
    }

    const auto &root = document.implementation_->table;
    std::size_t node_count = 0U;
    auto bounds = validateNodeBounds(root, 0U, node_count, "$");
    if (!bounds) {
        return forwardFailure<Configuration>(bounds.error());
    }

    auto schema_version = readInteger(root, "schema_version", "schema_version",
                                      std::numeric_limits<std::int64_t>::min(),
                                      std::numeric_limits<std::int64_t>::max());
    if (!schema_version) {
        return forwardFailure<Configuration>(schema_version.error());
    }
    // Explicit version dispatch is the migration boundary. Version 1 is an identity path;
    // every other version fails without an implicit or lossy migration.
    if (schema_version.value() != supportedSchemaVersion) {
        return Result<Configuration>::failure(unsupportedVersionError());
    }

    constexpr std::array root_keys = {
        std::string_view{"schema_version"}, std::string_view{"profile"},
        std::string_view{"appearance"},     std::string_view{"panel"},
        std::string_view{"launcher"},       std::string_view{"notifications"},
        std::string_view{"desktop"},        std::string_view{"integration"},
        std::string_view{"accessibility"},  std::string_view{"keyboard"},
        std::string_view{"developer"},
    };
    auto root_shape = validateTableShape(root, "", root_keys);
    if (!root_shape) {
        return forwardFailure<Configuration>(root_shape.error());
    }

    constexpr std::array profile_values = {
        std::pair{std::string_view{"lustre"}, Profile::lustre},
        std::pair{std::string_view{"forge"}, Profile::forge},
    };
    auto profile = readEnum(root, "profile", "profile", profile_values);
    if (!profile) {
        return forwardFailure<Configuration>(profile.error());
    }

    auto appearance_table = readTable(root, "appearance");
    if (!appearance_table) {
        return forwardFailure<Configuration>(appearance_table.error());
    }
    constexpr std::array appearance_keys = {
        std::string_view{"accent"},        std::string_view{"transparency_enabled"},
        std::string_view{"blur_quality"},  std::string_view{"reduced_motion"},
        std::string_view{"high_contrast"}, std::string_view{"text_scale"},
        std::string_view{"cursor_theme"},  std::string_view{"cursor_size_px"},
        std::string_view{"icon_theme"},
    };
    auto appearance_shape =
        validateTableShape(*appearance_table.value(), "appearance", appearance_keys);
    if (!appearance_shape) {
        return forwardFailure<Configuration>(appearance_shape.error());
    }
    auto accent = readString(*appearance_table.value(), "accent", "appearance.accent");
    auto transparency = readBoolean(*appearance_table.value(), "transparency_enabled",
                                    "appearance.transparency_enabled");
    constexpr std::array blur_values = {
        std::pair{std::string_view{"off"}, BlurQuality::off},
        std::pair{std::string_view{"low"}, BlurQuality::low},
        std::pair{std::string_view{"balanced"}, BlurQuality::balanced},
        std::pair{std::string_view{"high"}, BlurQuality::high},
    };
    auto blur =
        readEnum(*appearance_table.value(), "blur_quality", "appearance.blur_quality", blur_values);
    auto reduced_motion =
        readBoolean(*appearance_table.value(), "reduced_motion", "appearance.reduced_motion");
    auto high_contrast =
        readBoolean(*appearance_table.value(), "high_contrast", "appearance.high_contrast");
    auto text_scale =
        readNumber(*appearance_table.value(), "text_scale", "appearance.text_scale", 0.75, 3.0);
    auto cursor_theme =
        readString(*appearance_table.value(), "cursor_theme", "appearance.cursor_theme");
    auto cursor_size = readInteger(*appearance_table.value(), "cursor_size_px",
                                   "appearance.cursor_size_px", 16, 256);
    auto icon_theme = readString(*appearance_table.value(), "icon_theme", "appearance.icon_theme");
    if (!accent || !transparency || !blur || !reduced_motion || !high_contrast || !text_scale ||
        !cursor_theme || !cursor_size || !icon_theme) {
        const Error *error = !accent           ? &accent.error()
                             : !transparency   ? &transparency.error()
                             : !blur           ? &blur.error()
                             : !reduced_motion ? &reduced_motion.error()
                             : !high_contrast  ? &high_contrast.error()
                             : !text_scale     ? &text_scale.error()
                             : !cursor_theme   ? &cursor_theme.error()
                             : !cursor_size    ? &cursor_size.error()
                                               : &icon_theme.error();
        return forwardFailure<Configuration>(*error);
    }
    const auto valid_hex = [](unsigned char value) {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F') ||
               (value >= 'a' && value <= 'f');
    };
    if (accent.value().size() != 7U || accent.value().front() != '#' ||
        !std::all_of(accent.value().begin() + 1, accent.value().end(), valid_hex)) {
        return Result<Configuration>::failure(
            semanticError("appearance.accent", "Use an RGB color in #RRGGBB form."));
    }
    auto panel_table = readTable(root, "panel");
    if (!panel_table) {
        return forwardFailure<Configuration>(panel_table.error());
    }
    constexpr std::array panel_keys = {
        std::string_view{"edge"},         std::string_view{"size_px"},
        std::string_view{"autohide"},     std::string_view{"grouping"},
        std::string_view{"clock_format"}, std::string_view{"outputs"},
    };
    auto panel_shape = validateTableShape(*panel_table.value(), "panel", panel_keys);
    if (!panel_shape) {
        return forwardFailure<Configuration>(panel_shape.error());
    }
    constexpr std::array edge_values = {
        std::pair{std::string_view{"top"}, PanelEdge::top},
        std::pair{std::string_view{"right"}, PanelEdge::right},
        std::pair{std::string_view{"bottom"}, PanelEdge::bottom},
        std::pair{std::string_view{"left"}, PanelEdge::left},
    };
    constexpr std::array autohide_values = {
        std::pair{std::string_view{"never"}, AutohideMode::never},
        std::pair{std::string_view{"always"}, AutohideMode::always},
        std::pair{std::string_view{"overlap"}, AutohideMode::overlap},
    };
    constexpr std::array grouping_values = {
        std::pair{std::string_view{"always"}, GroupingMode::always},
        std::pair{std::string_view{"when_full"}, GroupingMode::when_full},
        std::pair{std::string_view{"never"}, GroupingMode::never},
    };
    constexpr std::array clock_values = {
        std::pair{std::string_view{"locale"}, ClockFormat::locale},
        std::pair{std::string_view{"12_hour"}, ClockFormat::hour_12},
        std::pair{std::string_view{"24_hour"}, ClockFormat::hour_24},
    };
    auto edge = readEnum(*panel_table.value(), "edge", "panel.edge", edge_values);
    auto panel_size = readInteger(*panel_table.value(), "size_px", "panel.size_px", 24, 192);
    auto autohide = readEnum(*panel_table.value(), "autohide", "panel.autohide", autohide_values);
    auto grouping = readEnum(*panel_table.value(), "grouping", "panel.grouping", grouping_values);
    auto clock = readEnum(*panel_table.value(), "clock_format", "panel.clock_format", clock_values);
    auto outputs = readStringArray(*panel_table.value(), "outputs", "panel.outputs", 32U);
    if (!edge || !panel_size || !autohide || !grouping || !clock || !outputs) {
        const Error *error = !edge         ? &edge.error()
                             : !panel_size ? &panel_size.error()
                             : !autohide   ? &autohide.error()
                             : !grouping   ? &grouping.error()
                             : !clock      ? &clock.error()
                                           : &outputs.error();
        return forwardFailure<Configuration>(*error);
    }

    auto launcher_table = readTable(root, "launcher");
    if (!launcher_table) {
        return forwardFailure<Configuration>(launcher_table.error());
    }
    constexpr std::array launcher_keys = {std::string_view{"layout"},
                                          std::string_view{"search_providers"},
                                          std::string_view{"recent_items"}};
    auto launcher_shape = validateTableShape(*launcher_table.value(), "launcher", launcher_keys);
    if (!launcher_shape) {
        return forwardFailure<Configuration>(launcher_shape.error());
    }
    constexpr std::array layout_values = {
        std::pair{std::string_view{"compact"}, LauncherLayout::compact},
        std::pair{std::string_view{"expanded"}, LauncherLayout::expanded},
    };
    constexpr std::array provider_values = {
        std::pair{std::string_view{"applications"}, SearchProvider::applications},
        std::pair{std::string_view{"settings"}, SearchProvider::settings},
        std::pair{std::string_view{"recent_files"}, SearchProvider::recent_files},
    };
    constexpr std::array recent_values = {
        std::pair{std::string_view{"disabled"}, RecentItemsPolicy::disabled},
        std::pair{std::string_view{"local_only"}, RecentItemsPolicy::local_only},
    };
    auto layout = readEnum(*launcher_table.value(), "layout", "launcher.layout", layout_values);
    auto providers = readEnumArray(*launcher_table.value(), "search_providers",
                                   "launcher.search_providers", 32U, provider_values);
    auto recent =
        readEnum(*launcher_table.value(), "recent_items", "launcher.recent_items", recent_values);
    if (!layout || !providers || !recent) {
        const Error *error = !layout      ? &layout.error()
                             : !providers ? &providers.error()
                                          : &recent.error();
        return forwardFailure<Configuration>(*error);
    }
    auto notifications_table = readTable(root, "notifications");
    if (!notifications_table) {
        return forwardFailure<Configuration>(notifications_table.error());
    }
    constexpr std::array notifications_keys = {
        std::string_view{"enabled"}, std::string_view{"history"},
        std::string_view{"default_timeout_ms"}, std::string_view{"do_not_disturb"}};
    auto notifications_shape =
        validateTableShape(*notifications_table.value(), "notifications", notifications_keys);
    if (!notifications_shape) {
        return forwardFailure<Configuration>(notifications_shape.error());
    }
    constexpr std::array history_values = {
        std::pair{std::string_view{"disabled"}, NotificationHistory::disabled},
        std::pair{std::string_view{"session"}, NotificationHistory::session},
        std::pair{std::string_view{"persistent"}, NotificationHistory::persistent},
    };
    auto notifications_enabled =
        readBoolean(*notifications_table.value(), "enabled", "notifications.enabled");
    auto history =
        readEnum(*notifications_table.value(), "history", "notifications.history", history_values);
    auto timeout = readInteger(*notifications_table.value(), "default_timeout_ms",
                               "notifications.default_timeout_ms", 0, 120000);
    auto do_not_disturb =
        readBoolean(*notifications_table.value(), "do_not_disturb", "notifications.do_not_disturb");
    if (!notifications_enabled || !history || !timeout || !do_not_disturb) {
        const Error *error = !notifications_enabled ? &notifications_enabled.error()
                             : !history             ? &history.error()
                             : !timeout             ? &timeout.error()
                                                    : &do_not_disturb.error();
        return forwardFailure<Configuration>(*error);
    }

    auto desktop_table = readTable(root, "desktop");
    if (!desktop_table) {
        return forwardFailure<Configuration>(desktop_table.error());
    }
    constexpr std::array desktop_keys = {std::string_view{"wallpaper_mode"},
                                         std::string_view{"icons_enabled"}};
    auto desktop_shape = validateTableShape(*desktop_table.value(), "desktop", desktop_keys);
    if (!desktop_shape) {
        return forwardFailure<Configuration>(desktop_shape.error());
    }
    constexpr std::array wallpaper_values = {
        std::pair{std::string_view{"solid"}, WallpaperMode::solid},
        std::pair{std::string_view{"center"}, WallpaperMode::center},
        std::pair{std::string_view{"fit"}, WallpaperMode::fit},
        std::pair{std::string_view{"fill"}, WallpaperMode::fill},
        std::pair{std::string_view{"stretch"}, WallpaperMode::stretch},
        std::pair{std::string_view{"tile"}, WallpaperMode::tile},
    };
    auto wallpaper = readEnum(*desktop_table.value(), "wallpaper_mode", "desktop.wallpaper_mode",
                              wallpaper_values);
    auto icons = readBoolean(*desktop_table.value(), "icons_enabled", "desktop.icons_enabled");
    if (!wallpaper || !icons) {
        return forwardFailure<Configuration>(!wallpaper ? wallpaper.error() : icons.error());
    }

    auto integration_table = readTable(root, "integration");
    if (!integration_table) {
        return forwardFailure<Configuration>(integration_table.error());
    }
    constexpr std::array integration_keys = {
        std::string_view{"export_gtk"}, std::string_view{"export_qt"},
        std::string_view{"export_xsettings"}, std::string_view{"export_portal"}};
    auto integration_shape =
        validateTableShape(*integration_table.value(), "integration", integration_keys);
    if (!integration_shape) {
        return forwardFailure<Configuration>(integration_shape.error());
    }
    auto export_gtk =
        readBoolean(*integration_table.value(), "export_gtk", "integration.export_gtk");
    auto export_qt = readBoolean(*integration_table.value(), "export_qt", "integration.export_qt");
    auto export_xsettings =
        readBoolean(*integration_table.value(), "export_xsettings", "integration.export_xsettings");
    auto export_portal =
        readBoolean(*integration_table.value(), "export_portal", "integration.export_portal");
    if (!export_gtk || !export_qt || !export_xsettings || !export_portal) {
        const Error *error = !export_gtk         ? &export_gtk.error()
                             : !export_qt        ? &export_qt.error()
                             : !export_xsettings ? &export_xsettings.error()
                                                 : &export_portal.error();
        return forwardFailure<Configuration>(*error);
    }

    auto accessibility_table = readTable(root, "accessibility");
    if (!accessibility_table) {
        return forwardFailure<Configuration>(accessibility_table.error());
    }
    constexpr std::array accessibility_keys = {std::string_view{"focus_emphasis"},
                                               std::string_view{"animation_scale"},
                                               std::string_view{"minimum_target_size_px"}};
    auto accessibility_shape =
        validateTableShape(*accessibility_table.value(), "accessibility", accessibility_keys);
    if (!accessibility_shape) {
        return forwardFailure<Configuration>(accessibility_shape.error());
    }
    constexpr std::array focus_values = {
        std::pair{std::string_view{"standard"}, FocusEmphasis::standard},
        std::pair{std::string_view{"strong"}, FocusEmphasis::strong},
    };
    auto focus = readEnum(*accessibility_table.value(), "focus_emphasis",
                          "accessibility.focus_emphasis", focus_values);
    auto animation_scale = readNumber(*accessibility_table.value(), "animation_scale",
                                      "accessibility.animation_scale", 0.0, 2.0);
    auto target_size = readInteger(*accessibility_table.value(), "minimum_target_size_px",
                                   "accessibility.minimum_target_size_px", 24, 96);
    if (!focus || !animation_scale || !target_size) {
        const Error *error = !focus             ? &focus.error()
                             : !animation_scale ? &animation_scale.error()
                                                : &target_size.error();
        return forwardFailure<Configuration>(*error);
    }
    auto keyboard_table = readTable(root, "keyboard");
    if (!keyboard_table) {
        return forwardFailure<Configuration>(keyboard_table.error());
    }
    constexpr std::array keyboard_keys = {std::string_view{"menu_key_opens_launcher"},
                                          std::string_view{"focus_wraps"}};
    auto keyboard_shape = validateTableShape(*keyboard_table.value(), "keyboard", keyboard_keys);
    if (!keyboard_shape) {
        return forwardFailure<Configuration>(keyboard_shape.error());
    }
    auto menu_key = readBoolean(*keyboard_table.value(), "menu_key_opens_launcher",
                                "keyboard.menu_key_opens_launcher");
    auto focus_wraps = readBoolean(*keyboard_table.value(), "focus_wraps", "keyboard.focus_wraps");
    if (!menu_key || !focus_wraps) {
        return forwardFailure<Configuration>(!menu_key ? menu_key.error() : focus_wraps.error());
    }

    auto developer_table = readTable(root, "developer");
    if (!developer_table) {
        return forwardFailure<Configuration>(developer_table.error());
    }
    constexpr std::array developer_keys = {std::string_view{"diagnostics_enabled"},
                                           std::string_view{"mock_capability_overrides"}};
    auto developer_shape =
        validateTableShape(*developer_table.value(), "developer", developer_keys);
    if (!developer_shape) {
        return forwardFailure<Configuration>(developer_shape.error());
    }
    auto diagnostics = readBoolean(*developer_table.value(), "diagnostics_enabled",
                                   "developer.diagnostics_enabled");
    auto capability_overrides =
        readStringArray(*developer_table.value(), "mock_capability_overrides",
                        "developer.mock_capability_overrides", 64U);
    if (!diagnostics || !capability_overrides) {
        return forwardFailure<Configuration>(!diagnostics ? diagnostics.error()
                                                          : capability_overrides.error());
    }
    const bool developer_settings_enabled =
        options.developerSettingsPolicy == DeveloperSettingsPolicy::developer &&
        foundation::developerOverridesEnabled();

    return Result<Configuration>::success(Configuration{
        supportedSchemaVersion,
        profile.value(),
        Appearance{std::move(accent).value(), transparency.value(), blur.value(),
                   reduced_motion.value(), high_contrast.value(), text_scale.value(),
                   std::move(cursor_theme).value(), static_cast<std::uint16_t>(cursor_size.value()),
                   std::move(icon_theme).value()},
        Panel{edge.value(), static_cast<std::uint16_t>(panel_size.value()), autohide.value(),
              grouping.value(), clock.value(), std::move(outputs).value()},
        Launcher{layout.value(), std::move(providers).value(), recent.value()},
        Notifications{notifications_enabled.value(), history.value(),
                      static_cast<std::uint32_t>(timeout.value()), do_not_disturb.value()},
        Desktop{wallpaper.value(), icons.value()},
        Integration{export_gtk.value(), export_qt.value(), export_xsettings.value(),
                    export_portal.value()},
        Accessibility{focus.value(), animation_scale.value(),
                      static_cast<std::uint16_t>(target_size.value())},
        Keyboard{menu_key.value(), focus_wraps.value()},
        Developer{developer_settings_enabled && diagnostics.value(),
                  developer_settings_enabled ? std::move(capability_overrides).value()
                                             : std::vector<std::string>{}},
    });
}

Result<Configuration> parseConfigurationToml(std::string_view input,
                                             ConfigurationParseOptions options) {
    auto parsed = parseConfigurationSyntax(input);
    if (!parsed) {
        return Result<Configuration>::failure(std::move(parsed).error());
    }
    return validateConfiguration(parsed.value(), options);
}

} // namespace prismdrake::config
