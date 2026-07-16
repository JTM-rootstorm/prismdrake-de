# Architecture overview

The Prismdrake architecture separates presentation, desktop policy, external
system authority, and optional native effects. Component boundaries remain
Proposed pending approval of [ADR 0002](../adr/0002-component-and-process-model.md).

## System context

```mermaid
flowchart TB
    Apps[GTK, Qt, SDL, and X11 applications]
    Standards[X11, ICCCM, EWMH, XDG, D-Bus, XSettings, portals]
    Prism[Prismdrake Desktop Environment]
    Glass[Glasswyrm]
    System[Audio, network, power, devices, authorization, login services]

    Apps <--> Standards
    Prism <--> Standards
    Glass <--> Standards
    Prism -. capability-negotiated generic GW_* requests .-> Glass
    Prism <--> System
```

## Proposed component model

```mermaid
flowchart LR
    Session[prismdrake-session]
    Settings[prismdrake-settingsd]
    Notify[prismdrake-notifyd]
    Shell[prismdrake-shell]
    Decor[prismdrake-decor]
    Control[prismdrake-control]
    Portal[prismdrake-portal]
    Polkit[prismdrake-polkit-agent]
    Lock[prismdrake-lock - deferred]
    Themes[prismdrake-themes]
    Qt[prismdrake-style-qt]
    Gtk[prismdrake-theme-gtk]
    WM[Window manager and compositor]

    Session --> Settings
    Session --> Notify
    Session --> Shell
    Session --> Portal
    Settings -- immutable generation --> Shell
    Settings -- immutable generation --> Notify
    Settings -- immutable generation --> Control
    Themes --> Settings
    Notify -- policy and presentation model --> Shell
    Control -- narrow validated requests --> Settings
    Shell -- standard WM requests --> WM
    Decor -- authoritative state and requests --> WM
    Portal --> Settings
    Polkit -. isolated prompts .-> Shell
    Lock -. only after accepted security design .-> WM
    Qt -. optional adapter .-> Settings
    Gtk -. optional adapter .-> Settings
```

Arrows express startup or data flow, not authority. The window manager and
compositor remain authoritative for window state, focus, stacking, workspaces,
composition, blur, capture, and output policy.

## Ownership summary

| Domain | Authority | Prismdrake behavior |
|---|---|---|
| Window state and policy | Glasswyrm or active WM | Mirror state and send standards/native requests |
| Composition and effects | Glasswyrm or active compositor | Request intent and geometry; select fallbacks |
| Desktop settings | `prismdrake-settingsd` | Validate and publish immutable generations |
| Shell presentation | `prismdrake-shell` | Render and interact without becoming state authority |
| Notification policy/history | `prismdrake-notifyd` | Route a presentation model to shell surfaces |
| Audio/network/power/devices | External system services | Present typed adapters and fail softly |
| Authorization | External authority plus isolated agent | Display minimal prompts; fail closed |
| Theme data | `prismdrake-themes` package, resolved by settings service | Consume validated token snapshots |

## Baseline and enhancement paths

The baseline uses XDG, D-Bus, XSettings, ICCCM, EWMH, freedesktop
notifications, StatusNotifierItem, portals, and ordinary alpha or opaque
materials. Optional Glasswyrm enhancements use separately accepted, versioned
`GW_*` capabilities. Missing enhancements never prevent core session startup;
blur falls back to a readable non-blur material and thumbnails fall back to
icons and titles.

Relevant requirements: `PD-SCOPE-001`, `PD-PLAT-001` through `PD-PLAT-004`,
`PD-COMP-001` through `PD-COMP-010`, and `PD-GW-001` through `PD-GW-010`.
