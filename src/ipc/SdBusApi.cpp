#include "SdBusApi.hpp"

namespace prismdrake::ipc::sdbus {

std::string_view providerName() noexcept {
#if defined(PRISMDRAKE_SD_BUS_PROVIDER_BASU)
    return "basu";
#else
    return "libsystemd";
#endif
}

} // namespace prismdrake::ipc::sdbus
