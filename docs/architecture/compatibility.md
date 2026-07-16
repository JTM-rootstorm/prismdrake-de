# Compatibility matrix

Compatibility means correct protocols, platform settings, discovery, icons,
cursors, fonts, scaling, notifications, portals, and optional styling. It does
not mean forcing every application to look identical. GTK and Qt adapters are
separately installable where practical; core startup requires neither both
stacks nor an optional Glasswyrm enhancement.

| Capability | Standard or protocol | Prismdrake owner | Basic X11 | Optional Glasswyrm enhancement | GTK impact | Qt impact | Fallback | Milestone |
|---|---|---|---|---|---|---|---|---|
| XDG base directories | XDG Base Directory | settings/session components | Required | None | Shared config/state locations | Shared config/state locations | XDG-specified defaults | PD1 |
| Desktop entry discovery | Desktop Entry Specification | shell launcher model | Required | None | Discovers GTK apps by standard metadata | Discovers Qt apps by standard metadata | Invalid entries omitted with diagnostics | PD1 |
| Icon themes | Icon Theme Specification | settings plus optional adapters | Required | None | Export theme name; optional GTK assets | Export theme name; optional Qt adapter | `hicolor` and generic placeholder | PD2/PD3 |
| Cursor themes | XCursor and XSettings | settings daemon | Required | None | GTK consumes exported theme/size | Qt consumes exported theme/size | Platform default cursor | PD2/PD3 |
| Fonts and DPI | XSettings, X resources where appropriate | settings daemon | Required | Rich output metadata may improve selection | Export supported font/DPI settings | Export supported font/DPI settings | Toolkit and X11 defaults | PD1/PD3 |
| XSettings | XSettings specification | settings daemon | Required | None | Primary GTK setting path | Some Qt platform integrations consume values | Documented environment/toolkit defaults | PD1 |
| GTK theme settings | GTK settings and optional data theme | optional GTK adapter | No | None | Coherent optional theme; app-owned CSD/libadwaita may differ | None | GTK/application default | PD3 |
| Qt platform and style | Qt platform settings and optional adapters | optional Qt adapter | No | None | None | Fonts/icons/cursors/palette; optional Widgets/Quick styles | Qt/application default | PD3 |
| Window metadata | ICCCM and EWMH | shell mirror model; WM authoritative | Required | Richer typed metadata | Application publishes standard hints | Application publishes standard hints | Exclude invalid/stale entries | PD1 |
| Dock work area | EWMH `_NET_WM_STRUT_PARTIAL` | shell requests; WM applies | Required | `GW_SHELL_ROLE_V1` may add role semantics | Apps observe work area | Apps observe work area | Standard dock strut | PD1/PD4 |
| Active window/task list | EWMH client list and active window | shell mirror model; WM authoritative | Required | Rich reliable lifecycle metadata | Normal task representation | Normal task representation | Icons/titles from standard fields | PD1/PD4 |
| Workspaces | EWMH desktop properties | shell mirror model; WM authoritative | Required where advertised | `GW_WORKSPACE_V1` rich metadata | Apps use standard workspace hints | Apps use standard workspace hints | Basic indexed list or hidden UI | PD2/PD4 |
| Notifications | Freedesktop Notifications | notification daemon and shell presentation | Required | None | Standard D-Bus client behavior | Standard D-Bus client behavior | Clear service-unavailable result; session continues | PD2 |
| Modern status items | StatusNotifierItem | shell status module | Optional baseline | None | Toolkit-specific host/client support | Toolkit-specific host/client support | Application remains usable without tray item | PD2 |
| Legacy tray | XEmbed system tray | isolated optional bridge | No | None | Legacy GTK apps only | Legacy Qt apps only | No legacy tray; never blocks session | Deferred compatibility decision |
| Portals/sandbox settings | XDG Desktop Portal | optional portal backend | Optional for unsandboxed baseline | None | Sandboxed GTK settings and requests | Sandboxed Qt settings and requests | Portal reports unavailable operation | PD3/PD5 |
| Autostart | Desktop Application Autostart | session component | Required | None | Toolkit-neutral | Toolkit-neutral | Invalid entries skipped safely | PD1 |
| Clipboard | X11 selections and ICCCM | X server/clients; shell presents only scoped UI | Required platform behavior | Future diagnostics only | Native toolkit handling | Native toolkit handling | No Prismdrake clipboard replacement in PD0 | PD2 |
| Session environment | POSIX process environment, XDG, D-Bus | session component | Required | Capability variables only if accepted | Receives standard environment | Receives standard environment | Safe minimal environment | PD1 |
| Power and authorization | External services and PolicyKit | typed adapters; external authority | Future daily-use feature | Secure lock primitives only after review | Toolkit-neutral service calls | Toolkit-neutral service calls | Disable control and fail closed for authorization | PD5 |
| Backdrop blur | No universal X11 baseline; proposed `GW_BACKDROP_V1` | shell requests; compositor executes | No | Blur region and parameters | Shell material only | Shell material only | Token-declared alpha/opaque material | PD4 |
| Window thumbnails | No reliable universal baseline; proposed `GW_WINDOW_THUMBNAIL_V1` | WM/compositor produces; shell presents | No | Authorized, scoped thumbnail | No forced toolkit impact | No forced toolkit impact | Application icon, title, and state | PD4 after privacy design |
| Server-side decorations | ICCCM/EWMH plus proposed `GW_DECORATION_V1` | WM authoritative; optional decorator renders | Usable windows required | Typed state/coordination | App CSD may remain | App CSD may remain | WM or client move/resize/close/focus path | PD4 |

## Toolkit limits

GTK compatibility includes supported fonts, DPI, icons, cursors, XSettings,
portals, and an optional Prismdrake GTK theme. Applications may deliberately use
their own client-side decorations or libadwaita presentation. libadwaita is not
a mandatory core dependency.

Qt compatibility includes supported fonts, DPI, icons, cursors, palette or
color-scheme settings, and optional style adapters. Not every Qt Quick
application permits or should receive external restyling.

Coherence is the goal. Prismdrake does not destructively overwrite user toolkit
configuration or promise pixel identity across application-owned interfaces.

Relevant requirements: `PD-INT-001` through `PD-INT-008`.

