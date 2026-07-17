#pragma once

#include "Result.hpp"
#include "XdgPaths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace prismdrake::session {

/// One private runtime directory owned by an individual Prismdrake session process.
///
/// Creation first validates the current-user XDG runtime boundary. All mutations are relative to
/// retained directory descriptors and never traverse symbolic links. Cleanup removes only marker
/// files created through this object and the exact directory inode created by prepare().
class SessionRuntime final {
  public:
    [[nodiscard]] static foundation::Result<SessionRuntime>
    prepare(const foundation::XdgPaths &paths);

    ~SessionRuntime();

    SessionRuntime(const SessionRuntime &) = delete;
    SessionRuntime &operator=(const SessionRuntime &) = delete;
    SessionRuntime(SessionRuntime &&other) noexcept;
    SessionRuntime &operator=(SessionRuntime &&) = delete;

    [[nodiscard]] const std::filesystem::path &instanceDirectory() const noexcept {
        return instance_directory_;
    }
    [[nodiscard]] std::filesystem::path readyMarkerPath() const;
    [[nodiscard]] std::filesystem::path safeModeMarkerPath() const;

    /// Atomically creates this instance's empty ready marker with private permissions.
    [[nodiscard]] foundation::Result<void> markReady();

    /// Atomically creates this instance's empty development-safe-mode marker.
    [[nodiscard]] foundation::Result<void> markSafeMode();

    /// Removes only markers created by this object and the exact owned instance directory.
    ///
    /// Unexpected entries or replaced paths cause cleanup to fail without recursively deleting
    /// anything. A successful call is idempotent.
    [[nodiscard]] foundation::Result<void> cleanup();

  private:
    SessionRuntime(int parentDescriptor, int instanceDescriptor,
                   std::filesystem::path instanceDirectory, std::string instanceName,
                   std::uintmax_t expectedUserId, std::uintmax_t device,
                   std::uintmax_t inode) noexcept;

    [[nodiscard]] foundation::Result<void> verifyOwnedInstance() const;
    [[nodiscard]] foundation::Result<void> createMarker(std::string_view name, int &descriptor);
    [[nodiscard]] foundation::Result<void> removeMarker(std::string_view name, int &descriptor);
    void closeDescriptors() noexcept;

    int parent_descriptor_ = -1;
    int instance_descriptor_ = -1;
    int ready_descriptor_ = -1;
    int safe_mode_descriptor_ = -1;
    std::filesystem::path instance_directory_;
    std::string instance_name_;
    std::uintmax_t expected_user_id_ = 0U;
    std::uintmax_t instance_device_ = 0U;
    std::uintmax_t instance_inode_ = 0U;
    bool active_ = false;
};

} // namespace prismdrake::session
