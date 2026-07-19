#include "PinState.hpp"

#include "AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prismdrake::shell::panel {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using Json = nlohmann::ordered_json;

constexpr std::size_t maximumPinJsonNesting = 4U;
constexpr std::size_t maximumPinJsonNodes = 1024U;
constexpr std::size_t maximumPinJsonContainerItems = maximumPinCount + 2U;

class FileDescriptor final {
  public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}
    ~FileDescriptor() { ::close(value_); }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    [[nodiscard]] int get() const noexcept { return value_; }

  private:
    int value_;
};

[[nodiscard]] std::size_t utf8CodePointCount(std::string_view value) noexcept {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xc0U) != 0x80U; }));
}

class PinStateBoundsSax final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t, const string_t &) override { return scalar(); }
    bool string(string_t &value) override {
        return value.size() <= launcher::maximumDesktopFileIdBytes &&
               utf8CodePointCount(value) <= launcher::maximumDesktopFileIdCodePoints && scalar();
    }
    bool binary(binary_t &) override { return false; }

    bool start_object(std::size_t elements) override { return startContainer(true, elements); }
    bool key(string_t &value) override {
        if (stack_.empty() || !stack_.back().object || value.size() > 32U) {
            return false;
        }
        auto &frame = stack_.back();
        ++frame.items;
        return frame.items <= maximumPinJsonContainerItems && frame.keys.insert(value).second;
    }
    bool end_object() override { return endContainer(true); }

    bool start_array(std::size_t elements) override { return startContainer(false, elements); }
    bool end_array() override { return endContainer(false); }

    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }

  private:
    struct Frame final {
        bool object;
        std::size_t items;
        std::set<std::string, std::less<>> keys;
    };

    bool noteValue() {
        ++nodes_;
        if (nodes_ > maximumPinJsonNodes || stack_.size() > maximumPinJsonNesting) {
            return false;
        }
        if (!stack_.empty() && !stack_.back().object) {
            ++stack_.back().items;
            if (stack_.back().items > maximumPinJsonContainerItems) {
                return false;
            }
        }
        return true;
    }

    bool scalar() { return noteValue(); }

    bool startContainer(bool object, std::size_t elements) {
        if (!noteValue() || stack_.size() >= maximumPinJsonNesting ||
            (elements != std::numeric_limits<std::size_t>::max() &&
             elements > maximumPinJsonContainerItems)) {
            return false;
        }
        stack_.push_back({object, 0U, {}});
        return true;
    }

    bool endContainer(bool object) {
        if (stack_.empty() || stack_.back().object != object) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    std::size_t nodes_{0U};
    std::vector<Frame> stack_;
};

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure({code, std::move(message), std::move(recovery)});
}

[[nodiscard]] Result<std::vector<launcher::DesktopFileId>> validatePins(const Json &document) {
    if (!document.is_object() || document.size() != 2U || !document.contains("schema_version") ||
        !document.contains("pins") || !document["schema_version"].is_number_unsigned() ||
        document["schema_version"].get<std::uint64_t>() != 1U || !document["pins"].is_array() ||
        document["pins"].size() > maximumPinCount) {
        return failure<std::vector<launcher::DesktopFileId>>(
            ErrorCode::validation_error, "The private pin-state document is invalid.",
            "Retain the file for diagnosis and start with an empty pin set.");
    }

    std::set<std::string> seen;
    std::vector<launcher::DesktopFileId> pins;
    pins.reserve(document["pins"].size());
    for (const auto &value : document["pins"]) {
        if (!value.is_string()) {
            return failure<std::vector<launcher::DesktopFileId>>(
                ErrorCode::validation_error, "The private pin-state document is invalid.",
                "Store only bounded desktop-file identifiers.");
        }
        const auto &text = value.get_ref<const std::string &>();
        auto identifier = launcher::deriveDesktopFileId(text);
        if (!identifier || identifier.value().value() != text || !seen.insert(text).second) {
            return failure<std::vector<launcher::DesktopFileId>>(
                ErrorCode::validation_error, "The private pin-state document is invalid.",
                "Store one ordered occurrence of each desktop-file identifier.");
        }
        pins.push_back(std::move(identifier).value());
    }
    return Result<std::vector<launcher::DesktopFileId>>::success(std::move(pins));
}

[[nodiscard]] Result<void> validatePrivateRegularFile(const std::filesystem::path &path) {
    struct stat metadata = {};
    if (::lstat(path.c_str(), &metadata) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return Result<void>::failure({ErrorCode::not_found,
                                          "The private pin-state file does not exist.",
                                          "Start with an empty pin set."});
        }
        return Result<void>::failure(
            {errno == EACCES ? ErrorCode::permission_denied : ErrorCode::io_error,
             "The private pin-state file could not be inspected.",
             "Review the state-directory boundary."});
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 0777U) != 0600U) {
        return Result<void>::failure(
            {ErrorCode::validation_error, "The private pin-state file boundary is invalid.",
             "Retain the file and restore an owner-only regular state file."});
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> ensurePrivateDirectory(const std::filesystem::path &path) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return Result<void>::failure(
            {errno == EACCES || errno == EPERM ? ErrorCode::permission_denied : ErrorCode::io_error,
             "A private pin-state directory could not be created.",
             "Review the XDG state-directory boundary."});
    }

    struct stat metadata = {};
    if (::lstat(path.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        S_ISLNK(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 0777U) != 0700U) {
        return Result<void>::failure(
            {ErrorCode::permission_denied, "A private pin-state directory is unsafe.",
             "Use a current-user-owned directory with mode 0700 and no symbolic links."});
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::string> readPrivateRegularFile(const std::filesystem::path &path) {
    const int rawDescriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (rawDescriptor < 0) {
        const int openError = errno;
        if (openError == ENOENT || openError == ENOTDIR) {
            return failure<std::string>(ErrorCode::not_found,
                                        "The private pin-state file does not exist.",
                                        "Start with an empty pin set.");
        }
        if (openError == ELOOP) {
            return failure<std::string>(
                ErrorCode::validation_error, "The private pin-state file boundary is invalid.",
                "Retain the file and restore an owner-only regular state file.");
        }
        return failure<std::string>(openError == EACCES || openError == EPERM
                                        ? ErrorCode::permission_denied
                                        : ErrorCode::io_error,
                                    "The private pin-state file could not be opened.",
                                    "Review the state-directory boundary.");
    }
    FileDescriptor descriptor{rawDescriptor};
    struct stat metadata = {};
    if (::fstat(descriptor.get(), &metadata) != 0) {
        return failure<std::string>(ErrorCode::io_error,
                                    "The private pin-state file could not be inspected.",
                                    "Review the state-directory boundary.");
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 0777U) != 0600U) {
        return failure<std::string>(
            ErrorCode::validation_error, "The private pin-state file boundary is invalid.",
            "Retain the file and restore an owner-only regular state file.");
    }
    if (metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > maximumPinStateBytes) {
        return failure<std::string>(ErrorCode::too_large,
                                    "The private pin-state file exceeds its byte limit.",
                                    "Retain the file for diagnosis and start with empty pins.");
    }

    std::string contents;
    try {
        contents.reserve(static_cast<std::size_t>(metadata.st_size));
    } catch (const std::bad_alloc &) {
        return failure<std::string>(ErrorCode::io_error,
                                    "The private pin-state file could not be buffered.",
                                    "Free memory and retry.");
    } catch (const std::length_error &) {
        return failure<std::string>(ErrorCode::too_large,
                                    "The private pin-state file exceeds its byte limit.",
                                    "Retain the file for diagnosis and start with empty pins.");
    }

    std::array<char, 16U * 1024U> buffer{};
    while (contents.size() <= maximumPinStateBytes) {
        const auto remaining = maximumPinStateBytes + 1U - contents.size();
        const auto requested = std::min(buffer.size(), remaining);
        const ssize_t count = ::read(descriptor.get(), buffer.data(), requested);
        if (count == 0) {
            return Result<std::string>::success(std::move(contents));
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failure<std::string>(ErrorCode::io_error,
                                        "The private pin-state file could not be read.",
                                        "Review the state-directory boundary and retry.");
        }
        try {
            contents.append(buffer.data(), static_cast<std::size_t>(count));
        } catch (const std::bad_alloc &) {
            return failure<std::string>(ErrorCode::io_error,
                                        "The private pin-state file could not be buffered.",
                                        "Free memory and retry.");
        } catch (const std::length_error &) {
            return failure<std::string>(ErrorCode::too_large,
                                        "The private pin-state file exceeds its byte limit.",
                                        "Retain the file for diagnosis and start with empty pins.");
        }
    }
    return failure<std::string>(ErrorCode::too_large,
                                "The private pin-state file exceeds its byte limit.",
                                "Retain the file for diagnosis and start with empty pins.");
}

[[nodiscard]] std::string serialize(const std::vector<launcher::DesktopFileId> &pins) {
    Json document = {{"schema_version", 1U}, {"pins", Json::array()}};
    for (const auto &pin : pins) {
        document["pins"].push_back(pin.value());
    }
    return document.dump(2) + '\n';
}

} // namespace

Result<PinStateStore> PinStateStore::open(std::filesystem::path stateFile) {
    if (stateFile.empty() || !stateFile.is_absolute() || stateFile.filename().empty()) {
        return failure<PinStateStore>(ErrorCode::invalid_argument,
                                      "The private pin-state path is invalid.",
                                      "Use the absolute Prismdrake XDG state path.");
    }

    std::vector<launcher::DesktopFileId> pins;
    PinStateLoadStatus status = PinStateLoadStatus::missing;
    auto contents = readPrivateRegularFile(stateFile);
    if (contents) {
        PinStateBoundsSax bounds;
        if (Json::sax_parse(contents.value(), &bounds, Json::input_format_t::json, true, false)) {
            Json document = Json::parse(contents.value(), nullptr, false, true);
            auto decoded = validatePins(document);
            if (decoded) {
                pins = std::move(decoded).value();
                status = PinStateLoadStatus::loaded;
            } else {
                status = PinStateLoadStatus::invalidRetained;
            }
        } else {
            status = PinStateLoadStatus::invalidRetained;
        }
    } else if (contents.error().code == ErrorCode::not_found) {
        status = PinStateLoadStatus::missing;
    } else if (contents.error().code == ErrorCode::validation_error ||
               contents.error().code == ErrorCode::too_large) {
        status = PinStateLoadStatus::invalidRetained;
    } else {
        return Result<PinStateStore>::failure(contents.error());
    }

    std::shared_ptr<const PinStateSnapshot> snapshot =
        std::make_shared<const PinStateSnapshot>(PinStateSnapshot{1U, std::move(pins), status});
    return Result<PinStateStore>::success(PinStateStore{std::move(stateFile), std::move(snapshot)});
}

Result<PinStateStore>
PinStateStore::openInStateDirectory(std::filesystem::path prismdrakeStateDirectory) {
    const bool containsTraversal =
        std::ranges::any_of(prismdrakeStateDirectory, [](const std::filesystem::path &component) {
            return component == "..";
        });
    if (prismdrakeStateDirectory.empty() || !prismdrakeStateDirectory.is_absolute() ||
        containsTraversal || prismdrakeStateDirectory.filename() != "prismdrake") {
        return failure<PinStateStore>(ErrorCode::invalid_argument,
                                      "The Prismdrake XDG state path is invalid.",
                                      "Use the absolute Prismdrake XDG state directory.");
    }

    auto prismdrakeDirectory = ensurePrivateDirectory(prismdrakeStateDirectory);
    if (!prismdrakeDirectory) {
        return Result<PinStateStore>::failure(prismdrakeDirectory.error());
    }
    const auto shellDirectory = prismdrakeStateDirectory / "shell";
    auto shell = ensurePrivateDirectory(shellDirectory);
    if (!shell) {
        return Result<PinStateStore>::failure(shell.error());
    }
    return open(shellDirectory / "pinned-applications-v1.json");
}

Result<std::shared_ptr<const PinStateSnapshot>>
PinStateStore::replace(std::vector<launcher::DesktopFileId> pins) {
    if (pins.size() > maximumPinCount) {
        return failure<std::shared_ptr<const PinStateSnapshot>>(
            ErrorCode::too_large, "The requested pin set is too large.",
            "Keep at most 256 ordered desktop-file identifiers.");
    }
    std::set<std::string> seen;
    for (const auto &pin : pins) {
        auto checked = launcher::deriveDesktopFileId(pin.value());
        if (!checked || checked.value().value() != pin.value() ||
            !seen.insert(pin.value()).second) {
            return failure<std::shared_ptr<const PinStateSnapshot>>(
                ErrorCode::validation_error, "The requested pin set is invalid.",
                "Use one ordered occurrence of each desktop-file identifier.");
        }
    }
    if (current_->generation == std::numeric_limits<std::uint64_t>::max()) {
        return failure<std::shared_ptr<const PinStateSnapshot>>(
            ErrorCode::too_large, "The pin-state generation is exhausted.",
            "Restart through a reviewed recovery path before another mutation.");
    }

    const auto boundary = validatePrivateRegularFile(state_file_);
    if (!boundary && boundary.error().code != ErrorCode::not_found) {
        return Result<std::shared_ptr<const PinStateSnapshot>>::failure(boundary.error());
    }

    auto write = foundation::writeFileAtomically(
        state_file_, serialize(pins),
        {std::filesystem::perms::owner_read | std::filesystem::perms::owner_write});
    if (!write) {
        return Result<std::shared_ptr<const PinStateSnapshot>>::failure(write.error());
    }
    std::shared_ptr<const PinStateSnapshot> snapshot = std::make_shared<const PinStateSnapshot>(
        PinStateSnapshot{current_->generation + 1U, std::move(pins), PinStateLoadStatus::loaded});
    current_ = snapshot;
    return Result<std::shared_ptr<const PinStateSnapshot>>::success(std::move(snapshot));
}

} // namespace prismdrake::shell::panel
