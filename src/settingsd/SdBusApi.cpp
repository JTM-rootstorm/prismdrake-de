#include "SdBusApi.hpp"

namespace prismdrake::settingsd::sdbus {

std::string_view providerName() noexcept {
#if defined(PRISMDRAKE_SD_BUS_PROVIDER_BASU)
    return "basu";
#else
    return "libsystemd";
#endif
}

} // namespace prismdrake::settingsd::sdbus
