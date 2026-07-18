#pragma once

#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumAssociationWindows = 16384U;
inline constexpr std::size_t maximumAssociationCatalogEntries = 16384U;
inline constexpr std::size_t maximumAssociationDiagnostics = 256U;

enum class ApplicationIdKind : std::uint8_t { desktopEntry, wmClassFallback, unknown };

/// A display-free, non-authorizing identity used only for grouping and presentation.
class ApplicationId final {
  public:
    [[nodiscard]] static ApplicationId desktopEntry(const DesktopFileId &desktopFileId);
    [[nodiscard]] static foundation::Result<ApplicationId>
    wmClassFallback(std::string_view normalizedWmClass);
    [[nodiscard]] static ApplicationId unknown();

    [[nodiscard]] ApplicationIdKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string &value() const noexcept { return value_; }

    friend bool operator==(const ApplicationId &, const ApplicationId &) = default;

  private:
    ApplicationId(ApplicationIdKind kind, std::string value)
        : kind_(kind), value_(std::move(value)) {}

    ApplicationIdKind kind_;
    std::string value_;
};

/// Copied X11 lifetime evidence. This type observes neither the server nor window lifecycle.
class ObservedWindowIdentity final {
  public:
    [[nodiscard]] static foundation::Result<ObservedWindowIdentity>
    fromObservation(std::uint32_t xid, std::uint64_t incarnation);

    [[nodiscard]] std::uint32_t xid() const noexcept { return xid_; }
    [[nodiscard]] std::uint64_t incarnation() const noexcept { return incarnation_; }

    friend bool operator==(const ObservedWindowIdentity &,
                           const ObservedWindowIdentity &) = default;

  private:
    ObservedWindowIdentity(std::uint32_t xid, std::uint64_t incarnation) noexcept
        : xid_(xid), incarnation_(incarnation) {}

    std::uint32_t xid_;
    std::uint64_t incarnation_;
};

struct CatalogApplicationIdentity final {
    DesktopFileId desktopFileId;
    std::optional<std::string> startupWmClass;
};

struct WindowAssociationObservation final {
    ObservedWindowIdentity window;
    std::optional<DesktopFileId> activationDesktopFileId;
    std::optional<std::string> wmClassInstance;
    std::optional<std::string> wmClassClass;
};

enum class ApplicationAssociationEvidence : std::uint8_t {
    activationDesktopFileId,
    startupWmClassClass,
    startupWmClassInstance,
    desktopFileIdMatch,
    wmClassFallback,
    unknownApplication,
};

enum class ApplicationAssociationDiagnosticCode : std::uint8_t {
    ambiguousStartupWmClassClass,
    ambiguousStartupWmClassInstance,
    ambiguousDesktopFileId,
};

struct ApplicationAssociationDiagnostic final {
    ApplicationAssociationDiagnosticCode code;
    ObservedWindowIdentity window;
};

struct ApplicationAssociation final {
    ObservedWindowIdentity window;
    ApplicationId applicationId;
    ApplicationAssociationEvidence evidence;
};

struct ApplicationAssociationSnapshot final {
    std::uint64_t generation;
    std::vector<ApplicationAssociation> associations;
    std::vector<ApplicationAssociationDiagnostic> diagnostics;
    bool diagnosticsTruncated;
};

/// Builds one immutable, complete association generation in the Accepted six-step order.
[[nodiscard]] foundation::Result<std::shared_ptr<const ApplicationAssociationSnapshot>>
associateApplications(const std::vector<CatalogApplicationIdentity> &catalog,
                      const std::vector<WindowAssociationObservation> &windows,
                      std::uint64_t generation);

} // namespace prismdrake::launcher
