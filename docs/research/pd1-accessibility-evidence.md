# PD1 production-shell accessibility evidence

- **Status:** Observed build-tree reference-guest smoke evidence
- **Date:** 2026-07-17
- **Scope:** PD1-WP13 platform-bridge and focus-traversal evidence

## Evidence boundary

The production `prismdrake-settingsd` and `prismdrake-shell` processes were
tested together under disposable Xvfb, Openbox, D-Bus, and AT-SPI sessions in
the Gentoo reference VM. The test did not replace the shell with a QML fixture.
Only application discovery was controlled through one private synthetic desktop
entry.

This evidence supports the platform-bridge portion of `PD-A11Y-001` through
`PD-A11Y-003`, `PD-A11Y-011`, `PD-A11Y-012`, and the launcher behavior in
`PD-LAUNCH-005`. It does not establish complete screen-reader support or close
the broader accessibility requirements by itself.

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

## Evidence privacy and failure behavior

The emitted document includes fixed environment labels, the fixture profile and
count, five allow-listed control names and descriptions, normalized roles,
the `Press` action, three boolean states, and exact focus targets. It excludes
the session and AT-SPI addresses, display number, PIDs, X11 window IDs,
filesystem paths, unrestricted window titles, arbitrary desktop entries, and
the full accessibility tree.

The semantic validator rejects missing or reordered phases, wrong focus,
duplicate controls, absent button actions, unexpected descriptions, and private
runtime data. Runtime traversal is bounded to 1,024 nodes, 32 levels, 256
children per node, and 16 actions per control. Process startup, focus polling,
session duration, and termination are also bounded.

## Reproduction

The build-tree and installed-artifact commands, dependency behavior, schema,
and remaining limitations are documented in
[Live accessibility testing](../development/accessibility-testing.md).

## Remaining limitations

- No screen reader, speech output, or natural-language announcement workflow
  was exercised.
- Production notifications are not integrated into the shell composition root,
  so this lane does not make a live notification accessibility claim.
- The deterministic desktop entry does not expose private user application
  names or exercise arbitrary localized content.
- Real application task buttons, multi-output, mixed-scale, alternate-WM,
  non-English, right-to-left, and installed-package replay remain separate
  evidence requirements.
- Visible focus, contrast, motion, text scale, target sizes, profile parity, and
  non-color cues remain covered by the QML and visual lanes rather than inferred
  from AT-SPI state.
