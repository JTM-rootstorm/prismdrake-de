# Product vision

Prismdrake Desktop Environment is a traditional Linux desktop environment,
initially targeting X11. It provides a familiar panel-and-launcher workflow
with expressive, original visuals, strong accessibility, clear failure modes,
and a standards-capable baseline.

## Users and use cases

Prismdrake serves people who prefer direct, discoverable desktop workflows and
use GTK, Qt, SDL, and traditional X11 applications together. Its primary use
cases are launching and switching applications, managing desktop context,
receiving notifications, adjusting common settings, and operating a resilient
session without requiring a full GNOME desktop stack.

Familiarity comes from hierarchy, feedback, and conventional desktop
ergonomics. Prismdrake does not copy proprietary assets or exact layouts. Its
Lustre and Forge profiles express one original prismatic and draconic visual
language at different levels of translucency, depth, compactness, and tactility.

## Product boundary

Prismdrake is a desktop environment, not a full application suite. A file
manager, terminal, browser, editor, office suite, package manager, and login
manager are outside core 1.0 unless separately scoped.

The project has three integration layers:

- Prismdrake owns the session, shell surfaces, appearance, notifications,
  settings presentation, toolkit integration, and fallback selection.
- X11 and freedesktop standards provide the portable baseline for application,
  session, and desktop interoperability.
- Glasswyrm optionally provides negotiated window-manager and compositor
  capabilities. It remains authoritative for window state, focus, stacking,
  workspaces, composition, blur, capture, and output policy.

Linux and X11 are the initial platform context. Wayland is deferred; current
contracts should avoid needless barriers to future research without claiming
support.

## Current maturity

PD0 produces documentation, schemas, examples, mockups, and validation. No
usable shell, settings service, notification service, or Glasswyrm protocol is
implemented in this milestone.

Relevant requirements: `PD-PROD-001` through `PD-PROD-010`, `PD-SCOPE-001`
through `PD-SCOPE-010`, and `PD-PLAT-001` through `PD-PLAT-005`.

