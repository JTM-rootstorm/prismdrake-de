# Prismdrake Agent Instructions

The workspace root `../AGENTS.md` applies here and owns coordination, worker topology, model allocation, external-write/VM boundaries, Git/commit/push policy, licensing selection, and general validation/security discipline. This file contains only Prismdrake-specific technical/product doctrine.

## 1. Identity and authority

Canonical names:

- Product: **Prismdrake Desktop Environment**
- Short name: **Prismdrake**
- Profiles: **Prismdrake Lustre** (`lustre`) and **Prismdrake Forge** (`forge`)
- Package/executable prefix: `prismdrake-*`
- D-Bus namespace: `org.prismdrake.*`
- Glasswyrm-native interface family: generic `GW_*`
- Repository: `JTM-rootstorm/prismdrake-de`

Do not introduce alternate product/profile/interface names without an intentional Accepted decision. Do not use Microsoft product/theme names as Prismdrake component, profile, theme, or feature names.

Read, when present and relevant:

1. `docs/PRISMDRAKE_PROJECT_SPECIFICATION.md`
2. `docs/index.md` and durable README setup/overview material
3. Accepted ADRs under `docs/adr/`
4. relevant architecture/security/design/schema/interface documents
5. current milestone/roadmap/issue acceptance criteria
6. older active phase plans, then Proposed ADRs/research notes

Conflict precedence inside Prismdrake:

1. Mike's current task instruction
2. latest committed project specification
3. newer Accepted ADRs that explicitly update/supersede affected specification text
4. other Accepted contracts/schemas/interfaces
5. current milestone/issue criteria
6. Proposed ADRs, research, mockups, and older attachments

Do not silently resolve authoritative contradictions. Preserve the higher-precedence decision, implement non-conflicting work, and report the conflict.

Milestone discipline remains binding: no later-phase feature creep, no production claim from a prototype, no milestone closure without its documented gate, and no stale status left behind when the project advances.

## 2. Ownership boundaries

Prismdrake owns desktop session/startup/recovery, shell surfaces, profile/appearance/settings UX, notification policy/history, toolkit settings propagation/adapters, desktop-specific portal integration, effect/capability requests, fallbacks, and user-visible diagnostics.

Glasswyrm owns X11 server behavior, window-management policy and authoritative window state, focus/stacking/workspaces, move/resize/minimize/maximize/fullscreen, composition/output authority, backdrop blur execution, compositor shadows/effects, scene capture/window thumbnails, input/output policy, and accepted native `GW_*` interfaces.

Prismdrake may request an effect and provide intent/geometry. It must not reimplement compositor behavior or simulate native backdrop blur by capturing/blurring desktop screenshots.

Audio, networking, Bluetooth, power, device management, authorization, and login/session services remain authoritative external domains. Prismdrake presents them through typed adapters rather than embedding replacement implementations.

State/fault rules:

- Every state domain has one authoritative owner; UI models are mirrors/caches.
- Window state comes from the active WM/accepted interface, never process-name heuristics.
- Settings/theme publication uses validated generation-tagged snapshots; never expose half-applied generations.
- Shell/decorator/settings-service failure must not destroy WM state, permanently break windows, or corrupt configuration.
- Missing optional services degrade only their feature; restarts use bounded backoff and safe-mode behavior.

## 3. Working component model

Follow Accepted ADRs when present. Until superseded:

| Component | Responsibility |
|---|---|
| `prismdrake-session` | session environment, startup, supervision, safe mode, logout/recovery |
| `prismdrake-shell` | panel, launcher, desktop, task UI, quick settings, status, notifications, OSDs |
| `prismdrake-settingsd` | validated settings, XSettings, profile switching, snapshots/broadcasts |
| `prismdrake-notifyd` | notification service, policy, history/routing |
| `prismdrake-decor` | optional server-side decorations from authoritative WM state |
| `prismdrake-control` | settings application |
| `prismdrake-portal` | desktop portal backend integration |
| `prismdrake-polkit-agent` | isolated authorization prompts |
| `prismdrake-lock` | secure lock coordination only after accepted threat model/platform support |
| `prismdrake-themes` | data-only visual assets/tokens |
| toolkit adapters | optional Qt/GTK integration packages |

Panel/launcher/desktop/task/quick-settings/notification/OSD surfaces remain logical modules of `prismdrake-shell` unless an Accepted ADR justifies another process boundary. A new daemon needs concrete fault-isolation, security, dependency, or performance justification.

## 4. Standards and native integration

Use a standards-first baseline with optional Glasswyrm enhancements. Prefer XDG base directories, Desktop Entry/Icon Theme/MIME/default-app standards, ICCCM/EWMH, D-Bus, XSettings, Freedesktop notifications, StatusNotifierItem, portals, and standard autostart contracts where applicable.

Native Glasswyrm rules:

- `GW_*` proposals are generic, versioned, optional, and capability-negotiated.
- Candidate names remain draft until Glasswyrm accepts them separately.
- Every native enhancement has a standards/reduced-feature fallback.
- Never infer support from executable/process names.
- Do not claim implementation from an interface sketch alone.
- Scene capture/window thumbnails require privacy/authorization review.
- When blur is unavailable, use ordinary alpha/opaque materials rather than imitation capture blur.

## 5. Decision and technology discipline

ADR states mean exactly: Proposed = non-authoritative, Accepted = authoritative, Rejected = intentionally not selected, Superseded = replaced, Deprecated = temporary/legacy.

- Do not mark a Proposed ADR Accepted because code exists or change an Accepted architecture decision in an unrelated patch.
- Material architecture changes require an ADR plus affected specification updates.
- Accepted ADRs contain no unresolved `TODO`, `TBD`, or `???` markers.
- Record alternatives/consequences and define validation where evidence is incomplete.
- Never invent package sizes, dependency counts, performance figures, or compatibility results.

Accepted technology direction currently includes Qt 6.11 Quick+ for visible shell surfaces; Rust by default for toolkit-neutral policy/models/parsers/services/validation; modern C++20 for retained Qt-bound presentation/justified native integration; Cargo+lockfile for Rust; CMake/CTest for retained Qt/C++/QML/native targets; TOML user configuration; JSON schemas for themes/normalized snapshots; and versioned Experimental D-Bus XML under `org.prismdrake.*`.

Public `GW_*` payloads/interface stability remain Proposed/Experimental until explicitly accepted. Do not add a new production technology stack without an Accepted decision.

## 6. Dependency boundaries

Mandatory core runtime dependencies must not include GNOME Shell, Mutter, `gnome-settings-daemon`, `gnome-control-center`, or libadwaita. GTK itself is not forbidden, and Qt need not be linked into every component.

- Keep GTK/Qt adapters optional and isolated where practical.
- Core startup must not require both toolkit stacks.
- Non-visual services avoid GUI toolkit dependencies where practical.
- Do not make systemd the only supported session supervisor.
- Keep build-only tools out of runtime metadata.
- Prefer system libraries over hidden vendored copies.
- Justify new mandatory runtime dependencies; feature detection must be explicit and reproducible.
- Missing optional dependencies disable only their related feature.

## 7. Originality, assets, and licensing

Follow the workspace licensing policy and existing committed declarations. Prismdrake's current project license must not be changed incidentally; any intentional relicensing must be explicit and coordinated with root policy.

Use original Prismdrake geometry, iconography, mockups, sounds, wallpapers, and visual language. Do not commit Microsoft/proprietary icons, logos, sounds, wallpapers, fonts, copied UI artwork, or pixel-for-pixel proprietary geometry. Record source/author/license/modifications/attribution for third-party assets.

Themes are data-only for 1.0 unless an Accepted security design says otherwise. Do not load arbitrary executable scripts from theme packages.

## 8. Security and privacy specifics
Treat configuration, themes, desktop entries, notification data, D-Bus messages, window metadata, portal responses, and native capability payloads as untrusted.

- Validate types, ranges, sizes, versions, and paths; reject traversal and unsupported versions.
- Never execute Desktop Entry commands through an implicit shell.
- Do not expose unrestricted arbitrary-key mutation over D-Bus.
- Keep privileged prompts minimal and isolated; do not store secrets in ordinary config.
- Do not log notification bodies, window titles, private paths, credentials, tokens, or keys by default.
- Developer overrides are off in production by default.
- Notification history needs explicit retention/clear controls.
- Capture/thumbnails require privacy, lock-state, and authorization handling.
- Do not claim secure locking until the full threat model/implementation is reviewed; a fullscreen overlay is not a secure lock screen.
- Prefer fail-closed authorization and fail-soft optional visuals.

Security-sensitive parsers/interfaces need focused negative tests and fuzzable boundaries where practical. Workspace Daybreak rules still govern substantive security review.

## 9. Accessibility and visual system

Accessibility is functional correctness. Primary workflows must be keyboard-operable with deterministic visible focus and correct accessible names/roles/states. Color cannot be the sole signal.

Both Lustre and Forge must support high contrast, reduced motion, text scaling, and disabled transparency. Accessibility overrides survive profile switches and remain independent from profile defaults. Minimum target sizes belong in shared design tokens/tests. Animation must not block input or essential actions, and accessibility regressions block release.

Lustre and Forge share one semantic token/component system; do not create separate shell implementations. Use primitive, semantic, component, and accessibility token layers, with every required semantic token defined in both profiles and every blur material having a non-blur fallback. Focus, pressed, active, urgent, disabled, and error states stay explicit.

Profile direction:

- **Lustre:** translucent/layered/prismatic but readable; compositor-provided blur when available; restrained highlights/shadows; brief purposeful motion; designed opaque/reduced-transparency fallbacks.
- **Forge:** mostly opaque/tactile/crisp/compact; stronger borders/separation; sharper geometry/shorter motion; clear active/pressed/urgent states.

Profile switching publishes one validated generation atomically. Mockups are labeled as such. Visual baseline changes are intentional, never mass-updated merely to hide regressions.

For UI changes, consider keyboard, screen-reader, contrast, motion, target size, wallpaper/fallback material, touch/hover assumptions, and text scaling.

## 10. Configuration, schemas, and interfaces

- Follow XDG directory semantics; never hard-code a home directory.
- Config/token documents carry explicit schema versions; unsupported versions fail safely.
- Writes are atomic, preserve permissions, retain previous valid config during migration, and leave packaged defaults available for recovery.
- Profile switching never publishes mixed generations.
- Interface/schema/implementation/docs changes land coherently.

JSON uses UTF-8, stable machine identifiers, deterministic formatting, and versioned strict-enough schemas. TOML uses lowercase `snake_case` keys unless an Accepted schema says otherwise, explicit types, and documented units.

D-Bus XML must parse. Prismdrake interfaces begin `org.prismdrake.`; draft/stable status is explicit; methods/signals stay narrow and typed; timeout/cancellation/error/ownership/version semantics are documented; no ABI stability is implied before acceptance.

## 11. Implementation boundaries

General implementation favors explicit ownership, small modules/public APIs, actionable errors, bounded queues/work, explicit cancellation/shutdown, monotonic clocks for durations, event-driven behavior, and no blocking I/O on UI or D-Bus dispatch threads.

### Rust

Rust is the default for toolkit-neutral policy, models, parsers, services, and validation semantics.

- Pure domain crates forbid unsafe code; boundary modules isolate reviewed unsafe with local rationale and malformed-boundary tests.
- Panics/unwinds do not cross C/C++/Qt/D-Bus/process/X11/other foreign boundaries.
- Use checked conversions, explicit domain types, bounded collections, and cancellation-aware APIs at untrusted boundaries.
- Keep Cargo dependencies/features explicit, locked, reviewed, and compatible with authoritative offline distribution builds.
- Do not use the workspace Wyrmroot Rust fork as a Prismdrake toolchain/dependency without a separately Accepted requirement.

### C++ / Qt / QML

Retained C++ uses C++20, namespace `prismdrake`, and local formatting/tooling conventions. Prefer value semantics/RAII and `std::unique_ptr`; use shared ownership only when lifetime truly requires it. Avoid owning raw pointers/manual `new`/`delete`; keep headers self-contained and toolkit-neutral service APIs free of Qt types unless intentionally Qt-bound. Exceptions must not cross C, Rust, Qt, D-Bus, process, or other foreign boundaries.

Qt Quick/QML owns layout, visual state, bindings, and animation, not persistent policy, parsing, D-Bus integration, or authoritative models. Avoid JavaScript business-logic islands, context-property sprawl, undocumented global singletons, GUI-thread I/O, and hard-coded profile values. Expose narrow typed models/commands and deterministic/disable-able animation for tests.

### X11 and D-Bus

Treat X11 properties/window metadata and all D-Bus input as untrusted. Respect ICCCM/EWMH where used; never assume XIDs survive async work; remove stale state promptly; use the documented application-identity fallback chain rather than one field; route state-changing window requests through the authoritative WM path.

D-Bus handlers validate callers/arguments, use typed stable errors, avoid long synchronous work, define authorization expectations, handle service disappearance/name reacquisition, and use bounded state-aware retries. Internal implementation objects are not accidental public API.

### Build and generated-file hygiene

Inspect actual build files/presets before choosing commands. Do not introduce a second build system without an Accepted decision. Keep build directories, caches, binaries, IDE/machine state, and other generated outputs untracked unless a repository contract explicitly requires generated source. Required generated source must be deterministic, document its generator, and not be hand-edited.

Preserve local/user changes and avoid unrelated formatting churn. Follow `.editorconfig` and repository formatters where present.

## 12. Performance and internationalization

Performance rules are functional guardrails, not permission for premature micro-optimization:

- common input actions should acknowledge within 100 ms under normal load;
- keep UI-thread work bounded and launcher search incremental/cancellable;
- avoid continuous timers when events suffice;
- render at output refresh cadence when capacity permits;
- measure before claiming performance improvements; release budgets need a documented reference machine/method.

User-facing strings are translatable. Avoid fragile concatenated fragments/fixed English widths; allow text expansion; use locale-aware formatting; account for RTL layout; search localized desktop-entry fields. Substantial UI work should exercise long-string and RTL cases when infrastructure exists.

## 13. Validation

Use the narrowest relevant validation first, then documented repository-wide gates. Discover actual commands from the checked-out build files/docs rather than inventing them.

Typical contract work may expose `make validate`; CMake work may use configure/build/CTest once those interfaces actually exist.

Test posture:

- behavior changes get tests; parsers/schemas get negative tests;
- prefer deterministic display-free model/service tests;
- use Xvfb/Xephyr for X11 integration and isolated D-Bus sessions where appropriate;
- cover service disappearance, invalid input, stale windows, output hotplug/mixed scaling, and fallback paths;
- cover Lustre, Forge, high contrast, reduced motion, disabled transparency, and missing blur where the affected UI makes them relevant;
- hardware/GPU tests remain opt-in when the environment cannot guarantee them;
- never silently skip or claim an unrun test.

Visual tests control fonts, animation time, scale, locale, profile, capability set, and output size; baseline changes are reviewed intentionally.

Security-sensitive changes should exercise relevant malformed/oversized input, version rejection, traversal, D-Bus validation, Desktop Entry shell-injection avoidance, notification/theme parsing, capture authorization, and lock-state restrictions.

## 14. Repository-specific completion

Before claiming Prismdrake work complete, verify the requested scope only, canonical names, component ownership, Accepted-vs-Proposed status, fallbacks/failure behavior, accessibility/security/privacy impact, dependency boundaries, relevant docs/contracts, and focused tests.

Do not overstate toolkit coherence, protocol support, lock security, compatibility, measurements, or validation. Report exact environmental blockers and remaining owner decisions.

Git/commit/branch/push/reporting mechanics come exclusively from the root `AGENTS.md`; this file does not override them.
