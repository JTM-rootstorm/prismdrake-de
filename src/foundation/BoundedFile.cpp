#include "BoundedFile.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::foundation {
namespace {

class FileDescriptor final {
  public:
    explicit FileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    [[nodiscard]] int release() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

  private:
    int descriptor_;
};

[[nodiscard]] Error openError(int errorNumber) {
    if (errorNumber == ENOENT || errorNumber == ENOTDIR) {
        return {ErrorCode::not_found, "The requested file does not exist",
                "Restore the file or use packaged defaults"};
    }
    if (errorNumber == EACCES || errorNumber == EPERM) {
        return {ErrorCode::permission_denied, "Permission denied while opening the file",
                "Review the file and parent-directory permissions"};
    }
    return {ErrorCode::io_error, "Unable to open the file for bounded reading",
            "Check the filesystem and retry"};
}

[[nodiscard]] Error readError(int errorNumber) {
    if (errorNumber == EACCES || errorNumber == EPERM) {
        return {ErrorCode::permission_denied, "Permission denied while reading the file",
                "Review the file permissions"};
    }
    return {ErrorCode::io_error, "Unable to read the complete file",
            "Check the filesystem and retry"};
}

[[nodiscard]] Result<std::string> closeAndReturn(FileDescriptor &descriptor, std::string contents) {
    const int rawDescriptor = descriptor.release();
    if (::close(rawDescriptor) != 0) {
        return Result<std::string>::failure({ErrorCode::io_error,
                                             "Unable to close the file after reading",
                                             "Check the filesystem and retry"});
    }
    return Result<std::string>::success(std::move(contents));
}

} // namespace

Result<std::string> readBoundedFile(const std::filesystem::path &path, std::size_t maxBytes) {
    if (path.empty()) {
        return Result<std::string>::failure(
            {ErrorCode::invalid_argument, "A file path is required", "Provide a non-empty path"});
    }
    if (maxBytes == 0 || maxBytes > std::string{}.max_size()) {
        return Result<std::string>::failure({ErrorCode::invalid_argument,
                                             "The file byte limit is invalid",
                                             "Provide a positive representable byte limit"});
    }

    const int rawDescriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (rawDescriptor < 0) {
        return Result<std::string>::failure(openError(errno));
    }
    FileDescriptor descriptor(rawDescriptor);

    struct stat metadata{};
    if (::fstat(descriptor.get(), &metadata) != 0) {
        return Result<std::string>::failure(readError(errno));
    }
    if (!S_ISREG(metadata.st_mode)) {
        return Result<std::string>::failure({ErrorCode::unsupported,
                                             "Bounded reads require a regular file",
                                             "Use a regular file as the input source"});
    }
    if (metadata.st_size < 0) {
        return Result<std::string>::failure({ErrorCode::io_error,
                                             "The file reported an invalid size",
                                             "Check the filesystem and retry"});
    }
    if (static_cast<std::uintmax_t>(metadata.st_size) > maxBytes) {
        return Result<std::string>::failure({ErrorCode::too_large,
                                             "The file exceeds the configured byte limit",
                                             "Reduce the file size or raise the reviewed limit"});
    }

    std::string contents;
    try {
        contents.reserve(static_cast<std::size_t>(metadata.st_size));
    } catch (const std::bad_alloc &) {
        return Result<std::string>::failure({ErrorCode::io_error,
                                             "Unable to allocate the bounded file buffer",
                                             "Free memory or use a smaller byte limit"});
    } catch (const std::length_error &) {
        return Result<std::string>::failure({ErrorCode::too_large,
                                             "The file cannot fit in the bounded file buffer",
                                             "Use a smaller input file"});
    }

    constexpr std::size_t bufferSize = static_cast<std::size_t>(16) * 1024;
    std::array<char, bufferSize> buffer{};
    while (contents.size() < maxBytes) {
        const std::size_t remaining = maxBytes - contents.size();
        const std::size_t requestSize = std::min(buffer.size(), remaining);
        const ssize_t bytesRead = ::read(descriptor.get(), buffer.data(), requestSize);
        if (bytesRead == 0) {
            return closeAndReturn(descriptor, std::move(contents));
        }
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<std::string>::failure(readError(errno));
        }
        try {
            contents.append(buffer.data(), static_cast<std::size_t>(bytesRead));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::failure({ErrorCode::io_error,
                                                 "Unable to grow the bounded file buffer",
                                                 "Free memory or use a smaller byte limit"});
        }
    }

    char sentinel = 0;
    for (;;) {
        const ssize_t bytesRead = ::read(descriptor.get(), &sentinel, 1);
        if (bytesRead > 0) {
            return Result<std::string>::failure(
                {ErrorCode::too_large, "The file exceeds the configured byte limit",
                 "Reduce the file size or raise the reviewed limit"});
        }
        if (bytesRead == 0) {
            return closeAndReturn(descriptor, std::move(contents));
        }
        if (errno != EINTR) {
            return Result<std::string>::failure(readError(errno));
        }
    }
}

} // namespace prismdrake::foundation
