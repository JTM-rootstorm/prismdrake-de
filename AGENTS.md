# AGENTS.md

## Scope

These instructions apply to the entire Prismdrake repository unless a deeper `AGENTS.md` provides more specific rules for a subtree.

Prismdrake is a multi-phase desktop environment project. This file is intentionally project-wide. Do not assume that the repository is still in PD0 or that an old phase plan is current. Inspect the repository, roadmap, issues, and Accepted decisions before choosing scope.

Read this file before making changes.

---

## Canonical project identity

Use these names exactly:

- Product: **Prismdrake Desktop Environment**
- Short name: **Prismdrake**
- Translucent profile: **Prismdrake Lustre**
- Classic profile: **Prismdrake Forge**
- Machine profile identifiers: `lustre`, `forge`
- Package and executable prefix: `prismdrake-*`
- D-Bus namespace: `org.prismdrake.*`
- Glasswyrm-native interface family: generic `GW_*`
- Canonical repository: `JTM-rootstorm/prismdrake-de`
- License: preserve the committed GPL-3.0 `LICENSE`

Do not introduce alternate spellings, abbreviations, namespaces, or profile names without explicit maintainer approval and an Accepted Architecture Decision Record.

Do not use “Aero,” “Luna,” “Windows,” or another Microsoft product name as a Prismdrake component, profile, theme, or feature name.

---

## Governing documents and read order

Before editing, inspect the latest repository state and read, when present:

1. The nearest applicable `AGENTS.md`.
2. `PRISMDRAKE_PROJECT_SPECIFICATION.md`.
3. `README.md` and `docs/index.md`.
4. Relevant Accepted ADRs under `docs/adr/`.
5. Relevant architecture, security, design, schema, and interface documents.
6. The current roadmap, milestone plan, implementation status, and issue acceptance criteria.
7. Earlier phase specifications or execution plans that are still explicitly active.
8. Proposed ADRs and research notes.

The project specification defines project-wide requirements. A milestone plan narrows work for a phase. A milestone plan must not silently contradict the project specification.

### Conflict handling

Use this precedence:

1. Explicit maintainer instructions for the current task.
2. The latest committed project specification.
3. Newer Accepted ADRs that also update or explicitly supersede affected specification text.
4. Other Accepted repository contracts, schemas, and interfaces.
5. Current milestone and issue acceptance criteria.
6. Proposed ADRs, research notes, mockups, and older attachments.

Do not silently choose between conflicting authoritative sources. Preserve the higher-precedence decision, implement non-conflicting work, and report the conflict.

---

## Opening checklist

At the start of every task:

1. Confirm the repository, branch, and worktree.
2. Inspect `git status` and identify uncommitted user changes.
3. Inspect recent commits and relevant files.
4. Determine the current project milestone and maturity.
5. Read the applicable specification sections and Accepted ADRs.
6. Identify the `PD-*` requirement IDs or issue criteria affected.
7. Classify the work as documentation, architecture, schema/interface, implementation, design asset, testing, packaging, or release work.
8. Identify component ownership and external service boundaries.
9. Decide which validation can run in the current environment.
10. Keep the change no broader than necessary.

Never assume the repository still matches an old attachment or chat transcript.

---

## Milestone discipline

- Work only within the current task and milestone unless the maintainer explicitly broadens scope.
- Do not implement later-phase features merely because a nearby interface makes them tempting.
- Do not block a current phase by prematurely designing every post-1.0 possibility.
- Do not mark a milestone complete unless its documented exit gate is satisfied.
- A prototype is not production behavior. Label it clearly.
- Experimental code must be isolated and removable.
- Phase-specific non-goals remain binding while that phase is active.
- When the repository advances to a new phase, update status documentation rather than leaving stale claims.

---

## Architecture invariants

### Prismdrake ownership

Prismdrake owns:

- Desktop session startup and environment.
- Panel, launcher, desktop surface, task presentation, quick settings, status presentation, notification surfaces, and on-screen displays.
- Desktop appearance, profile selection, and user-facing settings.
- Notification service and history policy.
- Toolkit settings propagation and optional style adapters.
- Desktop-specific portal integration.
- Requests for effects and optional native capabilities.
- Fallback selection and user-visible diagnostics.

### Glasswyrm ownership

Glasswyrm owns:

- X11 server behavior.
- Window-management policy.
- Authoritative window state.
- Focus and stacking.
- Move, resize, minimize, maximize, fullscreen, and workspace policy.
- Composition and output composition.
- Backdrop blur execution.
- Compositor shadows and scene effects.
- Scene capture and window thumbnails.
- Input and output policy.
- Accepted native `GW_*` interfaces.

Do not move Glasswyrm responsibilities into Prismdrake.

Prismdrake may request an effect and provide geometry or intent. It must not implement compositor behavior.

Never simulate native backdrop blur by capturing and blurring desktop screenshots in the shell.

### External system services

Audio, networking, Bluetooth, power, device management, authorization, and login/session services remain authoritative for their domains.

The shell may present controls through typed adapters. It must not embed replacement implementations merely to make a button work.

### State authority

- Every state domain has one authoritative owner.
- UI models are mirrors or caches, not alternate sources of truth.
- Window state must come from the active window manager or accepted interface.
- Settings and theme updates should use immutable, validated, generation-tagged snapshots.
- Do not publish half-applied settings.
- Do not use process names as capability detection.

### Fault boundaries

- A shell crash must not destroy WM state.
- A decorator crash must not make windows permanently unusable.
- A settings-service crash must not corrupt configuration.
- A missing optional service must degrade only the affected feature.
- Repeated restarts require bounded backoff and safe-mode behavior.

---

## Working component model

Follow Accepted ADRs when they exist. Until superseded, use this project-wide working model:

| Component | Responsibility |
|---|---|
| `prismdrake-session` | Session environment, startup, supervision, safe mode, logout, and recovery |
| `prismdrake-shell` | Panel, launcher, desktop, task UI, quick settings, status area, notifications, and OSD presentation |
| `prismdrake-settingsd` | Validated settings, XSettings, profile switching, snapshots, and broadcasts |
| `prismdrake-notifyd` | Notification service, policy, history, and routing |
| `prismdrake-decor` | Optional server-side decoration rendering from authoritative WM state |
| `prismdrake-control` | User-facing settings application |
| `prismdrake-portal` | Desktop-specific portal backend integration |
| `prismdrake-polkit-agent` | Isolated authorization prompts |
| `prismdrake-lock` | Secure lock coordination only after accepted threat model and platform support |
| `prismdrake-themes` | Shared data-only visual assets and design tokens |
| `prismdrake-style-qt` | Optional Qt integration package |
| `prismdrake-theme-gtk` | Optional GTK integration package |

Treat panel, launcher, desktop, task presentation, quick settings, notification surfaces, and OSDs as logical modules of `prismdrake-shell` unless an Accepted ADR creates a process boundary.

A new daemon requires a concrete fault-isolation, security, dependency, or performance reason. “It feels modular” is not enough.

---

## Standards and native integration

Use a standards-first baseline and optional Glasswyrm enhancements.

Prefer appropriate established contracts such as:

- XDG base directories.
- Desktop Entry Specification.
- Icon Theme Specification.
- ICCCM and EWMH.
- D-Bus.
- XSettings.
- Freedesktop notifications.
- StatusNotifierItem.
- Portals.
- Standard autostart, MIME, and default-application mechanisms.

Rules:

- Keep Glasswyrm-specific capabilities optional and capability-negotiated.
- Name native proposals with generic, versioned `GW_*` identifiers.
- Treat candidate `GW_*` names as drafts until Glasswyrm accepts them separately.
- Every native enhancement needs a standards or reduced-feature fallback.
- Never infer native support solely from the compositor, executable, or process name.
- Do not claim a protocol is implemented because an interface sketch exists.
- Window thumbnails and scene capture require privacy and authorization review.
- Use ordinary alpha or opaque materials when blur is unavailable.

---

## Decision discipline

### ADR status meanings

- **Proposed:** under review and not authoritative.
- **Accepted:** approved and authoritative.
- **Rejected:** evaluated and intentionally not selected.
- **Superseded:** replaced by a newer ADR.
- **Deprecated:** retained temporarily but no longer recommended.

### Rules

- Do not describe a Proposed decision as settled.
- Do not mark an ADR Accepted because its code exists.
- Do not change an Accepted architectural decision in an unrelated patch.
- A material architecture change requires an ADR and specification update.
- Accepted ADRs must not contain unresolved `TODO`, `TBD`, or `???` markers.
- Record alternatives and consequences, not only the chosen answer.
- When evidence is incomplete, state uncertainty and define a validation path.
- Do not invent package sizes, dependency counts, performance figures, or compatibility claims.

### Technology choices

The following are leading proposals until Accepted ADRs say otherwise:

- Qt 6 Quick for visible shell surfaces.
- Modern C++ for models, services, and integration.
- CMake for builds.
- TOML for user configuration.
- JSON schemas for themes and capability snapshots.
- Versioned D-Bus XML under `org.prismdrake.*`.

Do not add a production technology stack merely to make a proposal look inevitable.

---

## Dependency boundaries

Mandatory core runtime dependencies must not include:

- GNOME Shell.
- Mutter.
- `gnome-settings-daemon`.
- `gnome-control-center`.
- libadwaita.

GTK itself is not forbidden. Qt is not automatically required by every component.

- Keep GTK and Qt adapters optional and isolated where practical.
- Core startup must not require both toolkit stacks.
- Non-visual services should avoid GUI toolkit dependencies when practical.
- Do not make systemd the only supported session supervisor.
- Keep build-only tools out of runtime dependency metadata.
- Prefer system libraries over hidden vendored copies.
- Justify each new mandatory runtime dependency.
- Feature detection must be explicit and reproducible.
- Missing optional dependencies must disable only the related feature.

---

## Originality and licensing

- Preserve `LICENSE` unless the maintainer explicitly changes project licensing.
- Use original Prismdrake geometry, iconography, mockups, sounds, wallpapers, and visual language.
- Do not commit Microsoft icons, logos, sounds, wallpapers, fonts, or copied UI artwork.
- Do not commit proprietary font files.
- Do not recreate proprietary UI pixel-for-pixel through generated geometry.
- Record author, source, license, modifications, and attribution needs for third-party assets.
- Use generic placeholders in early mockups.
- Do not describe Prismdrake as an Aero or Luna implementation.
- Themes must be data-only for 1.0 unless an Accepted security design says otherwise.
- Do not load arbitrary executable scripts from theme packages.

---

## Security and privacy rules

Treat configuration, themes, desktop entries, notification data, D-Bus messages, window metadata, portal responses, and native capability payloads as untrusted.

- Validate types, ranges, sizes, versions, and paths.
- Reject path traversal and unsupported schema versions.
- Do not execute desktop-entry commands through an implicit shell.
- Do not expose unrestricted arbitrary-key mutation over D-Bus.
- Keep privileged prompts in a minimal isolated process.
- Do not store secrets in ordinary Prismdrake configuration.
- Do not log notification bodies, window titles, file paths, credentials, tokens, or private keys by default.
- Developer overrides must be disabled in production by default.
- Notification history must have explicit retention and clearing controls.
- Capture and thumbnail features require privacy, lock-state, and authorization handling.
- Do not claim secure locking until the complete threat model and implementation have been reviewed.
- A decorative full-screen overlay is not a secure lock screen.
- Prefer fail-closed behavior for authorization and fail-soft behavior for optional visuals.

When touching a security-sensitive parser or interface, add negative tests and fuzzable boundaries where practical.

---

## Accessibility rules

Accessibility is part of functional correctness.

- All primary shell workflows must be keyboard operable.
- Focus order must be deterministic and visible.
- Controls must expose accessible names, roles, states, and descriptions.
- Color must not be the only indicator of status or urgency.
- Lustre and Forge must both support high contrast, reduced motion, text scaling, and disabled transparency.
- Accessibility settings must survive profile switches.
- Minimum target sizes belong in design tokens and tests.
- Animation must not block input or delay essential actions.
- Reduced-motion behavior should replace or remove nonessential movement.
- Contrast must be tested over real wallpaper and fallback material conditions.
- Missing hover must not make touch or keyboard operation impossible.
- Accessibility regressions are functional regressions and should block release.

For UI changes, report keyboard, screen-reader, contrast, motion, and target-size impact.

---

## Visual-system rules

Prismdrake Lustre and Prismdrake Forge share one semantic token system and common component code.

- Do not create separate Lustre and Forge shell implementations.
- Use primitive, semantic, component, and accessibility token layers.
- Every required semantic token must exist in both profiles.
- Every blur material must have a non-blur fallback.
- Profile switching should publish one validated generation atomically.
- Keep user accessibility overrides independent from profile defaults.
- Status, focus, pressed, active, urgent, disabled, and error states must be explicit.
- Mockups must be labeled when they are not production design.
- Visual baselines must be reviewed intentionally.
- Do not mass-update screenshots to hide a regression.

### Lustre direction

- Translucent, layered, prismatic, and readable.
- Compositor-provided blur when available.
- Restrained facet highlights and directional shadows.
- Smooth, brief, purposeful motion.
- Designed opaque and reduced-transparency fallbacks.

### Forge direction

- Mostly opaque, tactile, crisp, and compact.
- Stronger borders and surface separation.
- Sharper geometry and shorter motion.
- Saturated but adjustable accents.
- Clear active, pressed, and urgent states.

---

## Configuration and schema rules

- Follow XDG directory semantics.
- Never hard-code a home directory.
- Configuration and token documents require explicit schema versions.
- Unsupported versions must fail safely.
- Writes must be atomic and preserve permissions.
- Preserve the previous valid configuration during migration.
- Packaged defaults must remain available for recovery.
- A profile switch must not publish mixed generations.
- Accessibility preferences must remain independent from profile selection.
- Example configuration must contain no secrets.
- Draft D-Bus and schema interfaces must be labeled draft.
- Interface, schema, implementation, and documentation changes belong in the same coherent patch.

### JSON

- Use UTF-8.
- Use consistent indentation selected by the repository formatter.
- Use stable machine identifiers.
- Keep schemas strict enough to catch errors but extensible through documented versioning.
- Reject duplicate semantic meanings under different keys.

### TOML

- Use lowercase `snake_case` keys unless an Accepted schema defines otherwise.
- Keep examples complete enough to validate.
- Do not rely on implicit type coercion.
- Document units in names or schema descriptions.

### XML and D-Bus

- XML must parse.
- Prismdrake interface names begin with `org.prismdrake.`.
- Stable or draft status must be documented.
- Methods and signals remain narrow and typed.
- Document timeout, cancellation, error, ownership, and version semantics.
- Do not imply ABI stability before it is accepted.

---

## Implementation guidance

Follow the repository's Accepted language, build, formatting, and linting decisions. When no convention exists, avoid creating a broad style policy in an unrelated feature patch.

### General code

- Keep modules small and ownership explicit.
- Prefer RAII and explicit lifetimes.
- Avoid raw owning pointers.
- Avoid global mutable state.
- Use descriptive domain types instead of ambiguous primitives where practical.
- Keep public interfaces small and documented.
- Handle errors explicitly and include actionable context.
- Do not swallow errors silently.
- Do not use exceptions across C, D-Bus, process, or plugin boundaries.
- Avoid hidden threads and unbounded work queues.
- Make cancellation and shutdown behavior explicit.
- Keep blocking work off UI and D-Bus dispatch threads.
- Use monotonic clocks for durations and timeouts.
- Avoid busy polling. Prefer event-driven operation.
- Do not add a dependency to avoid writing a small, testable utility without justification.

### Conditional C++ rules

When C++ is present and no narrower convention supersedes these rules:

- Use the standard selected by the Accepted build ADR.
- Use namespace `prismdrake` with narrower nested namespaces.
- Prefer `#pragma once` unless the repository selects include guards.
- Use `std::unique_ptr` for sole ownership and `std::shared_ptr` only for demonstrated shared lifetime.
- Prefer value types and `std::optional` or an accepted result type for absence and errors.
- Avoid owning raw pointers and manual `new` or `delete`.
- Mark non-owning pointers and references clearly.
- Use `[[nodiscard]]` for results that must be checked.
- Keep headers self-contained.
- Avoid exposing Qt types from toolkit-neutral service interfaces unless the component is explicitly Qt-bound.
- Add API documentation for public classes, interfaces, and non-obvious lifetime rules.

Do not impose naming or formatting changes across unrelated files. Use `.clang-format`, `.clang-tidy`, and established local conventions when present.

### Conditional Qt and QML rules

When Qt 6 Quick is Accepted or already present:

- Keep persistent policy, parsing, D-Bus integration, and authoritative models outside QML.
- Use QML for layout, visual state, bindings, and animation.
- Avoid JavaScript business-logic islands.
- Avoid context-property sprawl and undocumented global singletons.
- Expose narrow, typed models and commands.
- Do not block the GUI thread with disk, network, D-Bus, image, or search work.
- Ensure QObjects have clear ownership and thread affinity.
- Use stable object names or accessibility identifiers for tests where appropriate.
- Bind colors, spacing, radii, motion, and metrics through theme tokens.
- Avoid hard-coded profile-specific values inside components.
- Support deterministic animation time or disabled animations in visual tests.
- Check keyboard focus and accessible metadata for each interactive component.

### X11 and window-model rules

- Treat X11 properties and window metadata as untrusted.
- Respect ICCCM and EWMH semantics where used.
- Never assume window identifiers remain valid after asynchronous work.
- Subscribe to lifecycle changes and remove stale state promptly.
- Do not infer application identity from one field when a documented fallback chain is required.
- Respect skip-taskbar, desktop, dock, override-redirect, transient, and modal semantics as applicable.
- Send state-changing requests through the authoritative WM path.
- Keep X11 error handling explicit and non-fatal where recovery is possible.

### D-Bus rules

- Validate all inbound arguments.
- Use typed errors and stable names.
- Avoid long-running synchronous method handlers.
- Define caller expectations and authorization requirements.
- Handle service disappearance and name reacquisition.
- Make retries bounded and state-aware.
- Do not expose internal implementation objects as accidental public API.

---

## Performance rules

- Common input actions should acknowledge within 100 ms under normal load.
- Keep UI-thread work bounded.
- Do not perform synchronous file discovery or indexing during launcher presentation.
- Use incremental and cancellable search.
- Render at the output refresh cadence when capacity permits.
- Avoid continuous timers when events are available.
- Treat blur quality as a negotiated and adjustable effect.
- Measure before optimizing, but do not merge obvious unbounded work.
- Add benchmarks for code that affects startup, search, frame time, wakeups, or large model updates.
- Do not claim performance improvements without measurements.
- Numeric release budgets need a documented reference machine and method.

---

## Internationalization rules

- User-facing strings must be translatable.
- Do not concatenate translatable fragments into grammatically fragile sentences.
- Allow text expansion.
- Avoid fixed widths that assume English labels.
- Use locale-aware date, time, number, and measurement formatting.
- Consider right-to-left layout for panel, launcher, menus, notifications, and settings.
- Search localized desktop-entry fields.
- Do not commit proprietary font files.
- Test at least one long-string and one right-to-left scenario for substantial UI work when infrastructure exists.

---

## Build and repository hygiene

### Before editing

- Inspect existing build files and presets.
- Do not assume CMake or command names before they exist.
- Do not generate a second build system without an Accepted decision.
- Preserve local and uncommitted user changes.

### Generated files

- Do not commit build directories, caches, object files, generated binaries, local IDE state, or machine-specific files.
- Commit generated source only when the repository policy requires it and regeneration is deterministic.
- Include the generator command or source contract.
- Do not hand-edit generated files unless explicitly documented.

### File quality

- Use UTF-8.
- Use final newlines.
- Follow `.editorconfig` and repository formatters.
- Do not add empty placeholder directories.
- Do not add placeholder-only files to make a tree look complete.
- Keep relative links valid.
- Keep line endings consistent.
- Avoid unrelated whitespace churn.

### Git safety

- Do not run destructive commands such as `git reset --hard`, `git clean -fd`, force pushes, or history rewrites unless the maintainer explicitly requests them.
- Do not discard uncommitted user changes.
- Do not modify unrelated files merely to make the worktree clean.
- Do not commit, push, create branches, open pull requests, tag releases, or publish artifacts unless the task explicitly requests it.
- When commits are requested, keep them scoped and intentional.

---

## Validation and testing

Use the narrowest relevant validation first, then the repository-wide required checks.

### Discover commands

Inspect `README.md`, `CONTRIBUTING.md`, `Makefile`, `CMakePresets.json`, `CMakeLists.txt`, and CI workflows before choosing commands.

Early contract work may use:

```bash
make validate
```

A CMake-based implementation may use commands such as:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

These are examples, not permission to invent build support that does not exist.

### Required test posture

- Add tests with behavior changes.
- Add negative tests for parsers and schemas.
- Prefer deterministic, display-free tests for models and services.
- Use Xvfb or Xephyr for X11 integration where appropriate.
- Use isolated D-Bus sessions for service tests.
- Cover service disappearance, invalid input, stale windows, output hotplug, and fallback paths.
- Test Lustre, Forge, high contrast, reduced motion, disabled transparency, and missing blur.
- Keep hardware or GPU tests opt-in when the environment cannot guarantee support.
- Do not silently skip required tests.
- Do not claim a test passed unless it ran successfully.
- When a test cannot run, report the exact command, failure, and environmental limitation.

### Visual tests

- Use deterministic fonts available in the test environment.
- Freeze or control animation time.
- Record scale, locale, profile, capability set, and output size.
- Review baseline changes intentionally.
- Never update every baseline solely to make CI green.

### Security tests

For security-sensitive changes, include relevant tests for:

- Oversized and malformed input.
- Unsupported versions.
- Path traversal.
- D-Bus caller and argument validation.
- Desktop-entry execution without shell injection.
- Notification rich-text and image handling.
- Theme and asset parsing.
- Capture or thumbnail authorization.
- Lock-state restrictions.

---

## Working method

### 1. Inspect

- Read the governing documents.
- Inspect relevant code and tests.
- Search for canonical names and conflicting namespaces.
- Find the authoritative owner of the state being changed.
- Identify uncommitted changes and nearby conventions.

### 2. Plan narrowly

For each planned change, know:

- Why it exists.
- Which requirement or issue criterion it addresses.
- Whether it changes an Accepted or Proposed decision.
- Which component owns it.
- What fallback and failure behavior apply.
- How it will be validated.

### 3. Implement

- Preserve existing architecture and local conventions.
- Keep changes focused.
- Add validation with contracts where practical.
- Keep UI, policy, and integration boundaries clear.
- Avoid hidden scope expansion.

### 4. Review the complete diff

Before validation:

- Read every changed line.
- Search for misspelled canonical names.
- Search for unauthorized `org.*` namespaces.
- Search for non-generic native Glasswyrm names.
- Search for `TODO`, `TBD`, and `???` in authoritative material.
- Search for copied proprietary names and assets.
- Check security and privacy impact.
- Check accessibility and fallback behavior.
- Check local links, schemas, examples, and generated files.
- Confirm no unrelated user change was overwritten.

### 5. Validate

- Run focused tests.
- Run contract validation.
- Run the documented build and test suite.
- Record exact commands and results.

### 6. Report

Summarize:

- What changed.
- Which requirements were addressed.
- Which files changed.
- Which decisions remain Proposed or Deferred.
- Which validation commands ran and their outcomes.
- Any skipped checks and exact reasons.
- Accessibility, security, dependency, and Glasswyrm impact.
- Remaining risks or owner decisions.

---

## Change-size guidance

Prefer reviewable increments.

Good changes include:

- One component behavior with tests and documentation.
- One schema with examples, migration, and negative tests.
- One ADR with evidence and accompanying specification updates.
- One visual component family across Lustre, Forge, and accessibility states.
- One adapter with clear optional dependency boundaries.
- One recovery path with failure-injection tests.

Avoid:

- A giant patch mixing architecture, implementation, styling, packaging, and unrelated cleanup.
- Mechanical creation of dozens of empty files.
- Wide formatting changes in a feature patch.
- New daemons without justified ownership.
- New public interfaces without versioning and tests.
- New dependencies hidden in convenience code.
- Implementing a later milestone while the current milestone is unresolved.

---

## Commit and pull-request guidance

Use scoped commit messages when commits are requested or appropriate.

Suggested forms:

```text
docs(identity): define canonical Prismdrake terminology
docs(architecture): clarify shell and Glasswyrm ownership
adr(toolkit): evaluate visible shell implementation choices
feat(shell): add keyboard-accessible task presentation
feat(settings): publish validated generation snapshots
feat(theme): implement Forge component tokens
feat(integration): export XSettings values
fix(notify): preserve replacement semantics
fix(session): bound component restart loops
test(x11): cover stale window lifecycle
ci(validation): verify schemas interfaces and links
packaging(core): install session and D-Bus metadata
```

Pull-request descriptions should include:

- Scope and motivation.
- Requirement IDs and issue criteria.
- Components and interfaces affected.
- Decision-status changes.
- Dependency impact.
- Accessibility impact.
- Security and privacy impact.
- Glasswyrm and standards-baseline impact.
- Fallback behavior.
- Validation commands and results.
- Migration or compatibility impact.
- Owner-review items.

Do not mark a Proposed ADR Accepted merely because a pull request implements it.

---

## Common mistakes to avoid

- Treating Qt 6 Quick as final before owner approval.
- Treating GTK as forbidden when the actual constraint concerns GNOME desktop dependencies.
- Making both GTK and Qt mandatory for core startup.
- Giving Prismdrake ownership of focus, stacking, workspaces, or blur execution.
- Creating Prismdrake-specific names for Glasswyrm protocols.
- Omitting fallbacks for blur, thumbnails, capture, or native capabilities.
- Creating separate Lustre and Forge component implementations.
- Hard-coding theme values inside UI components.
- Treating accessibility as later polish.
- Claiming pixel-perfect toolkit coherence.
- Copying Microsoft geometry, icons, sounds, assets, or terminology.
- Committing proprietary fonts.
- Publishing half-applied settings generations.
- Exposing arbitrary configuration mutation over D-Bus.
- Executing desktop entries through a shell.
- Treating an overlay as a secure lock screen.
- Adding an in-process plugin system without a threat model.
- Assuming systemd is always present.
- Blocking the UI thread with I/O or long D-Bus calls.
- Using process names as capability detection.
- Ignoring output hotplug and mixed scaling.
- Adding placeholder-only files.
- Inventing measurements or test results.
- Ignoring uncommitted user changes.
- Updating documentation in a later, unspecified follow-up after changing behavior now.

---

## Definition of done for a task

A task is complete when:

- The requested scope is implemented and no broader.
- Canonical names remain exact.
- Component ownership remains correct.
- Accepted and Proposed decisions remain clearly separated.
- Relevant specification, ADR, interface, schema, and user documentation are updated.
- Fallback and failure behavior are defined.
- Accessibility impact is addressed.
- Security and privacy impact is addressed.
- No mandatory GNOME desktop-stack dependency was introduced.
- No proprietary or unlicensed asset was introduced.
- No unrelated user change was overwritten.
- Tests cover the changed behavior and negative paths where applicable.
- Required validation passes, or the exact environmental blocker is reported.
- The final response identifies remaining owner decisions and risks.
- No implementation, compatibility, security, or test result is overstated.

---

## Final response template

Use this structure for substantial work:

### Summary

What was completed and why.

### Changed files

Group by component or concern.

### Requirements and decisions

- Requirements addressed.
- Accepted decisions preserved or changed.
- Proposed decisions.
- Deferred or blocked work.

### Validation

List exact commands and outcomes.

### Impact

- Accessibility.
- Security and privacy.
- Dependencies and packaging.
- Standards baseline.
- Glasswyrm integration.
- Compatibility and migration.

### Owner review

List decisions requiring maintainer approval.

### Remaining risks

State concrete known limitations without inventing future completion promises.
