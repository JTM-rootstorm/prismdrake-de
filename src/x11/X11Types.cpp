#include "X11Types.hpp"

namespace prismdrake::x11 {

foundation::Result<WindowId> WindowId::fromProtocol(Value value) {
    if (value == 0U) {
        return foundation::Result<WindowId>::failure(
            {foundation::ErrorCode::invalid_argument, "The X11 window identifier is invalid.",
             "Use a nonzero identifier obtained from the active X11 connection."});
    }
    return foundation::Result<WindowId>::success(WindowId{value});
}

} // namespace prismdrake::x11
