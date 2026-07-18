# Component model

This catalog refines specification section 10. Names and process boundaries are
Accepted by [ADR 0002](../adr/0002-component-and-process-model.md). “Restart”
always means bounded retry with backoff; repeated failure enters safe mode or
leaves only the affected optional feature unavailable.

## `prismdrake-session`

- **Purpose:** establish the session environment, order startup and shutdown,
  supervise components, coordinate logout, and enter safe mode.
- **Inputs:** XDG/session environment, packaged component metadata, component
  readiness and failure events, and exact-child private readiness channels.
- **Outputs:** child process lifecycle, readiness and diagnostics, orderly
  shutdown requests.
- **Owned state:** supervision state, retry budget, safe-mode state, session
  lifecycle; not application or window state.
- **Forbidden responsibilities:** shell rendering, WM policy, privileged system
  operations, or editing settings behind `prismdrake-settingsd`.
- **Dependencies/package:** toolkit-neutral process and D-Bus facilities in a
  core package; no mandatory systemd dependency.
- **Crash/restart/security:** an external session entry point must retain a basic
  exit path; restart is environment-specific. Each shell launch receives a new
  bounded private readiness channel. Child arguments and environment are
  untrusted and logs exclude secrets.

## `prismdrake-shell`

- **Purpose:** render panel, launcher, desktop, task UI, quick settings, status,
  notifications, and on-screen displays as logical modules.
- **Inputs:** immutable settings/theme generations, mirrored WM state,
  notification models, typed external-service adapters, user input.
- **Outputs:** UI, accessibility tree, narrow commands and effect requests, and
  one private readiness message after the initial panel epoch exists.
- **Owned state:** transient view state and caches only.
- **Forbidden responsibilities:** authoritative focus, stacking, workspace,
  composition, capture, settings, notification-history, or privilege policy.
- **Dependencies/package:** Accepted Qt 6 Quick visible surfaces and C++ models
  in `prismdrake-shell`; Qt remains isolated from toolkit-neutral services.
- **Crash/restart/security:** WM state survives; session restarts shell with its
  last validated generation. All external metadata and rich content are
  untrusted and work must not block UI or D-Bus dispatch threads.

## `prismdrake-settingsd`

- **Purpose:** validate configuration and theme data, switch profiles, export
  XSettings, and publish immutable generation-tagged snapshots.
- **Inputs:** packaged defaults, user configuration, validated requests, and
  capability changes.
- **Outputs:** snapshots, narrow D-Bus replies/signals, diagnostics, atomic
  persistent writes.
- **Owned state:** current valid settings generation and migration state.
- **Forbidden responsibilities:** control-center UI, arbitrary-key mutation,
  secrets, or compositor policy.
- **Dependencies/package:** toolkit-neutral parser, filesystem, D-Bus, and X11
  settings support in the core package.
- **Crash/restart/security:** consumers retain the last complete generation;
  restart revalidates persisted data. Reject malformed, oversized,
  unsupported-version, and unsafe-path input without corrupting the prior file.

## `prismdrake-notifyd`

- **Purpose:** own the freedesktop notification service, replacement semantics,
  routing, retention, history, and do-not-disturb policy.
- **Inputs:** untrusted notification calls and validated settings generations.
- **Outputs:** typed presentation models to the shell and narrow history
  operations.
- **Owned state:** notification identifiers, policy, and bounded history.
- **Forbidden responsibilities:** shell rendering, executing notification
  content, or storing sensitive bodies in logs.
- **Dependencies/package:** toolkit-neutral D-Bus and storage support in a core
  notification package.
- **Crash/restart/security:** shell and session continue; name reacquisition and
  recovery are explicit. Rich text, image data, paths, sizes, and actions are
  validated; retention and clearing controls are user-visible.

## `prismdrake-decor`

- **Purpose:** optionally render server-side decorations from authoritative WM
  state and relay user intent.
- **Inputs:** versioned WM state, theme generation, pointer/keyboard actions.
- **Outputs:** decoration surfaces and move/resize/close requests.
- **Owned state:** rendering cache and transient interaction state.
- **Forbidden responsibilities:** focus, stacking, geometry, lifecycle, or
  window-state policy.
- **Dependencies/package:** optional rendering toolkit and accepted WM contract
  in `prismdrake-decor`.
- **Crash/restart/security:** WM supplies a usable move/resize/close fallback;
  stale window identifiers are discarded. Window titles and metadata are
  untrusted and private by default.

## `prismdrake-control`

- **Purpose:** provide user-facing settings pages and validation feedback.
- **Inputs:** public snapshots, capabilities, schemas, and user edits.
- **Outputs:** narrow validated change requests, never direct hidden-state edits.
- **Owned state:** draft form state only.
- **Forbidden responsibilities:** settings authority, privileged system policy,
  or direct mutation of another component's storage.
- **Dependencies/package:** an implementation-reviewed UI toolkit in
  `prismdrake-control`; the control application remains separately installable
  from non-visual services.
- **Crash/restart/security:** losing a draft does not corrupt applied settings;
  risky requests use typed adapters and authorization where required.

## `prismdrake-portal`

- **Purpose:** implement desktop-specific portal integration where a standard
  portal contract requires a Prismdrake backend.
- **Inputs:** versioned portal requests and desktop settings.
- **Outputs:** narrowly scoped portal results or typed failures.
- **Owned state:** transient request and cancellation state.
- **Forbidden responsibilities:** replacing the portal framework, bypassing
  authorization, or exposing unrestricted settings.
- **Dependencies/package:** adapter-specific and optional in
  `prismdrake-portal`.
- **Crash/restart/security:** sandboxed applications receive a clear failure;
  restart must not replay completed authorization. Validate callers, scope,
  timeouts, cancellation, and returned paths.

## `prismdrake-polkit-agent`

- **Purpose:** present minimal isolated authorization prompts for an external
  PolicyKit authority.
- **Inputs:** authenticated authorization requests and user input.
- **Outputs:** the minimum response required by the authority.
- **Owned state:** one bounded prompt transaction; no policy authority.
- **Forbidden responsibilities:** running inside the general shell, caching
  credentials, or deciding authorization policy.
- **Dependencies/package:** optional isolated agent package.
- **Crash/restart/security:** fail closed and require a fresh transaction;
  redact credentials and request details from logs.

## `prismdrake-lock`

- **Purpose:** coordinate secure lock presentation only after a complete threat
  model and suitable X11/Glasswyrm primitives are Accepted.
- **Inputs/outputs/state:** Deferred; no interface or security claim exists in
  PD0.
- **Forbidden responsibilities:** treating an overlay as a secure lock or
  claiming input exclusion without authoritative platform support.
- **Dependencies/package:** deferred optional component.
- **Crash/restart/security:** fail closed only under a future accepted design;
  current absence must be documented honestly.

## `prismdrake-themes`

- **Purpose:** ship shared data-only visual tokens, icons, cursors, and approved
  profile assets.
- **Inputs:** versioned authored data with provenance.
- **Outputs:** read-only packaged defaults consumed by validation/settings.
- **Owned state:** no runtime mutable state.
- **Forbidden responsibilities:** executable theme scripts or proprietary and
  unlicensed assets.
- **Dependencies/package:** data-only `prismdrake-themes` package.
- **Crash/restart/security:** malformed data is rejected before publication;
  packaged recovery defaults remain available.

## `prismdrake-style-qt`

- **Purpose:** optionally export Prismdrake settings and provide supported Qt
  integration or style adapters.
- **Inputs:** immutable settings/theme generations and Qt platform contracts.
- **Outputs:** supported fonts, icons, cursors, palette, and style behavior.
- **Owned state:** adapter caches only.
- **Forbidden responsibilities:** forcing styles on applications that opt out or
  becoming mandatory for non-Qt services.
- **Dependencies/package:** optional Qt-specific `prismdrake-style-qt` package.
- **Crash/restart/security:** Qt applications retain platform fallbacks; validate
  all environment and configuration inputs.

## `prismdrake-theme-gtk`

- **Purpose:** optionally export GTK settings and data-only theme integration.
- **Inputs:** immutable settings/theme generations and supported GTK contracts.
- **Outputs:** fonts, icons, cursors, settings, and optional theme assets.
- **Owned state:** adapter caches and generated settings where specified.
- **Forbidden responsibilities:** requiring GNOME desktop components, overriding
  application-owned presentation, or becoming core startup authority.
- **Dependencies/package:** optional GTK-specific `prismdrake-theme-gtk`
  package; GTK itself is not categorically forbidden.
- **Crash/restart/security:** applications retain toolkit defaults; paths and
  serialized settings are validated and written atomically.
