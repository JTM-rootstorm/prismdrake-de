#include "ThemeBundle.hpp"

#include "BoundedFile.hpp"
#include "ThemeParser.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace prismdrake::theme {
namespace {

using foundation::Result;

[[nodiscard]] Result<ThemeDocument> loadDocument(const std::filesystem::path &directory,
                                                 std::string_view filename) {
    auto bytes =
        foundation::readBoundedFile(directory / std::string(filename), maximumThemeDocumentBytes);
    if (!bytes) {
        return Result<ThemeDocument>::failure(std::move(bytes).error());
    }
    return parseThemeDocumentJson(bytes.value());
}

} // namespace

Result<ThemeBundle> loadPackagedThemeBundle(const std::filesystem::path &themeDirectory) {
    auto base = loadDocument(themeDirectory, "base.tokens.json");
    if (!base) {
        return Result<ThemeBundle>::failure(std::move(base).error());
    }
    auto lustre = loadDocument(themeDirectory, "lustre.tokens.json");
    if (!lustre) {
        return Result<ThemeBundle>::failure(std::move(lustre).error());
    }
    auto forge = loadDocument(themeDirectory, "forge.tokens.json");
    if (!forge) {
        return Result<ThemeBundle>::failure(std::move(forge).error());
    }
    auto accessibility = loadDocument(themeDirectory, "accessibility.tokens.json");
    if (!accessibility) {
        return Result<ThemeBundle>::failure(std::move(accessibility).error());
    }

    return Result<ThemeBundle>::success(
        ThemeBundle{std::move(base).value(), std::move(lustre).value(), std::move(forge).value(),
                    std::move(accessibility).value()});
}

} // namespace prismdrake::theme
