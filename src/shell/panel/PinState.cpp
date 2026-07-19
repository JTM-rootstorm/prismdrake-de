#include "PinState.hpp"

#include "AtomicFile.hpp"
#include "BoundedFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <set>
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
               utf8CodePointCount(value) <= 4096U && scalar();
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
    const auto boundary = validatePrivateRegularFile(stateFile);
    if (boundary) {
        auto contents = foundation::readBoundedFile(stateFile, maximumPinStateBytes);
        if (contents) {
            PinStateBoundsSax bounds;
            if (Json::sax_parse(contents.value(), &bounds, Json::input_format_t::json, true,
                                false)) {
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
        } else if (contents.error().code == ErrorCode::too_large) {
            status = PinStateLoadStatus::invalidRetained;
        } else {
            return Result<PinStateStore>::failure(contents.error());
        }
    } else if (boundary.error().code == ErrorCode::not_found) {
        status = PinStateLoadStatus::missing;
    } else if (boundary.error().code == ErrorCode::validation_error) {
        status = PinStateLoadStatus::invalidRetained;
    } else {
        return Result<PinStateStore>::failure(boundary.error());
    }

    std::shared_ptr<const PinStateSnapshot> snapshot =
        std::make_shared<const PinStateSnapshot>(PinStateSnapshot{1U, std::move(pins), status});
    return Result<PinStateStore>::success(PinStateStore{std::move(stateFile), std::move(snapshot)});
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
