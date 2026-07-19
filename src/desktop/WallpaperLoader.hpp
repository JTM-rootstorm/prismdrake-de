#pragma once

#include "Result.hpp"
#include "WallpaperModel.hpp"

#include <QObject>

#include <functional>
#include <memory>

namespace prismdrake::desktop {

struct WallpaperDecodeResult final {
    WallpaperLoadStatus status;
    std::shared_ptr<const QImage> image;
};

/// Worker-side decoder boundary. Implementations must not include source paths in diagnostics.
class WallpaperDecoder {
  public:
    using Cancelled = std::function<bool()>;

    virtual ~WallpaperDecoder() = default;
    [[nodiscard]] virtual WallpaperDecodeResult decode(const WallpaperRequest &request,
                                                       const Cancelled &cancelled) const = 0;
};

/// Creates the production local-file QImageReader decoder.
[[nodiscard]] std::shared_ptr<const WallpaperDecoder> makeQtWallpaperDecoder();

/// Coalescing asynchronous loader with at most one active and one pending generation.
class WallpaperLoader final {
  public:
    using Completion =
        std::function<void(std::shared_ptr<const WallpaperSnapshot> wallpaperSnapshot)>;

    explicit WallpaperLoader(std::shared_ptr<const WallpaperDecoder> decoder = {});
    ~WallpaperLoader();
    WallpaperLoader(const WallpaperLoader &) = delete;
    WallpaperLoader &operator=(const WallpaperLoader &) = delete;
    WallpaperLoader(WallpaperLoader &&) = delete;
    WallpaperLoader &operator=(WallpaperLoader &&) = delete;

    /// Replaces any pending generation. Completion runs on context's thread only if still current.
    [[nodiscard]] foundation::Result<void> load(WallpaperRequest request, QObject *context,
                                                Completion completion);

    /// Returns the last published immutable snapshot, if any.
    [[nodiscard]] std::shared_ptr<const WallpaperSnapshot> current() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace prismdrake::desktop
