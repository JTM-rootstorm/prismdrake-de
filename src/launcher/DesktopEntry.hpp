#pragma once

#include <optional>
#include <string>
#include <vector>

namespace prismdrake::launcher {

/// Parsed, display-free fields from an Application desktop entry.
///
/// Discovery, desktop-file identifier precedence, visibility evaluation, Exec
/// tokenization, process execution, and filesystem access are deliberately
/// outside this value type. A hidden entry may be a minimal tombstone and thus
/// omit the fields required by a launchable Application entry.
struct DesktopEntry final {
    std::optional<std::string> name;
    std::optional<std::string> genericName;
    std::optional<std::string> comment;
    std::optional<std::string> icon;
    std::vector<std::string> keywords;
    std::vector<std::string> categories;
    std::optional<std::string> exec;
    std::optional<std::string> tryExec;
    std::optional<std::string> path;
    bool terminal{false};
    bool hidden{false};
    bool noDisplay{false};
    bool dbusActivatable{false};
    std::optional<std::vector<std::string>> onlyShowIn;
    std::optional<std::vector<std::string>> notShowIn;

    friend bool operator==(const DesktopEntry &, const DesktopEntry &) = default;
};

} // namespace prismdrake::launcher
