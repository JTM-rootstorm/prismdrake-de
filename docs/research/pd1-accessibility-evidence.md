# PD1 production-shell accessibility evidence

- **Status:** Observed build-tree reference-guest interaction evidence
- **Date:** 2026-07-18
- **Scope:** PD1-WP13 platform bridge, focus traversal, and bounded task actions

## Evidence boundary

The production `prismdrake-settingsd` and `prismdrake-shell` processes were
tested together under disposable Xvfb, Openbox, D-Bus, and AT-SPI sessions in
the Gentoo reference VM. The test did not replace the shell with a QML fixture.
Application discovery and the two task windows were controlled through private,
bounded fixtures.

This evidence supports the platform-bridge portion of `PD-A11Y-001` through
`PD-A11Y-003`, `PD-A11Y-011`, `PD-A11Y-012`, and the launcher behavior in
`PD-LAUNCH-005`. The task-action extension also supports `PD-PANEL-003`,
`PD-WIN-004`, `PD-WIN-005`, `PD-A11Y-004`, and `PD-A11Y-006` through
`PD-A11Y-009`. It does not establish complete screen-reader support or close the
broader accessibility requirements by itself.

## Observed environment and result

The reference guest used GCC 15.3.0, Qt 6.11.1, Xvfb, Openbox,
`at-spi2-core`, Python GObject introspection, and `xdotool`. The focused build
compiled the production settings daemon and shell with warnings as errors. The
display-free evidence contract and live platform case both passed:

```text
AccessibilityEvidenceContractTest ... Passed
LiveAtspiAccessibilityTest .......... Passed
```

The corrected live case completed in 0.49 seconds. Before passing, it exposed
two harness defects rather than product defects: the isolated environment had
not created Prismdrake's required private runtime subdirectory, and the
launcher heading and pane intentionally shared the accessible name
`Applications`. The final harness creates the documented mode-0700 runtime
boundary and resolves a control by both name and role.

The later task-action demonstration archive
`prismdrake-pd1-action-demo-final-v6.tar.gz` has SHA-256
`7d41d316dd21184a009087bd494134e7f65ecf3022b48a468c5d766a01fda437`.
Its strict contract and live case passed 2/2; the live case completed in 2.55
seconds. The resulting mode-0600 evidence document has SHA-256
`524764a6517a6fc282386b3d06496c270412cc3ed25915a38171f71b5fd93957`.

## Observed sequence

The strict version-one evidence recorded seven ordered phases:

1. The mapped panel exposed launcher and diagnostics push buttons with
   descriptions, `Press` actions, and enabled/focusable state.
2. Invoking `Open applications` through AT-SPI opened the production launcher
   and focused its editable search field.
3. Tab moved focus to the single synthetic application result.
4. Backtab returned focus to search.
5. Escape dismissed the launcher and returned focus to the panel launcher.
6. Panel Tab moved focus to diagnostics.
7. Panel Backtab returned focus to the launcher control.

The launcher pane, search field, and result were observed with pane,
editable-text, and push-button roles respectively. The missing-compositor
environment selected the declared opaque fallback, and that state was included
in the fixed panel and launcher descriptions.

The task-action extension then recorded these exact behaviors:

1. The shell and AT-SPI application owners matched before input, and both task
   buttons exposed push-button semantics, focusability, and fixed descriptions.
2. Shift+F10 opened `Window actions` for the focused first task; Space invoked
   Minimize, and the later EWMH observation contained `_NET_WM_STATE_HIDDEN`.
3. Return on that task requested activation, after which the authoritative state
   no longer contained the hidden atom.
4. A bounded secondary-button click opened the second task's action surface;
   the AT-SPI `Press` action on its Close menu item removed that exact window and
   fixture process.
5. The first task was minimized again, reopened with disabled Minimize and Close
   as the deterministic focus target, and Space removed that exact window and
   process.

The action surface exposed `PopupMenu` and `MenuItem` roles, task-specific names
and descriptions, deterministic focus, minimum token-sized targets,
one-open-menu behavior, and Escape restoration. The generic task-icon claim is
deliberately limited to semantics in live evidence; the code-native glyph
geometry is covered by the QML test lane.

## Evidence privacy and failure behavior

The emitted documents include fixed environment labels, bounded fixture counts,
allow-listed control names and descriptions, normalized roles and actions,
boolean states, and exact focus targets. They exclude
the session and AT-SPI addresses, display number, PIDs, X11 window IDs,
filesystem paths, unrestricted window titles, arbitrary desktop entries, and
the full accessibility tree.

The semantic validator rejects missing or reordered phases, wrong focus,
duplicate controls, absent button actions, unexpected descriptions, and private
runtime data. Runtime traversal is bounded to 1,024 nodes, 32 levels, 256
children per node, and 16 actions per control. Process startup, focus polling,
session duration, and termination are also bounded.

## Reproduction

The launcher-specific build-tree and installed-artifact commands, dependency
behavior, schema, and remaining limitations are documented in
[Live accessibility testing](../development/accessibility-testing.md). The
integrated task-action build-tree command is documented there as well;
installed-artifact replay passed as part of the exact
[PD1 Portage lifecycle](pd1-portage-lifecycle-evidence.md).

## Remaining limitations

- No screen reader, speech output, or natural-language announcement workflow
  was exercised.
- Production notifications are not integrated into the shell composition root,
  so this lane does not make a live notification accessibility claim.
- The deterministic desktop entry does not expose private user application
  names or exercise arbitrary localized content.
- Multi-output, mixed-scale, alternate-WM, non-English, and complete
  screen-reader workflows remain separate later-milestone evidence
  requirements. The exact installed package passed the bounded live AT-SPI
  replay. RTL has a deterministic visual candidate but no live AT-SPI replay.
- Visible focus, contrast, motion, text scale, target sizes, profile parity, and
  non-color cues remain covered by the QML and visual lanes rather than inferred
  from AT-SPI state.
