#pragma once

#include "IconRequest.hpp"
#include "Result.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <functional>
#include <memory>

namespace prismdrake::icons {

inline constexpr std::size_t maximumIconThemeSearchRoots = 128U;
inline constexpr std::size_t maximumIconThemeSearchPathBytes = 64U * 1024U;

struct IconThemeMetadata final {
    QString themeName;
    QString fallbackThemeName;
    QStringList searchPaths;
};

/// Injectable Qt theme-metadata boundary. Implementations run only on an icon worker thread.
class IconThemeMetadataProvider {
  public:
    virtual ~IconThemeMetadataProvider() = default;
    /// May be called concurrently by bounded icon workers; implementations must be reentrant.
    [[nodiscard]] virtual foundation::Result<IconThemeMetadata> snapshot() = 0;
};

/// Qt-bound asynchronous icon adapter. Reentrant Qt theme metadata, bounded theme-file discovery,
/// and all image decoding are captured or executed only on worker threads.
class IconResolver final {
  public:
    using Completion = std::function<void(IconResolution)>;

    explicit IconResolver(std::size_t maximumOutstandingRequests = 32U,
                          std::unique_ptr<IconThemeMetadataProvider> metadataProvider = {});
    ~IconResolver();
    IconResolver(const IconResolver &) = delete;
    IconResolver &operator=(const IconResolver &) = delete;
    IconResolver(IconResolver &&) noexcept;
    IconResolver &operator=(IconResolver &&) noexcept;

    /// Queues one immutable request and invokes completion on context's thread if it remains live.
    [[nodiscard]] foundation::Result<void> resolve(IconRequest request, QObject *context,
                                                   Completion completion) const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace prismdrake::icons
