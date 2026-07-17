#include "BuildInfo.hpp"

#include "prismdrake/foundation/BuildInfoConfig.hpp"

namespace prismdrake::foundation {

std::string_view productVersion() noexcept { return PRISMDRAKE_VERSION_STRING; }

bool developerOverridesEnabled() noexcept { return PRISMDRAKE_DEVELOPER_OVERRIDES_ENABLED != 0; }

} // namespace prismdrake::foundation
