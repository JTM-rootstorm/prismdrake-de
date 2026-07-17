#include "DesktopExec.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class FieldCode : std::uint8_t {
    singleFile,
    files,
    singleUrl,
    urls,
    icon,
    name,
    desktopFile,
    deprecated,
};

struct ExecPart final {
    std::string literal;
    std::optional<FieldCode> fieldCode;
};

struct ExecArgument final {
    std::vector<ExecPart> parts;
    bool preserveExplicitEmpty{false};
};

struct ParsedExec final {
    std::vector<ExecArgument> arguments;
    std::optional<FieldCode> targetCode;
};

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool isAsciiLetter(char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

[[nodiscard]] bool isTargetCode(FieldCode code) noexcept {
    return code == FieldCode::singleFile || code == FieldCode::files ||
           code == FieldCode::singleUrl || code == FieldCode::urls;
}

[[nodiscard]] bool isMultiArgumentCode(FieldCode code) noexcept {
    return code == FieldCode::files || code == FieldCode::urls || code == FieldCode::icon;
}

[[nodiscard]] std::optional<FieldCode> decodeFieldCode(char character) noexcept {
    switch (character) {
    case 'f':
        return FieldCode::singleFile;
    case 'F':
        return FieldCode::files;
    case 'u':
        return FieldCode::singleUrl;
    case 'U':
        return FieldCode::urls;
    case 'i':
        return FieldCode::icon;
    case 'c':
        return FieldCode::name;
    case 'k':
        return FieldCode::desktopFile;
    case 'd':
    case 'D':
    case 'n':
    case 'N':
    case 'v':
    case 'm':
        return FieldCode::deprecated;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool isReservedUnquoted(char character) noexcept {
    constexpr std::string_view reserved{"\"'\\><~|&;$*?#()`"};
    return reserved.find(character) != std::string_view::npos;
}

void appendLiteral(ExecArgument &argument, char character) {
    if (argument.parts.empty() || argument.parts.back().fieldCode) {
        argument.parts.push_back(ExecPart{});
    }
    argument.parts.back().literal.push_back(character);
}

void appendFieldCode(ExecArgument &argument, FieldCode code) {
    argument.parts.push_back(ExecPart{{}, code});
}

[[nodiscard]] Result<void> observeTargetCode(ParsedExec &parsed, FieldCode code) {
    if (!isTargetCode(code)) {
        return Result<void>::success();
    }
    if (parsed.targetCode) {
        return Result<void>::failure(
            {ErrorCode::validation_error,
             "A desktop-entry Exec value contains multiple file or URL field codes.",
             "Use at most one of %f, %F, %u, or %U in an Exec value."});
    }
    parsed.targetCode = code;
    return Result<void>::success();
}

[[nodiscard]] Result<ParsedExec> parseExec(std::string_view command) {
    if (command.empty()) {
        return failure<ParsedExec>(ErrorCode::validation_error,
                                   "The desktop-entry Exec value is empty.",
                                   "Provide a nonempty executable and argument vector.");
    }
    if (command.size() > maximumDesktopExecBytes) {
        return failure<ParsedExec>(ErrorCode::too_large,
                                   "The desktop-entry Exec value exceeds its byte limit.",
                                   "Use a smaller bounded Exec value.");
    }
    if (!std::ranges::all_of(command, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x20U && byte <= 0x7EU;
        })) {
        return failure<ParsedExec>(
            ErrorCode::validation_error,
            "The desktop-entry Exec value is not bounded printable ASCII.",
            "Use printable ASCII in Exec and supply non-ASCII text only through field expansion.");
    }

    ParsedExec parsed;
    std::size_t offset = 0U;
    while (offset < command.size()) {
        while (offset < command.size() && command[offset] == ' ') {
            ++offset;
        }
        if (offset == command.size()) {
            break;
        }
        if (parsed.arguments.size() >= maximumDesktopExecArguments) {
            return failure<ParsedExec>(ErrorCode::too_large,
                                       "The desktop-entry Exec value has too many arguments.",
                                       "Use a smaller bounded argument vector.");
        }

        ExecArgument argument;
        if (command[offset] == '"') {
            argument.preserveExplicitEmpty = true;
            ++offset;
            bool closed = false;
            while (offset < command.size()) {
                const char character = command[offset++];
                if (character == '"') {
                    closed = true;
                    break;
                }
                if (character == '\\') {
                    if (offset == command.size()) {
                        return failure<ParsedExec>(
                            ErrorCode::syntax_error,
                            "A quoted desktop-entry Exec argument has an incomplete escape.",
                            "Escape only a quote, backtick, dollar sign, or backslash in quotes.");
                    }
                    const char escaped = command[offset++];
                    if (escaped != '"' && escaped != '`' && escaped != '$' && escaped != '\\') {
                        return failure<ParsedExec>(
                            ErrorCode::syntax_error,
                            "A quoted desktop-entry Exec argument has an invalid escape.",
                            "Escape only a quote, backtick, dollar sign, or backslash in quotes.");
                    }
                    appendLiteral(argument, escaped);
                    continue;
                }
                if (character == '`' || character == '$') {
                    return failure<ParsedExec>(
                        ErrorCode::syntax_error,
                        "A quoted desktop-entry Exec argument contains an unescaped reserved "
                        "character.",
                        "Escape backticks and dollar signs inside quoted Exec arguments.");
                }
                if (character != '%') {
                    appendLiteral(argument, character);
                    continue;
                }
                if (offset < command.size() && command[offset] == '%') {
                    ++offset;
                    appendLiteral(argument, '%');
                    continue;
                }
                if (offset < command.size() && isAsciiLetter(command[offset])) {
                    return failure<ParsedExec>(
                        ErrorCode::validation_error,
                        "A desktop-entry field code appears inside a quoted Exec argument.",
                        "Place field codes in unquoted arguments and quote only literal "
                        "arguments.");
                }
                return failure<ParsedExec>(
                    ErrorCode::syntax_error,
                    "A desktop-entry Exec value contains an invalid percent escape.",
                    "Use %% for a literal percent or a supported field code.");
            }
            if (!closed) {
                return failure<ParsedExec>(ErrorCode::syntax_error,
                                           "A desktop-entry Exec argument has an unmatched quote.",
                                           "Close the whole double-quoted argument.");
            }
            if (offset < command.size() && command[offset] != ' ') {
                return failure<ParsedExec>(
                    ErrorCode::syntax_error,
                    "A desktop-entry Exec argument uses partial or concatenated quoting.",
                    "Quote the complete argument and separate arguments with spaces.");
            }
        } else {
            while (offset < command.size() && command[offset] != ' ') {
                const char character = command[offset++];
                if (character == '"') {
                    return failure<ParsedExec>(
                        ErrorCode::syntax_error,
                        "A desktop-entry Exec argument uses partial or concatenated quoting.",
                        "Quote the complete argument and separate arguments with spaces.");
                }
                if (isReservedUnquoted(character)) {
                    return failure<ParsedExec>(
                        ErrorCode::syntax_error,
                        "A desktop-entry Exec argument contains an unquoted reserved character.",
                        "Quote complete arguments that contain reserved characters.");
                }
                if (character != '%') {
                    appendLiteral(argument, character);
                    continue;
                }
                if (offset < command.size() && command[offset] == '%') {
                    ++offset;
                    appendLiteral(argument, '%');
                    continue;
                }
                if (offset == command.size() || !isAsciiLetter(command[offset])) {
                    return failure<ParsedExec>(
                        ErrorCode::syntax_error,
                        "A desktop-entry Exec value contains an invalid percent escape.",
                        "Use %% for a literal percent or a supported field code.");
                }
                const auto code = decodeFieldCode(command[offset++]);
                if (!code) {
                    return failure<ParsedExec>(
                        ErrorCode::unsupported,
                        "A desktop-entry Exec value contains an unsupported field code.",
                        "Use only field codes defined by Desktop Entry Specification 1.5.");
                }
                auto observed = observeTargetCode(parsed, *code);
                if (!observed) {
                    return Result<ParsedExec>::failure(observed.error());
                }
                appendFieldCode(argument, *code);
            }
        }

        parsed.arguments.push_back(std::move(argument));
    }

    if (parsed.arguments.empty()) {
        return failure<ParsedExec>(ErrorCode::validation_error,
                                   "The desktop-entry Exec value has no executable.",
                                   "Provide a nonempty executable as the first argument.");
    }
    const auto &executable = parsed.arguments.front();
    if (std::ranges::any_of(executable.parts,
                            [](const ExecPart &part) { return part.fieldCode.has_value(); })) {
        return failure<ParsedExec>(
            ErrorCode::validation_error, "The desktop-entry Exec executable contains a field code.",
            "Use a fixed executable name or path; only %% is allowed in argv[0].");
    }
    std::string executableText;
    for (const auto &part : executable.parts) {
        executableText.append(part.literal);
    }
    if (executableText.empty() || executableText.find('=') != std::string::npos) {
        return failure<ParsedExec>(
            ErrorCode::validation_error,
            "The desktop-entry Exec executable is empty or contains an equals sign.",
            "Use a nonempty executable name or path without an equals sign.");
    }

    for (const auto &argument : parsed.arguments) {
        const auto fieldCount = std::ranges::count_if(
            argument.parts, [](const ExecPart &part) { return part.fieldCode.has_value(); });
        if (fieldCount == 0) {
            continue;
        }
        for (const auto &part : argument.parts) {
            if (!part.fieldCode || !isMultiArgumentCode(*part.fieldCode)) {
                continue;
            }
            if (argument.parts.size() != 1U || fieldCount != 1) {
                return failure<ParsedExec>(
                    ErrorCode::validation_error,
                    "A multi-argument desktop-entry field code is not a standalone argument.",
                    "Use %F, %U, and %i only as complete standalone arguments.");
            }
        }
    }
    return Result<ParsedExec>::success(std::move(parsed));
}

[[nodiscard]] Result<void> validateExpansionContext(const DesktopExecExpansionContext &context) {
    if (context.targets.size() > maximumDesktopExecTargets ||
        context.targets.size() > maximumDesktopExecInvocations) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "The desktop-entry expansion has too many targets.",
                                      "Use a smaller bounded file or URL selection."});
    }
    if (context.targetKind == DesktopExecTargetKind::none && !context.targets.empty()) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument,
             "The desktop-entry expansion has targets without a target kind.",
             "Classify supplied targets as local files or URIs."});
    }
    if (context.targetKind != DesktopExecTargetKind::none &&
        context.targetKind != DesktopExecTargetKind::localFiles &&
        context.targetKind != DesktopExecTargetKind::uris) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The desktop-entry target kind is invalid.",
                                      "Use none, local files, or URIs."});
    }

    std::size_t targetBytes = 0U;
    for (const auto &target : context.targets) {
        if (target.empty() || target.find('\0') != std::string::npos) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "A desktop-entry expansion target is invalid.",
                                          "Use nonempty NUL-free file paths or URIs."});
        }
        if (target.size() > maximumDesktopExecTargetBytes ||
            target.size() > maximumDesktopExecTargetVectorBytes - targetBytes) {
            return Result<void>::failure(
                {ErrorCode::too_large,
                 "The desktop-entry expansion targets exceed their byte limit.",
                 "Use fewer or smaller file paths or URIs."});
        }
        targetBytes += target.size();
    }
    if (context.desktopFileLocation &&
        context.desktopFileLocation->find('\0') != std::string::npos) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The desktop-entry file location is invalid.",
             "Use a bounded NUL-free local path or URI for desktop-file expansion."});
    }
    if (context.desktopFileLocation &&
        context.desktopFileLocation->size() > maximumDesktopExecLocationBytes) {
        return Result<void>::failure(
            {ErrorCode::too_large, "The desktop-entry file location exceeds its byte limit.",
             "Use a smaller bounded local path or URI for desktop-file expansion."});
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> appendArgument(DesktopExecInvocation &invocation, std::string argument) {
    if (invocation.argv.size() >= maximumDesktopExecArguments) {
        return Result<void>::failure(
            {ErrorCode::too_large,
             "The expanded desktop-entry argument vector has too many entries.",
             "Use fewer arguments or expansion targets."});
    }
    if (argument.find('\0') != std::string::npos) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "An expanded desktop-entry argument is invalid.",
                                      "Use NUL-free arguments and expansion inputs."});
    }
    if (argument.size() > maximumDesktopExecArgumentBytes) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "An expanded desktop-entry argument exceeds its byte limit.",
                                      "Use smaller arguments and expansion inputs."});
    }
    std::size_t existingBytes = 0U;
    for (const auto &existing : invocation.argv) {
        existingBytes += existing.size();
    }
    if (argument.size() > maximumDesktopExecArgumentVectorBytes - existingBytes) {
        return Result<void>::failure(
            {ErrorCode::too_large,
             "The expanded desktop-entry argument vector exceeds its byte limit.",
             "Use fewer or smaller arguments and expansion inputs."});
    }
    invocation.argv.push_back(std::move(argument));
    return Result<void>::success();
}

[[nodiscard]] std::string_view targetForInvocation(const DesktopExecExpansionContext &context,
                                                   std::size_t invocationIndex) {
    return context.targets[invocationIndex];
}

[[nodiscard]] Result<void> expandArgument(const ExecArgument &argument, const DesktopEntry &entry,
                                          const DesktopExecExpansionContext &context,
                                          std::size_t invocationIndex,
                                          DesktopExecInvocation &invocation) {
    if (argument.parts.size() == 1U && argument.parts.front().fieldCode) {
        switch (*argument.parts.front().fieldCode) {
        case FieldCode::files:
        case FieldCode::urls:
            for (const auto &target : context.targets) {
                auto appended = appendArgument(invocation, target);
                if (!appended) {
                    return appended;
                }
            }
            return Result<void>::success();
        case FieldCode::icon:
            if (!entry.icon || entry.icon->empty()) {
                return Result<void>::success();
            }
            if (entry.icon->find('\0') != std::string::npos) {
                return Result<void>::failure({ErrorCode::invalid_argument,
                                              "The desktop-entry Icon expansion is invalid.",
                                              "Use a bounded NUL-free Icon value."});
            }
            if (auto appended = appendArgument(invocation, "--icon"); !appended) {
                return appended;
            }
            return appendArgument(invocation, *entry.icon);
        default:
            break;
        }
    }

    std::string expanded;
    bool removedOnlyContent = true;
    bool preserveEmpty = argument.preserveExplicitEmpty;
    for (const auto &part : argument.parts) {
        if (!part.literal.empty()) {
            if (part.literal.size() > maximumDesktopExecArgumentBytes - expanded.size()) {
                return Result<void>::failure(
                    {ErrorCode::too_large,
                     "An expanded desktop-entry argument exceeds its byte limit.",
                     "Use smaller arguments and expansion inputs."});
            }
            expanded.append(part.literal);
            removedOnlyContent = false;
        }
        if (!part.fieldCode) {
            continue;
        }
        std::string_view replacement;
        switch (*part.fieldCode) {
        case FieldCode::singleFile:
        case FieldCode::singleUrl:
            if (!context.targets.empty()) {
                replacement = targetForInvocation(context, invocationIndex);
                removedOnlyContent = false;
            }
            break;
        case FieldCode::name:
            if (!entry.name || entry.name->empty() || entry.name->find('\0') != std::string::npos) {
                return Result<void>::failure(
                    {ErrorCode::validation_error,
                     "The desktop-entry Name expansion is unavailable or invalid.",
                     "Provide a nonempty bounded NUL-free application Name."});
            }
            replacement = *entry.name;
            removedOnlyContent = false;
            break;
        case FieldCode::desktopFile:
            preserveEmpty = true;
            if (context.desktopFileLocation) {
                replacement = *context.desktopFileLocation;
                if (!replacement.empty()) {
                    removedOnlyContent = false;
                }
            }
            break;
        case FieldCode::deprecated:
            break;
        case FieldCode::files:
        case FieldCode::urls:
        case FieldCode::icon:
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "A multi-argument desktop-entry field code is not standalone.",
                 "Use %F, %U, and %i only as complete standalone arguments."});
        }
        if (replacement.size() > maximumDesktopExecArgumentBytes - expanded.size()) {
            return Result<void>::failure(
                {ErrorCode::too_large, "An expanded desktop-entry argument exceeds its byte limit.",
                 "Use smaller arguments and expansion inputs."});
        }
        expanded.append(replacement);
    }
    if (expanded.empty() && removedOnlyContent && !preserveEmpty) {
        return Result<void>::success();
    }
    return appendArgument(invocation, std::move(expanded));
}

} // namespace

Result<std::vector<DesktopExecInvocation>>
expandDesktopExec(const DesktopEntry &entry, const DesktopExecExpansionContext &context) {
    if (entry.dbusActivatable) {
        return failure<std::vector<DesktopExecInvocation>>(
            ErrorCode::unsupported,
            "A D-Bus-activatable desktop entry cannot use process Exec expansion.",
            "Use the separate D-Bus application activation path.");
    }
    if (!entry.exec) {
        return failure<std::vector<DesktopExecInvocation>>(
            ErrorCode::validation_error, "The desktop entry has no Exec value.",
            "Provide a parsed nonempty Exec value for process activation.");
    }
    auto contextValidation = validateExpansionContext(context);
    if (!contextValidation) {
        return Result<std::vector<DesktopExecInvocation>>::failure(contextValidation.error());
    }
    auto parsed = parseExec(*entry.exec);
    if (!parsed) {
        return Result<std::vector<DesktopExecInvocation>>::failure(parsed.error());
    }

    const auto targetCode = parsed.value().targetCode;
    if (!context.targets.empty() && !targetCode) {
        return failure<std::vector<DesktopExecInvocation>>(
            ErrorCode::validation_error,
            "Desktop-entry expansion targets were supplied without a file or URL field code.",
            "Launch without targets or add one supported file or URL field code.");
    }
    if (!context.targets.empty() &&
        (targetCode == FieldCode::singleFile || targetCode == FieldCode::files) &&
        context.targetKind != DesktopExecTargetKind::localFiles) {
        return failure<std::vector<DesktopExecInvocation>>(
            ErrorCode::unsupported,
            "A file field code cannot expand the supplied non-local targets.",
            "Materialize local files first or use a URL field code.");
    }

    std::size_t invocationCount = 1U;
    if (!context.targets.empty() &&
        (targetCode == FieldCode::singleFile || targetCode == FieldCode::singleUrl)) {
        invocationCount = context.targets.size();
    }
    if (invocationCount > maximumDesktopExecInvocations) {
        return failure<std::vector<DesktopExecInvocation>>(
            ErrorCode::too_large, "Desktop-entry expansion produces too many invocations.",
            "Use a smaller bounded file or URL selection.");
    }

    std::vector<DesktopExecInvocation> invocations;
    invocations.reserve(invocationCount);
    for (std::size_t invocationIndex = 0U; invocationIndex < invocationCount; ++invocationIndex) {
        DesktopExecInvocation invocation;
        for (const auto &argument : parsed.value().arguments) {
            auto expanded = expandArgument(argument, entry, context, invocationIndex, invocation);
            if (!expanded) {
                return Result<std::vector<DesktopExecInvocation>>::failure(expanded.error());
            }
        }
        if (invocation.argv.empty() || invocation.argv.front().empty()) {
            return failure<std::vector<DesktopExecInvocation>>(
                ErrorCode::validation_error,
                "Desktop-entry expansion did not produce a valid executable argument.",
                "Use a fixed nonempty executable as argv[0].");
        }
        invocations.push_back(std::move(invocation));
    }
    return Result<std::vector<DesktopExecInvocation>>::success(std::move(invocations));
}

} // namespace prismdrake::launcher
