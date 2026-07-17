#pragma once

#if defined(PRISMDRAKE_SD_BUS_PROVIDER_BASU) && defined(PRISMDRAKE_SD_BUS_PROVIDER_LIBSYSTEMD)
#error "Exactly one Prismdrake sd-bus provider must be selected"
#elif defined(PRISMDRAKE_SD_BUS_PROVIDER_BASU)
#include <basu/sd-bus-vtable.h>
#include <basu/sd-bus.h>
#elif defined(PRISMDRAKE_SD_BUS_PROVIDER_LIBSYSTEMD)
#include <systemd/sd-bus-vtable.h>
#include <systemd/sd-bus.h>
#else
#error "A Prismdrake sd-bus provider must be selected by the build system"
#endif

#include <string_view>
#include <utility>

namespace prismdrake::ipc::sdbus {

template <typename Object, Object *(*Unref)(Object *)> class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(Object *object) noexcept : object_(object) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;

    UniqueHandle(UniqueHandle &&other) noexcept : object_(other.release()) {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] Object *get() const noexcept { return object_; }
    [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }

    /// Releases the current object and returns storage suitable for an sd-bus out parameter.
    [[nodiscard]] Object **put() noexcept {
        reset();
        return &object_;
    }

    [[nodiscard]] Object *release() noexcept { return std::exchange(object_, nullptr); }

    void reset(Object *object = nullptr) noexcept {
        if (object_ != nullptr) {
            Unref(object_);
        }
        object_ = object;
    }

  private:
    Object *object_ = nullptr;
};

using Bus = UniqueHandle<sd_bus, sd_bus_flush_close_unref>;
using Slot = UniqueHandle<sd_bus_slot, sd_bus_slot_unref>;
using Message = UniqueHandle<sd_bus_message, sd_bus_message_unref>;
using Credentials = UniqueHandle<sd_bus_creds, sd_bus_creds_unref>;

/// Returns the selected build-time provider name for diagnostics.
[[nodiscard]] std::string_view providerName() noexcept;

} // namespace prismdrake::ipc::sdbus
