#include "SessionRuntime.hpp"

#include <cerrno>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prismdrake::session {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view readyMarkerName = "ready";
constexpr std::string_view safeModeMarkerName = "safe-mode";
constexpr std::size_t maximumCreationAttempts = 128U;

[[nodiscard]] Error filesystemError(int errorNumber, std::string operation) {
    if (errorNumber == EACCES || errorNumber == EPERM || errorNumber == EROFS) {
        return {ErrorCode::permission_denied, std::move(operation),
                "Check the private runtime-directory ownership and permissions."};
    }
    if (errorNumber == ENOENT || errorNumber == ENOTDIR) {
        return {ErrorCode::not_found, std::move(operation),
                "Recreate the private Prismdrake runtime boundary and retry."};
    }
    if (errorNumber == ENOSPC || errorNumber == EDQUOT || errorNumber == EMFILE ||
        errorNumber == ENFILE) {
        return {ErrorCode::too_large, std::move(operation),
                "Free runtime filesystem or descriptor capacity and retry."};
    }
    return {ErrorCode::io_error, std::move(operation), "Check the runtime filesystem and retry."};
}

[[nodiscard]] bool sameFile(const struct stat &metadata, std::uintmax_t device,
                            std::uintmax_t inode) noexcept {
    return static_cast<std::uintmax_t>(metadata.st_dev) == device &&
           static_cast<std::uintmax_t>(metadata.st_ino) == inode;
}

void closeDescriptor(int &descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

[[nodiscard]] Result<int> openDirectoryWithoutSymlinks(const std::filesystem::path &path) {
    if (!path.is_absolute() || path != path.lexically_normal()) {
        return Result<int>::failure(
            {ErrorCode::invalid_environment,
             "The session runtime path is not an absolute normalized directory.",
             "Resolve the XDG runtime path before preparing session state."});
    }

    int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0) {
        return Result<int>::failure(
            filesystemError(errno, "Unable to begin opening the session runtime boundary."));
    }

    for (const auto &component : path.relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            closeDescriptor(current);
            return Result<int>::failure(
                {ErrorCode::invalid_environment,
                 "Parent traversal is not permitted in the session runtime path.",
                 "Use a normalized XDG runtime path without parent components."});
        }

        struct stat metadata = {};
        if (::fstatat(current, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            const auto error =
                filesystemError(errno, "Unable to inspect a session runtime path component.");
            closeDescriptor(current);
            return Result<int>::failure(error);
        }
        if (S_ISLNK(metadata.st_mode)) {
            closeDescriptor(current);
            return Result<int>::failure(
                {ErrorCode::invalid_environment,
                 "Symbolic links are not permitted in the session runtime path.",
                 "Use a private runtime path without symbolic links."});
        }
        if (!S_ISDIR(metadata.st_mode)) {
            closeDescriptor(current);
            return Result<int>::failure(
                {ErrorCode::invalid_environment,
                 "The session runtime path contains a non-directory component.",
                 "Use an existing private directory for session runtime state."});
        }

        const int next =
            ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const auto error =
                filesystemError(errno, "Unable to open a session runtime path component.");
            closeDescriptor(current);
            return Result<int>::failure(error);
        }
        closeDescriptor(current);
        current = next;
    }
    return Result<int>::success(current);
}

} // namespace

Result<SessionRuntime> SessionRuntime::prepare(const foundation::XdgPaths &paths) {
    constexpr std::string_view prismdrakeRootName = "prismdrake";
    if (!paths.runtimeDirectory.is_absolute() ||
        paths.runtimeDirectory != paths.runtimeDirectory.lexically_normal()) {
        return Result<SessionRuntime>::failure(
            {ErrorCode::invalid_environment,
             "The Prismdrake runtime path is not absolute and normalized.",
             "Resolve XDG_RUNTIME_DIR before preparing session state."});
    }
    const auto runtimeBase = paths.runtimeDirectory.parent_path();
    if (paths.runtimeDirectory != runtimeBase / prismdrakeRootName) {
        return Result<SessionRuntime>::failure(
            {ErrorCode::invalid_environment,
             "The Prismdrake runtime path is outside its expected XDG boundary.",
             "Use the prismdrake directory directly beneath XDG_RUNTIME_DIR."});
    }

    auto openedBase = openDirectoryWithoutSymlinks(runtimeBase);
    if (!openedBase) {
        return Result<SessionRuntime>::failure(std::move(openedBase).error());
    }
    int baseDescriptor = std::move(openedBase).value();

    struct stat baseMetadata = {};
    const auto expectedUserId = foundation::currentProcessUserId();
    if (::fstat(baseDescriptor, &baseMetadata) != 0 || !S_ISDIR(baseMetadata.st_mode) ||
        static_cast<std::uintmax_t>(baseMetadata.st_uid) != expectedUserId ||
        (baseMetadata.st_mode & 07777U) != 0700U) {
        closeDescriptor(baseDescriptor);
        return Result<SessionRuntime>::failure(
            {ErrorCode::permission_denied,
             "The XDG runtime base is not private and current-user owned.",
             "Use a current-user XDG_RUNTIME_DIR with mode 0700 and retry."});
    }

    bool createdRoot = false;
    if (::mkdirat(baseDescriptor, prismdrakeRootName.data(), 0700) == 0) {
        createdRoot = true;
    } else if (errno != EEXIST) {
        const auto error = filesystemError(errno, "Unable to create the Prismdrake runtime root.");
        closeDescriptor(baseDescriptor);
        return Result<SessionRuntime>::failure(error);
    }
    struct stat rootPathMetadata = {};
    if (::fstatat(baseDescriptor, prismdrakeRootName.data(), &rootPathMetadata,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        const auto error = filesystemError(errno, "Unable to inspect the Prismdrake runtime root.");
        closeDescriptor(baseDescriptor);
        return Result<SessionRuntime>::failure(error);
    }
    if (S_ISLNK(rootPathMetadata.st_mode) || !S_ISDIR(rootPathMetadata.st_mode)) {
        closeDescriptor(baseDescriptor);
        return Result<SessionRuntime>::failure(
            {ErrorCode::invalid_environment,
             "The Prismdrake runtime root is not a regular directory.",
             "Replace it with a private current-user directory without symbolic links."});
    }

    const int parentDescriptor = ::openat(baseDescriptor, prismdrakeRootName.data(),
                                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parentDescriptor < 0) {
        const auto error = filesystemError(errno, "Unable to open the Prismdrake runtime root.");
        closeDescriptor(baseDescriptor);
        return Result<SessionRuntime>::failure(error);
    }
    closeDescriptor(baseDescriptor);

    if (createdRoot && ::fchmod(parentDescriptor, 0700) != 0) {
        const auto error = filesystemError(errno, "Unable to secure the Prismdrake runtime root.");
        int mutableParent = parentDescriptor;
        closeDescriptor(mutableParent);
        return Result<SessionRuntime>::failure(error);
    }

    struct stat parentMetadata = {};
    if (::fstat(parentDescriptor, &parentMetadata) != 0 || !S_ISDIR(parentMetadata.st_mode) ||
        parentMetadata.st_dev != rootPathMetadata.st_dev ||
        parentMetadata.st_ino != rootPathMetadata.st_ino ||
        static_cast<std::uintmax_t>(parentMetadata.st_uid) != expectedUserId ||
        (parentMetadata.st_mode & 07777U) != 0700U) {
        int mutableParent = parentDescriptor;
        closeDescriptor(mutableParent);
        return Result<SessionRuntime>::failure(
            {ErrorCode::permission_denied,
             "The opened Prismdrake runtime boundary is not private and current-user owned.",
             "Use a current-user directory with mode 0700 and retry."});
    }

    for (std::size_t attempt = 0U; attempt < maximumCreationAttempts; ++attempt) {
        const auto instanceName =
            "session-" + std::to_string(::getpid()) + "-" + std::to_string(attempt);
        if (::mkdirat(parentDescriptor, instanceName.c_str(), 0700) != 0) {
            if (errno == EEXIST) {
                continue;
            }
            int mutableParent = parentDescriptor;
            const auto error =
                filesystemError(errno, "Unable to create a session runtime instance.");
            closeDescriptor(mutableParent);
            return Result<SessionRuntime>::failure(error);
        }

        struct stat instancePathMetadata = {};
        if (::fstatat(parentDescriptor, instanceName.c_str(), &instancePathMetadata,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(instancePathMetadata.st_mode)) {
            (void)::unlinkat(parentDescriptor, instanceName.c_str(), AT_REMOVEDIR);
            int mutableParent = parentDescriptor;
            closeDescriptor(mutableParent);
            return Result<SessionRuntime>::failure(
                {ErrorCode::invalid_environment,
                 "The new session runtime instance changed before it could be opened.",
                 "Inspect the private runtime root and retry."});
        }

        const int instanceDescriptor = ::openat(parentDescriptor, instanceName.c_str(),
                                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (instanceDescriptor < 0) {
            const auto error =
                filesystemError(errno, "Unable to open a new session runtime instance.");
            (void)::unlinkat(parentDescriptor, instanceName.c_str(), AT_REMOVEDIR);
            int mutableParent = parentDescriptor;
            closeDescriptor(mutableParent);
            return Result<SessionRuntime>::failure(error);
        }

        struct stat instanceMetadata = {};
        if (::fchmod(instanceDescriptor, 0700) != 0 ||
            ::fstat(instanceDescriptor, &instanceMetadata) != 0 ||
            !S_ISDIR(instanceMetadata.st_mode) ||
            instanceMetadata.st_dev != instancePathMetadata.st_dev ||
            instanceMetadata.st_ino != instancePathMetadata.st_ino ||
            static_cast<std::uintmax_t>(instanceMetadata.st_uid) != expectedUserId ||
            (instanceMetadata.st_mode & 07777U) != 0700U) {
            int mutableInstance = instanceDescriptor;
            closeDescriptor(mutableInstance);
            (void)::unlinkat(parentDescriptor, instanceName.c_str(), AT_REMOVEDIR);
            int mutableParent = parentDescriptor;
            closeDescriptor(mutableParent);
            return Result<SessionRuntime>::failure(
                {ErrorCode::permission_denied,
                 "The new session runtime instance could not be made private.",
                 "Check the runtime filesystem ownership and permission support."});
        }

        return Result<SessionRuntime>::success(SessionRuntime{
            parentDescriptor,
            instanceDescriptor,
            paths.runtimeDirectory / instanceName,
            instanceName,
            expectedUserId,
            static_cast<std::uintmax_t>(instanceMetadata.st_dev),
            static_cast<std::uintmax_t>(instanceMetadata.st_ino),
        });
    }

    int mutableParent = parentDescriptor;
    closeDescriptor(mutableParent);
    return Result<SessionRuntime>::failure(
        {ErrorCode::too_large, "Unable to allocate a unique session runtime instance.",
         "Remove stale reviewed session instances or retry with a new process."});
}

SessionRuntime::SessionRuntime(int parentDescriptor, int instanceDescriptor,
                               std::filesystem::path instanceDirectory, std::string instanceName,
                               std::uintmax_t expectedUserId, std::uintmax_t device,
                               std::uintmax_t inode) noexcept
    : parent_descriptor_(parentDescriptor), instance_descriptor_(instanceDescriptor),
      instance_directory_(std::move(instanceDirectory)), instance_name_(std::move(instanceName)),
      expected_user_id_(expectedUserId), instance_device_(device), instance_inode_(inode),
      active_(true) {}

SessionRuntime::~SessionRuntime() {
    (void)cleanup();
    closeDescriptors();
}

SessionRuntime::SessionRuntime(SessionRuntime &&other) noexcept
    : parent_descriptor_(std::exchange(other.parent_descriptor_, -1)),
      instance_descriptor_(std::exchange(other.instance_descriptor_, -1)),
      ready_descriptor_(std::exchange(other.ready_descriptor_, -1)),
      safe_mode_descriptor_(std::exchange(other.safe_mode_descriptor_, -1)),
      instance_directory_(std::move(other.instance_directory_)),
      instance_name_(std::move(other.instance_name_)), expected_user_id_(other.expected_user_id_),
      instance_device_(other.instance_device_), instance_inode_(other.instance_inode_),
      active_(std::exchange(other.active_, false)) {}

std::filesystem::path SessionRuntime::readyMarkerPath() const {
    return instance_directory_ / readyMarkerName;
}

std::filesystem::path SessionRuntime::safeModeMarkerPath() const {
    return instance_directory_ / safeModeMarkerName;
}

Result<void> SessionRuntime::verifyOwnedInstance() const {
    if (!active_ || parent_descriptor_ < 0 || instance_descriptor_ < 0) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The session runtime instance is no longer active.",
             "Prepare a new session runtime instance before creating markers."});
    }

    struct stat descriptorMetadata = {};
    struct stat pathMetadata = {};
    if (::fstat(instance_descriptor_, &descriptorMetadata) != 0 ||
        ::fstatat(parent_descriptor_, instance_name_.c_str(), &pathMetadata, AT_SYMLINK_NOFOLLOW) !=
            0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to verify the session runtime instance."));
    }
    if (!S_ISDIR(descriptorMetadata.st_mode) || !S_ISDIR(pathMetadata.st_mode) ||
        !sameFile(descriptorMetadata, instance_device_, instance_inode_) ||
        !sameFile(pathMetadata, instance_device_, instance_inode_) ||
        static_cast<std::uintmax_t>(descriptorMetadata.st_uid) != expected_user_id_ ||
        static_cast<std::uintmax_t>(pathMetadata.st_uid) != expected_user_id_ ||
        (descriptorMetadata.st_mode & 07777U) != 0700U ||
        (pathMetadata.st_mode & 07777U) != 0700U) {
        return Result<void>::failure(
            {ErrorCode::permission_denied,
             "The session runtime instance no longer matches its private owned directory.",
             "Stop using the changed runtime instance and inspect it manually."});
    }
    return Result<void>::success();
}

Result<void> SessionRuntime::createMarker(std::string_view name, int &descriptor) {
    auto verified = verifyOwnedInstance();
    if (!verified) {
        return verified;
    }
    if (descriptor >= 0) {
        struct stat descriptorMetadata = {};
        struct stat pathMetadata = {};
        const std::string markerName{name};
        if (::fstat(descriptor, &descriptorMetadata) != 0 ||
            ::fstatat(instance_descriptor_, markerName.c_str(), &pathMetadata,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(descriptorMetadata.st_mode) || !S_ISREG(pathMetadata.st_mode) ||
            descriptorMetadata.st_dev != pathMetadata.st_dev ||
            descriptorMetadata.st_ino != pathMetadata.st_ino) {
            return Result<void>::failure(
                {ErrorCode::permission_denied,
                 "A session runtime marker no longer matches its owned file.",
                 "Inspect the changed session runtime instance manually."});
        }
        return Result<void>::success();
    }

    const std::string markerName{name};
    descriptor = ::openat(instance_descriptor_, markerName.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to create a session runtime marker."));
    }
    if (::fchmod(descriptor, 0600) != 0) {
        const int errorNumber = errno;
        (void)::unlinkat(instance_descriptor_, markerName.c_str(), 0);
        closeDescriptor(descriptor);
        return Result<void>::failure(
            filesystemError(errorNumber, "Unable to secure a session runtime marker."));
    }

    struct stat metadata = {};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        static_cast<std::uintmax_t>(metadata.st_uid) != expected_user_id_ ||
        (metadata.st_mode & 07777U) != 0600U) {
        (void)::unlinkat(instance_descriptor_, markerName.c_str(), 0);
        closeDescriptor(descriptor);
        return Result<void>::failure(
            {ErrorCode::permission_denied,
             "A session runtime marker could not be made private and current-user owned.",
             "Check the runtime filesystem ownership and permission support."});
    }
    return Result<void>::success();
}

Result<void> SessionRuntime::removeMarker(std::string_view name, int &descriptor) {
    if (descriptor < 0) {
        return Result<void>::success();
    }

    struct stat descriptorMetadata = {};
    struct stat pathMetadata = {};
    const std::string markerName{name};
    if (::fstat(descriptor, &descriptorMetadata) != 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to inspect an owned session runtime marker."));
    }
    if (::fstatat(instance_descriptor_, markerName.c_str(), &pathMetadata, AT_SYMLINK_NOFOLLOW) !=
        0) {
        if (errno == ENOENT) {
            closeDescriptor(descriptor);
            return Result<void>::success();
        }
        return Result<void>::failure(
            filesystemError(errno, "Unable to verify an owned session runtime marker."));
    }
    if (!S_ISREG(descriptorMetadata.st_mode) || !S_ISREG(pathMetadata.st_mode) ||
        descriptorMetadata.st_dev != pathMetadata.st_dev ||
        descriptorMetadata.st_ino != pathMetadata.st_ino ||
        static_cast<std::uintmax_t>(pathMetadata.st_uid) != expected_user_id_) {
        return Result<void>::failure(
            {ErrorCode::permission_denied,
             "A session runtime marker was replaced and will not be removed.",
             "Inspect the changed session runtime instance manually."});
    }
    if (::unlinkat(instance_descriptor_, markerName.c_str(), 0) != 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to remove an owned session runtime marker."));
    }
    closeDescriptor(descriptor);
    return Result<void>::success();
}

Result<void> SessionRuntime::markReady() {
    return createMarker(readyMarkerName, ready_descriptor_);
}

Result<void> SessionRuntime::markSafeMode() {
    return createMarker(safeModeMarkerName, safe_mode_descriptor_);
}

Result<void> SessionRuntime::cleanup() {
    if (!active_) {
        return Result<void>::success();
    }
    auto verified = verifyOwnedInstance();
    if (!verified) {
        return verified;
    }
    auto ready = removeMarker(readyMarkerName, ready_descriptor_);
    if (!ready) {
        return ready;
    }
    auto safeMode = removeMarker(safeModeMarkerName, safe_mode_descriptor_);
    if (!safeMode) {
        return safeMode;
    }
    if (::unlinkat(parent_descriptor_, instance_name_.c_str(), AT_REMOVEDIR) != 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to remove the owned session runtime instance."));
    }

    active_ = false;
    closeDescriptors();
    return Result<void>::success();
}

void SessionRuntime::closeDescriptors() noexcept {
    closeDescriptor(ready_descriptor_);
    closeDescriptor(safe_mode_descriptor_);
    closeDescriptor(instance_descriptor_);
    closeDescriptor(parent_descriptor_);
}

} // namespace prismdrake::session
