#include "AtomicFile.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prismdrake::foundation {
namespace {

class FileDescriptor final {
  public:
    explicit FileDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&other) noexcept : descriptor_(other.release()) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    [[nodiscard]] int release() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

  private:
    int descriptor_;
};

class TemporaryFile final {
  public:
    TemporaryFile(int directory, std::string name) noexcept
        : directory_(directory), name_(std::move(name)) {}
    ~TemporaryFile() {
        if (active_) {
            ::unlinkat(directory_, name_.c_str(), 0);
        }
    }

    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;

    [[nodiscard]] const std::string &name() const noexcept { return name_; }
    void release() noexcept { active_ = false; }

  private:
    int directory_;
    std::string name_;
    bool active_ = true;
};

struct CreatedTemporaryFile {
    int descriptor;
    std::string name;
};

[[nodiscard]] Error filesystemError(int errorNumber, std::string operation) {
    if (errorNumber == EACCES || errorNumber == EPERM || errorNumber == EROFS) {
        return {ErrorCode::permission_denied, std::move(operation),
                "Review the destination and parent-directory permissions"};
    }
    if (errorNumber == ENOENT || errorNumber == ENOTDIR) {
        return {ErrorCode::not_found, std::move(operation),
                "Create the destination directory before writing"};
    }
    return {ErrorCode::io_error, std::move(operation), "Check the filesystem and retry"};
}

[[nodiscard]] Result<int> openDirectoryWithoutSymlinks(const std::filesystem::path &path) {
    const bool absolute = path.is_absolute();
    FileDescriptor current(
        ::open(absolute ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (current.get() < 0) {
        return Result<int>::failure(
            filesystemError(errno, "Unable to begin validating the destination directory"));
    }

    const std::filesystem::path components = absolute ? path.relative_path() : path;
    for (const auto &component : components) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return Result<int>::failure({ErrorCode::invalid_argument,
                                         "Parent traversal is not permitted in atomic paths",
                                         "Use a normalized path without '..' components"});
        }

        struct stat metadata{};
        if (::fstatat(current.get(), component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            return Result<int>::failure(
                filesystemError(errno, "Unable to inspect a destination-directory component"));
        }
        if (S_ISLNK(metadata.st_mode)) {
            return Result<int>::failure(
                {ErrorCode::invalid_argument,
                 "Symbolic links are not permitted in the destination directory chain",
                 "Use a reviewed directory path without symbolic links"});
        }
        if (!S_ISDIR(metadata.st_mode)) {
            return Result<int>::failure(
                {ErrorCode::unsupported, "The destination parent chain contains a non-directory",
                 "Use an existing regular directory as the destination parent"});
        }

        const int next = ::openat(current.get(), component.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            return Result<int>::failure(
                filesystemError(errno, "Unable to open a destination-directory component"));
        }
        current = FileDescriptor(next);
    }

    return Result<int>::success(current.release());
}

[[nodiscard]] bool sameFile(const struct stat &left, const struct stat &right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

[[nodiscard]] Result<void> verifyDestinationUnchanged(int directory, const std::string &name,
                                                      bool existed, const struct stat &original) {
    struct stat current{};
    if (::fstatat(directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(current.st_mode)) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "The destination became a symbolic link",
                                          "Retry with a stable regular-file destination"});
        }
        if (!existed || !sameFile(original, current)) {
            return Result<void>::failure({ErrorCode::io_error,
                                          "The destination changed during the atomic write",
                                          "Retry after concurrent writers have stopped"});
        }
        return Result<void>::success();
    }

    if (errno == ENOENT) {
        if (!existed) {
            return Result<void>::success();
        }
        return Result<void>::failure({ErrorCode::io_error,
                                      "The destination disappeared during the atomic write",
                                      "Retry after concurrent writers have stopped"});
    }
    return Result<void>::failure(
        filesystemError(errno, "Unable to recheck the destination before replacement"));
}

[[nodiscard]] bool fillRandom(std::array<unsigned char, 16> &bytes) noexcept {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const ssize_t result = ::getrandom(bytes.data() + completed, bytes.size() - completed, 0);
        if (result > 0) {
            completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] Result<CreatedTemporaryFile> createTemporaryFile(int directory,
                                                               const std::string &destinationName) {
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    constexpr std::size_t attempts = 32;
    constexpr std::size_t randomNameLength = 32;
    const std::string prefix = "." + destinationName + ".prismdrake.tmp.";

    errno = 0;
    const long nameLimit = ::fpathconf(directory, _PC_NAME_MAX);
    if (nameLimit >= 0 && prefix.size() + randomNameLength > static_cast<std::size_t>(nameLimit)) {
        return Result<CreatedTemporaryFile>::failure(
            {ErrorCode::io_error, "The same-directory temporary filename is too long",
             "Use a shorter destination filename"});
    }
    if (nameLimit < 0 && errno != 0) {
        return Result<CreatedTemporaryFile>::failure(
            filesystemError(errno, "Unable to determine the directory filename limit"));
    }

    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        std::array<unsigned char, 16> randomBytes{};
        if (!fillRandom(randomBytes)) {
            return Result<CreatedTemporaryFile>::failure(
                filesystemError(errno, "Unable to generate a unique temporary filename"));
        }

        std::string name = prefix;
        name.reserve(prefix.size() + randomNameLength);
        for (const unsigned char byte : randomBytes) {
            name.push_back(hexadecimal[byte >> 4U]);
            name.push_back(hexadecimal[byte & 0x0fU]);
        }

        const int descriptor = ::openat(directory, name.c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0) {
            return Result<CreatedTemporaryFile>::success({descriptor, std::move(name)});
        }
        if (errno != EEXIST) {
            return Result<CreatedTemporaryFile>::failure(
                filesystemError(errno, "Unable to create the same-directory temporary file"));
        }
    }

    return Result<CreatedTemporaryFile>::failure({ErrorCode::io_error,
                                                  "Unable to select a unique temporary filename",
                                                  "Remove stale temporary files and retry"});
}

[[nodiscard]] bool syncDescriptor(int descriptor) noexcept {
    for (;;) {
        if (::fsync(descriptor) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

} // namespace

Result<void> writeFileAtomically(const std::filesystem::path &destination, std::string_view payload,
                                 AtomicWriteOptions options) {
    if (destination.empty() || destination.filename().empty() || destination.filename() == "." ||
        destination.filename() == "..") {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "A regular destination file path is required",
                                      "Provide a non-empty file path"});
    }

    constexpr auto supportedPermissions = std::filesystem::perms::all;
    if (options.createPermissions == std::filesystem::perms::unknown ||
        (options.createPermissions & ~supportedPermissions) != std::filesystem::perms::none) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The requested file permissions are invalid",
             "Use only owner, group, and other read, write, or execute permissions"});
    }

    const std::filesystem::path parent =
        destination.parent_path().empty() ? std::filesystem::path{"."} : destination.parent_path();
    auto openedDirectory = openDirectoryWithoutSymlinks(parent);
    if (!openedDirectory) {
        return Result<void>::failure(std::move(openedDirectory.error()));
    }
    FileDescriptor directory(std::move(openedDirectory).value());
    const std::string destinationName = destination.filename().string();

    bool destinationExisted = false;
    struct stat originalMetadata{};
    if (::fstatat(directory.get(), destinationName.c_str(), &originalMetadata,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        destinationExisted = true;
        if (S_ISLNK(originalMetadata.st_mode)) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "Symbolic-link destinations are not permitted",
                                          "Write to a reviewed regular-file path"});
        }
        if (!S_ISREG(originalMetadata.st_mode)) {
            return Result<void>::failure({ErrorCode::unsupported,
                                          "Atomic replacement requires a regular destination",
                                          "Use a regular file as the destination"});
        }
        if (originalMetadata.st_uid != ::geteuid()) {
            return Result<void>::failure({ErrorCode::permission_denied,
                                          "The destination is owned by another user",
                                          "Use a destination owned by the effective user"});
        }
    } else if (errno != ENOENT) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to inspect the atomic-write destination"));
    }

    auto createdTemporary = createTemporaryFile(directory.get(), destinationName);
    if (!createdTemporary) {
        return Result<void>::failure(std::move(createdTemporary.error()));
    }
    CreatedTemporaryFile created = std::move(createdTemporary).value();
    FileDescriptor temporary(created.descriptor);
    TemporaryFile temporaryFile(directory.get(), std::move(created.name));

    const mode_t permissions = destinationExisted
                                   ? static_cast<mode_t>(originalMetadata.st_mode & 0777)
                                   : static_cast<mode_t>(options.createPermissions) & 0777;
    if (::fchmod(temporary.get(), permissions) != 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to establish temporary-file permissions"));
    }

    std::size_t written = 0;
    constexpr std::size_t maximumWrite =
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
    while (written < payload.size()) {
        const std::size_t request = std::min(payload.size() - written, maximumWrite);
        const ssize_t result = ::write(temporary.get(), payload.data() + written, request);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return Result<void>::failure(
            filesystemError(result == 0 ? EIO : errno, "Unable to write the complete payload"));
    }

    if (!syncDescriptor(temporary.get())) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to synchronize the temporary file"));
    }

    const int closedDescriptor = temporary.release();
    if (::close(closedDescriptor) != 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to close the synchronized temporary file"));
    }

    auto unchanged = verifyDestinationUnchanged(directory.get(), destinationName,
                                                destinationExisted, originalMetadata);
    if (!unchanged) {
        return unchanged;
    }

    if (::renameat(directory.get(), temporaryFile.name().c_str(), directory.get(),
                   destinationName.c_str()) != 0) {
        return Result<void>::failure(
            filesystemError(errno, "Unable to atomically replace the destination"));
    }
    temporaryFile.release();

    if (!syncDescriptor(directory.get())) {
        return Result<void>::failure(
            filesystemError(errno, "The replacement succeeded but directory sync failed"));
    }

    return Result<void>::success();
}

} // namespace prismdrake::foundation
