#include "ApplicationIdentity.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view desktopSuffix = ".desktop";
constexpr std::size_t maximumAssociationTokenBytes = 4096U;

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure({code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool validUtf8(std::string_view value) noexcept {
    for (std::size_t index = 0U; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t width = 0U;
        std::uint32_t codepoint = 0U;
        if (lead <= 0x7fU) {
            width = 1U;
            codepoint = lead;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            width = 2U;
            codepoint = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            width = 3U;
            codepoint = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            width = 4U;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (index + width > value.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset < width; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if ((width == 3U && codepoint < 0x800U) || (width == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU ||
            codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
            return false;
        }
        index += width;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> normalized(std::string_view value,
                                                    bool removeDesktopSuffix = false) {
    if (removeDesktopSuffix && value.ends_with(desktopSuffix)) {
        value.remove_suffix(desktopSuffix.size());
    }
    if (value.empty() || value.size() > maximumAssociationTokenBytes ||
        value.find('\0') != std::string_view::npos || !validUtf8(value)) {
        return std::nullopt;
    }
    std::string result{value};
    for (char &character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::size_t>
matchesStartup(const std::vector<CatalogApplicationIdentity> &catalog, std::string_view token) {
    std::vector<std::size_t> matches;
    for (std::size_t index = 0U; index < catalog.size(); ++index) {
        if (catalog[index].startupWmClass) {
            const auto candidate = normalized(*catalog[index].startupWmClass);
            if (candidate && *candidate == token) {
                matches.push_back(index);
            }
        }
    }
    return matches;
}

[[nodiscard]] std::vector<std::size_t>
matchesDesktopFileId(const std::vector<CatalogApplicationIdentity> &catalog,
                     std::string_view token) {
    std::vector<std::size_t> matches;
    for (std::size_t index = 0U; index < catalog.size(); ++index) {
        const auto candidate = normalized(catalog[index].desktopFileId.value(), true);
        if (candidate && *candidate == token) {
            matches.push_back(index);
        }
    }
    return matches;
}

} // namespace

ApplicationId ApplicationId::desktopEntry(const DesktopFileId &desktopFileId) {
    return {ApplicationIdKind::desktopEntry, desktopFileId.value()};
}

Result<ApplicationId> ApplicationId::wmClassFallback(std::string_view normalizedWmClass) {
    const auto checked = normalized(normalizedWmClass);
    if (!checked || checked->size() > maximumAssociationTokenBytes) {
        return failure<ApplicationId>(ErrorCode::validation_error,
                                      "The WM_CLASS fallback identity is invalid.",
                                      "Use bounded normalized WM_CLASS evidence.");
    }
    return Result<ApplicationId>::success(
        {ApplicationIdKind::wmClassFallback, "wm-class:" + *checked});
}

ApplicationId ApplicationId::unknown() {
    return {ApplicationIdKind::unknown, "unknown-application"};
}

Result<ObservedWindowIdentity> ObservedWindowIdentity::fromObservation(std::uint32_t xid,
                                                                       std::uint64_t incarnation) {
    if (xid == 0U || incarnation == 0U) {
        return failure<ObservedWindowIdentity>(
            ErrorCode::invalid_argument, "The copied window lifetime identity is invalid.",
            "Copy a nonzero XID and decoder-assigned incarnation from one observation.");
    }
    return Result<ObservedWindowIdentity>::success({xid, incarnation});
}

Result<std::shared_ptr<const ApplicationAssociationSnapshot>>
associateApplications(const std::vector<CatalogApplicationIdentity> &catalog,
                      const std::vector<WindowAssociationObservation> &windows,
                      std::uint64_t generation) {
    if (generation == 0U) {
        return failure<std::shared_ptr<const ApplicationAssociationSnapshot>>(
            ErrorCode::invalid_argument, "The association generation is invalid.",
            "Publish a complete snapshot with a nonzero generation.");
    }
    if (catalog.size() > maximumAssociationCatalogEntries ||
        windows.size() > maximumAssociationWindows) {
        return failure<std::shared_ptr<const ApplicationAssociationSnapshot>>(
            ErrorCode::too_large, "The application association input is too large.",
            "Keep catalog and window observations within their documented bounds.");
    }

    std::set<std::string> desktopIds;
    for (const auto &application : catalog) {
        if (!desktopIds.insert(application.desktopFileId.value()).second ||
            (application.startupWmClass && !normalized(*application.startupWmClass))) {
            return failure<std::shared_ptr<const ApplicationAssociationSnapshot>>(
                ErrorCode::validation_error, "The application identity catalog is invalid.",
                "Use unique identifiers and bounded StartupWMClass values.");
        }
    }
    std::set<std::pair<std::uint32_t, std::uint64_t>> windowIds;
    for (const auto &window : windows) {
        if (!windowIds.emplace(window.window.xid(), window.window.incarnation()).second ||
            (window.wmClassClass && !normalized(*window.wmClassClass)) ||
            (window.wmClassInstance && !normalized(*window.wmClassInstance))) {
            return failure<std::shared_ptr<const ApplicationAssociationSnapshot>>(
                ErrorCode::validation_error, "The copied window identity evidence is invalid.",
                "Use unique window incarnations and bounded decoded WM_CLASS fields.");
        }
    }

    std::vector<ApplicationAssociation> associations;
    std::vector<ApplicationAssociationDiagnostic> diagnostics;
    associations.reserve(windows.size());
    bool diagnosticsTruncated = false;
    const auto addDiagnostic = [&](ApplicationAssociationDiagnosticCode code,
                                   const ObservedWindowIdentity &window) {
        if (diagnostics.size() < maximumAssociationDiagnostics) {
            diagnostics.push_back({code, window});
        } else {
            diagnosticsTruncated = true;
        }
    };

    for (const auto &window : windows) {
        std::optional<ApplicationAssociation> association;
        if (window.activationDesktopFileId &&
            desktopIds.contains(window.activationDesktopFileId->value())) {
            association = ApplicationAssociation{
                window.window, ApplicationId::desktopEntry(*window.activationDesktopFileId),
                ApplicationAssociationEvidence::activationDesktopFileId};
        }

        const auto classToken =
            window.wmClassClass ? normalized(*window.wmClassClass) : std::optional<std::string>{};
        const auto instanceToken = window.wmClassInstance ? normalized(*window.wmClassInstance)
                                                          : std::optional<std::string>{};
        bool ambiguous = false;
        if (!association && classToken) {
            const auto matches = matchesStartup(catalog, *classToken);
            if (matches.size() == 1U) {
                association = {window.window,
                               ApplicationId::desktopEntry(catalog[matches.front()].desktopFileId),
                               ApplicationAssociationEvidence::startupWmClassClass};
            } else if (matches.size() > 1U) {
                ambiguous = true;
                addDiagnostic(ApplicationAssociationDiagnosticCode::ambiguousStartupWmClassClass,
                              window.window);
            }
        }
        if (!association && !ambiguous && instanceToken) {
            const auto matches = matchesStartup(catalog, *instanceToken);
            if (matches.size() == 1U) {
                association = {window.window,
                               ApplicationId::desktopEntry(catalog[matches.front()].desktopFileId),
                               ApplicationAssociationEvidence::startupWmClassInstance};
            } else if (matches.size() > 1U) {
                ambiguous = true;
                addDiagnostic(ApplicationAssociationDiagnosticCode::ambiguousStartupWmClassInstance,
                              window.window);
            }
        }
        if (!association && !ambiguous) {
            std::vector<std::size_t> matches;
            if (classToken) {
                matches = matchesDesktopFileId(catalog, *classToken);
            }
            if (matches.empty() && instanceToken) {
                matches = matchesDesktopFileId(catalog, *instanceToken);
            }
            if (matches.size() == 1U) {
                association = {window.window,
                               ApplicationId::desktopEntry(catalog[matches.front()].desktopFileId),
                               ApplicationAssociationEvidence::desktopFileIdMatch};
            } else if (matches.size() > 1U) {
                ambiguous = true;
                addDiagnostic(ApplicationAssociationDiagnosticCode::ambiguousDesktopFileId,
                              window.window);
            }
        }
        if (!association) {
            const auto fallbackToken = classToken ? classToken : instanceToken;
            if (ambiguous) {
                association = {window.window, ApplicationId::unknown(),
                               ApplicationAssociationEvidence::unknownApplication};
            } else if (fallbackToken) {
                auto fallback = ApplicationId::wmClassFallback(*fallbackToken);
                if (!fallback) {
                    return Result<std::shared_ptr<const ApplicationAssociationSnapshot>>::failure(
                        fallback.error());
                }
                association = {window.window, std::move(fallback).value(),
                               ApplicationAssociationEvidence::wmClassFallback};
            } else {
                association = {window.window, ApplicationId::unknown(),
                               ApplicationAssociationEvidence::unknownApplication};
            }
        }
        associations.push_back(std::move(*association));
    }

    std::shared_ptr<const ApplicationAssociationSnapshot> snapshot =
        std::make_shared<const ApplicationAssociationSnapshot>(ApplicationAssociationSnapshot{
            generation, std::move(associations), std::move(diagnostics), diagnosticsTruncated});
    return Result<std::shared_ptr<const ApplicationAssociationSnapshot>>::success(
        std::move(snapshot));
}

} // namespace prismdrake::launcher
