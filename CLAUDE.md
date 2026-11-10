# Prismdrake Agent Instructions

The workspace `../CLAUDE.md` governs coordination, delegation, external/VM
effects, Git and commits, licensing selection, security-review recording, and
general validation. This file only narrows it for Prismdrake.

## 1. Authority, scope, and identity

Read when relevant: `docs/PRISMDRAKE_PROJECT_SPECIFICATION.md`; `docs/index.md`
and durable README setup/overview material; Accepted ADRs under `docs/adr/`; relevant
architecture, security, design, schema, and interface documents; current
milestone, roadmap, and issue criteria; then older active-phase plans,
Proposed ADRs, research, mockups, and older attachments.

Conflict precedence is: Mike's current instruction; the latest committed
project specification; newer Accepted ADRs that explicitly amend it; other
Accepted contracts/schemas/interfaces; current milestone/issue criteria; then
Proposed ADRs, research, mockups, and older attachments. Preserve the higher-
precedence decision, implement only non-conflicting work, and report
contradictions instead of silently choosing.

Milestone discipline remains binding: no later-phase feature creep, no
production claim from a prototype, no closure without the documented gate,
and no stale status when the project advances. Preserve Accepted/Proposed
status and explicit implementation/evidence limits.

Canonical names are **Prismdrake Desktop Environment** (**Prismdrake**),
**Prismdrake Lustre** (`lustre`), and **Prismdrake Forge** (`forge`). Packages
and executables use `prismdrake-*`, D-Bus uses `org.prismdrake.*`, Glasswyrm
interfaces use generic `GW_*`, and the repository is
`JTM-rootstorm/prismdrake-de`. Do not introduce alternate product, profile, or
interface names without an intentional Accepted decision. Do not use Microsoft
product/theme names for Prismdrake components, profiles, themes, or features.

Prismdrake owns desktop session/startup/recovery, shell surfaces,
profile/appearance/settings UX, notification policy/history, toolkit settings
propagation/adapters, desktop portal integration, effect/capability requests,
fallbacks, and user diagnostics. Glasswyrm owns X11 server behavior,
window-management policy and authoritative window state,
focus/stacking/workspaces, move/resize/minimize/maximize/fullscreen,
composition/output authority, backdrop-blur execution, compositor
shadows/effects, scene capture/window thumbnails, input/output policy, and
accepted `GW_*` interfaces. Prismdrake may provide effect intent/geometry but must not
reimplement compositor behavior or imitate native blur through screenshot
capture. Audio, networking, Bluetooth, power, device management,
authorization, and login/session services remain external authorities exposed
through typed adapters rather than embedded replacement implementations.

Each state domain has one authority; UI state is a mirror/cache. Window state
comes from the active WM or accepted interface, never process-name heuristics.
Settings/theme publication uses complete validated generation-tagged snapshots
and never exposes half-applied generations. Shell, decorator, or settings
failure must not destroy WM state, permanently break or strand windows, or
corrupt configuration. Optional-service failure degrades only its feature;
restart uses bounded backoff and safe mode.

## 2. Components and integration

Accepted process roles are:

- `prismdrake-session`: environment, startup, supervision, safe mode,
  logout/recovery;
- `prismdrake-shell`: panel, launcher, desktop, task UI, quick settings,
  notifications, status, and OSDs;
- `prismdrake-settingsd`: validated settings, XSettings, profile switching,
  snapshots/broadcasts;
- `prismdrake-notifyd`: notification service, policy, history, routing;
- `prismdrake-decor`: optional decorations from authoritative WM state;
- `prismdrake-control`: settings application;
- `prismdrake-portal`: desktop portal integration;
- `prismdrake-polkit-agent`: isolated authorization prompts;
- `prismdrake-lock`: secure lock coordination only after Accepted platform
  support and threat model;
- `prismdrake-themes`: data-only visual assets/tokens; and
- optional toolkit adapters for Qt/GTK integration.

Shell surfaces remain modules of one shell process unless an Accepted ADR
justifies another daemon for fault, security, dependency, or measured
performance isolation.

Use standards-first XDG base directories, Desktop Entry, Icon Theme, MIME and
default-app standards, ICCCM/EWMH, D-Bus, XSettings, Freedesktop notifications,
StatusNotifierItem, portals, and autostart where applicable. `GW_*`
enhancements remain generic, versioned, optional, capability-negotiated, and
separately accepted; candidate names remain draft until Glasswyrm accepts them.
Every enhancement has a standards/reduced-feature fallback, and support is
never inferred from executable/process names. Do not claim implementation from an
interface sketch. Blur belongs to the compositor, never screenshot capture;
when unavailable, use ordinary alpha/opaque materials. Capture/thumbnails
require privacy and authorization review.

ADR states are exact: Proposed is non-authoritative; Accepted is authoritative;
Rejected was intentionally not selected; Superseded was replaced; Deprecated
is temporary/legacy. Code existence does not accept a Proposed ADR, and an
unrelated patch must not alter an Accepted decision. Material architecture
changes require an ADR plus affected specification updates. Accepted ADRs have
no unresolved `TODO`, `TBD`, or `???`; record alternatives/consequences and
define validation where evidence is incomplete. Never invent package sizes,
dependency counts, performance figures, or compatibility results.

## 3. Technology and dependencies

Accepted direction is Qt 6.11 Quick+ for visible shell surfaces; Rust by
default for toolkit-neutral policy, models, parsers, services, and validation;
retained C++20 for Qt-bound presentation or justified native integration;
Cargo plus tracked lockfile for Rust; CMake/CTest for retained Qt/C++/QML and
native targets; versioned TOML user configuration; versioned JSON schemas for
themes/normalized snapshots; and Experimental versioned D-Bus XML. Public
`GW_*` payloads/interface stability remain
Proposed/Experimental until explicitly accepted. A new production stack needs
an Accepted decision.

Core runtime must not require GNOME Shell, Mutter, `gnome-settings-daemon`,
`gnome-control-center`, or libadwaita. GTK/Qt adapters stay optional and
isolated where practical; GTK itself is not forbidden, Qt need not be linked into every
component, and core startup must not require both stacks. Keep GUI toolkits out
of nonvisual services where practical, do not require systemd exclusively,
keep build tools out of runtime metadata, prefer system libraries over hidden
vendored copies, and
explicitly justify mandatory dependencies with reproducible feature detection.
Missing optional dependencies disable only their feature.

Follow workspace licensing and existing declarations. Do not incidentally
change Prismdrake's declared license; intentional relicensing is explicit and
coordinated under root policy. Use original Prismdrake geometry, iconography,
mockups, sounds, wallpapers, and visual language, plus one semantic
token/component system for Lustre and Forge. Record source, author, license,
modifications, and attribution for third-party assets. Never commit Microsoft
or other proprietary icons, logos, sounds, wallpapers, fonts, copied UI artwork,
or pixel-for-pixel geometry. Themes are data-only for 1.0 unless an
Accepted security design says otherwise; never load arbitrary executable theme
scripts.

## 4. Security, privacy, accessibility, and data

Treat config, themes, desktop entries, notification content, D-Bus,
window metadata, portal results, and native capability payloads as untrusted. Validate
types, ranges, sizes, versions, and paths; reject traversal and unsupported
versions.
Never execute Desktop Entry commands through an implicit shell or expose
unrestricted arbitrary-key mutation over D-Bus. Privileged prompts stay
minimal and isolated; do not store secrets in ordinary configuration. Prefer
fail-closed authorization and fail-soft optional visuals.

Do not log notification bodies, window titles, private paths, credentials,
tokens, or keys by default. Production developer overrides are off.
Notification history needs explicit retention/clear controls. Capture/thumbnails respect
privacy, authorization, and lock state. Do not claim secure locking until the
full threat model and implementation are reviewed; a fullscreen overlay is not
a secure lock. Security-sensitive parsers/interfaces need focused negative tests and
fuzzable boundaries where practical. Workspace `../CLAUDE.md` §1 still governs
substantive security review.

Accessibility is functional correctness. Primary workflows must be keyboard-operable with
deterministic visible focus and correct accessible names, roles, and states;
color cannot be the sole signal. Both profiles must support high contrast, reduced
motion, text scaling, disabled transparency, and non-blur fallbacks. These
overrides survive profile switches and remain independent from profile
defaults. Minimum target sizes live in shared design tokens/tests; animation
must not block input or essential actions, and accessibility regressions block
release.

Lustre and Forge share primitive, semantic, component, and accessibility token
layers; every required semantic token exists in both profiles, every blur
material has a non-blur fallback, and focus/pressed/active/urgent/disabled/error
states remain explicit. Lustre is translucent, layered, and prismatic but
readable, with compositor blur, restrained highlights/shadows, brief purposeful
motion, and designed opaque/reduced-transparency fallbacks. Forge is mostly
opaque, tactile, crisp, and compact, with stronger borders/separation, sharper
geometry, shorter motion, and clear active/pressed/urgent states. Do not create
separate shell implementations.

Review UI changes for keyboard, screen reader, contrast, motion, target size,
text scaling, wallpaper/fallback material, touch/hover assumptions, and text expansion/RTL. Label mockups
as mockups; change visual baselines intentionally, never en masse merely to hide
regressions.

Follow XDG paths; never hard-code a home. Config/token documents carry explicit
schema versions, and unsupported versions fail safely. Writes are atomic,
permission-preserving, recoverable, retain prior valid data during migration,
and keep packaged defaults available for recovery. Profile switching atomically
publishes one complete validated generation. JSON uses UTF-8, stable machine identifiers,
deterministic formatting, and versioned strict-enough schemas. TOML uses
lowercase `snake_case` keys unless an Accepted schema says otherwise, explicit
types, and documented units. D-Bus XML must parse; Prismdrake interfaces begin
`org.prismdrake.`, have explicit draft/stable status, remain narrow and typed,
and document timeout, cancellation, error, ownership, version, and stability
semantics. No ABI stability is implied before acceptance.
Interface/schema/implementation/docs changes land coherently.

## 5. Implementation and build boundaries

Prefer explicit ownership, small APIs/modules, actionable errors,
bounded work/queues/retries, cancellation and shutdown, monotonic duration
clocks, and event-driven behavior. No blocking I/O belongs on UI or D-Bus
dispatch threads.

- Pure Rust domain crates forbid unsafe. Isolate reviewed unsafe/FFI with local
  rationale and malformed-boundary tests; no panic/unwind crosses C/C++/Qt,
  D-Bus, process, X11, or another foreign boundary.
  Use checked conversions, domain types, bounded collections, explicit
  cancellation, and explicit locked reviewed dependencies/features compatible
  with authoritative offline distribution builds. APIs at untrusted boundaries
  are cancellation-aware.
  Do not use the workspace Wyrmroot Rust fork as a Prismdrake
  toolchain/dependency without a separately Accepted requirement.
- C++ uses C++20, namespace `prismdrake`, RAII/value semantics, and
  `std::unique_ptr` by default; shared ownership is only for genuinely shared
  lifetimes. Avoid owning raw pointers and manual `new`/`delete`, keep headers
  self-contained, and keep toolkit-neutral service APIs free of Qt types unless
  intentionally Qt-bound. Follow local formatting/tooling conventions;
  exceptions do not cross C, Rust, Qt, D-Bus, process, or other foreign
  boundaries.
- QML owns layout, visual state, bindings, and
  deterministic disable-able animation—not persistence, parsing, D-Bus,
  authoritative models, JavaScript business-logic islands, context-property
  sprawl, undocumented global singletons, GUI-thread I/O, or hard-coded profile
  values. Use narrow typed models/commands/adapters.
- X11/D-Bus input is untrusted. Respect ICCCM/EWMH where used; never assume
  XIDs survive async work, remove stale state promptly, use the documented
  application-identity fallback rather than one field, and never bypass the WM
  for state changes. D-Bus handlers validate caller, arguments, and state; use
  stable typed errors; avoid long synchronous work; define authorization
  expectations; handle service disappearance/name reacquisition; and use
  bounded state-aware retries. Internal objects are not accidental public API.

Inspect actual build files/presets before choosing commands. Do not add a second build
system without an Accepted decision. Keep build trees, caches, binaries, IDE
or machine state, and generated outputs untracked unless a contract requires deterministic
generated source. Required generated source documents its deterministic
generator and is never hand-edited. Preserve local/user changes, avoid unrelated
formatting churn, and follow `.editorconfig` and repository formatters where
present.

## 6. Performance and internationalization

Performance guardrails are functional, not permission for premature
micro-optimization. Common input actions should acknowledge within 100 ms
under normal load; keep UI-thread work bounded and launcher search incremental
and cancellable; avoid continuous timers where events suffice; and render at
output refresh cadence when capacity permits. Measure before claiming an
improvement, and give release budgets a documented reference machine/method.

User-facing strings are translatable. Avoid fragile concatenation and fixed
English widths, allow text expansion, use locale-aware formatting, account for
RTL layout, and search localized Desktop Entry fields. Exercise long-string
and RTL cases for substantial UI work when infrastructure exists.

## 7. Validation and completion

The separate `prismdrake-vm` domain on `qemu:///system` is Prismdrake's required
build/test environment, but it is not covered by root's standing `OS-Project`
VM exception and gains no authority here. Compilation, formatting, linting,
tests, packaging, and runtime validation run there only with Mike's exact active
authorization for that domain and operation, under root-coordinator ownership.
Workers must not access VM/control/data endpoints. The source host is limited
to repository edits, inspection, and lightweight static checks. Historical VM
paths, packages, state, or evidence require live preflight and do not transfer
acceptance.

Discover actual commands from checked-out build files/docs rather than
inventing them. Use the narrowest relevant validation first, then documented
repository-wide gates. `make validate` or CMake configure/build/CTest apply only once the
checked-out repository actually exposes those interfaces. Never silently skip
or report an unrun test.

Behavior changes get tests; parsers and schemas get negative cases. Prefer
deterministic display-free tests, Xvfb/Xephyr for X11 integration, and isolated
D-Bus sessions where appropriate. Relevant coverage includes service loss,
invalid input, stale windows/generations, output hotplug/mixed scaling,
fallbacks, both profiles, high contrast, reduced motion, disabled transparency,
and missing blur when affected. Hardware/GPU tests remain opt-in when the
environment cannot guarantee them.

Visual tests control fonts, animation time, scale, locale, profile, capability
set, and output size; review baseline changes intentionally. Security-sensitive
changes should exercise relevant malformed/oversized input, version rejection,
traversal, D-Bus validation, Desktop Entry shell-injection avoidance,
notification/theme parsing, capture authorization, and lock-state restrictions.

Before claiming completion, verify the requested scope only, canonical names,
component ownership, Accepted/Proposed status, fallbacks/failure behavior,
accessibility/security/privacy impact, dependency boundaries, relevant
docs/contracts, and focused tests/evidence. Do not overstate
toolkit coherence, protocol support, lock security, compatibility, measurements,
or validation. Report exact environmental blockers and remaining owner
decisions. Git,
commit, branch, push, and reporting mechanics come only from `../CLAUDE.md`.
This file does not override them.
