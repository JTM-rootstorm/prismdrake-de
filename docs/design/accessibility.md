# Accessibility contract

Accessibility is functional correctness for Prismdrake Lustre and Prismdrake
Forge. Profile switching must preserve user overrides and publish them in the
same atomic generation as visual tokens.

## Required behavior

- Every primary workflow is keyboard operable with deterministic, visible
  focus order and no hover-only action.
- Interactive elements expose stable accessible names, roles, states,
  descriptions, and test identifiers where appropriate.
- Color is not the only indicator of focus, urgency, selection, error, or
  disabled state.
- Text scaling permits expansion and does not rely on fixed English widths.
- Minimum target sizes come from tokens; the reference accessibility layer uses
  48 px.
- Reduced motion removes nonessential movement, uses instantaneous or simple
  state replacement, and never delays input.
- Disabled transparency selects opaque materials. High contrast strengthens
  border, text, and focus separation.
- Screen-reader content omits purely decorative facets and describes meaningful
  status independently from presentation.

## PD0 review matrix

| Surface | Keyboard | Screen reader | Contrast/fallback | Motion | Target size |
|---|---|---|---|---|---|
| Panel/tasks | Linear focus and shortcuts; no hover dependency | Name, running/active/urgent state, action | Opaque panel and non-color state border/label | State swap under reduced motion | Token minimum |
| Launcher | Search focus, arrows, activation, escape | Search role, result name/type/state | Opaque surface and visible focus | No expanding flourish required | Token minimum |
| Notifications | Focus actions without stealing focus unexpectedly | App, summary, urgency, action; body handled as untrusted | Opaque card and explicit urgency | No entrance animation required | Token minimum |
| Quick settings | Tab/arrows and explicit toggles | Name, role, checked/value/unavailable state | Disabled state uses shape/text as well as color | Immediate value update | Token minimum |
| Decorations | Keyboard WM fallback remains available | Window actions and state where platform supports it | Active/inactive/urgent borders remain distinct | No motion required | Token minimum |

PD0 mockups are visual hierarchy aids, not evidence of assistive-technology
support. PD1 toolkit approval requires keyboard and AT-SPI prototype evidence.

Relevant requirements: `PD-A11Y-001` through `PD-A11Y-010`.
