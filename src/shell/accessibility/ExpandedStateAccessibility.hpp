#pragma once

#include <optional>

class QObject;

namespace prismdrake::shell::accessibility {

struct ExpandedState final {
    bool expandable;
    bool expanded;
    bool collapsed;
};

/// Installs the Qt Quick accessibility factory extension used by expandable
/// Prismdrake controls. Call this after constructing QGuiApplication and before
/// constructing shell controls; calling it more than once is safe.
void installExpandedStateAccessibility();

/// Returns the state exported through the real QAccessible interface when the
/// object participates in Prismdrake's expanded-state contract.
[[nodiscard]] std::optional<ExpandedState> queryExpandedState(QObject *object);

} // namespace prismdrake::shell::accessibility
