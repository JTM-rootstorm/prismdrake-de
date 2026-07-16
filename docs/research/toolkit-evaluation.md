# Visible shell toolkit evaluation

- **Status:** Research supporting a Proposed decision
- **Reviewed:** 2026-07-15
- **Scope:** PD0 qualitative comparison; no toolkit was built or packaged

This evaluation supports [ADR 0003](../adr/0003-shell-toolkit.md). Scores use a
1–5 scale, where 5 best satisfies the criterion. Weighted points equal
`score / 5 * weight`. Scores compare suitability for Prismdrake's visible shell,
not the general quality of a toolkit. Package counts, installed sizes, runtime
memory, and distribution availability were not measured and must be verified in
a PD1 spike.

## Results

| Candidate | Rendering 20 | C++ split 15 | A11y 15 | Dependency 15 | X11 10 | Theme 10 | Tests 10 | License/package 5 | Total / 100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Qt 6 Quick | 5 | 5 | 4 | 3 | 5 | 5 | 4 | 4 | **88** |
| Qt 6 Widgets | 3 | 5 | 5 | 3 | 5 | 3 | 5 | 4 | **81** |
| GTK 4 | 4 | 3 | 5 | 3 | 5 | 4 | 4 | 5 | **80** |
| Slint | 4 | 5 | 3 | 4 | 3 | 5 | 4 | 4 | **80** |
| Custom layer | 5 | 5 | 1 | 1 | 3 | 5 | 2 | 3 | **64** |

The close middle scores are intentional: each candidate has credible strengths.
Qt 6 Quick leads because the shell requires declarative, animated,
composited surfaces and a strong C++ model boundary. Approval still depends on
an accessible X11 prototype and a verified Gentoo dependency manifest.

## Candidate evidence

### Qt 6 Quick

- **Strengths:** a retained scene graph and graphics abstraction fit layered
  translucent surfaces; QML offers declarative bindings and state; typed C++
  models can keep policy outside the view layer. Qt Quick exposes accessible
  roles, actions, relationships, names, and descriptions, and standard controls
  include keyboard support.
- **Risks:** custom visual items require explicit accessibility metadata and
  keyboard behavior. The runtime graph is broader than a tiny custom layer, and
  required modules must be audited rather than assumed. Deterministic rendering
  and X11 multi-output behavior need a project-specific spike.
- **Evidence:** [Qt Quick scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html),
  [Qt Quick accessibility](https://doc.qt.io/qt-6/accessible-qtquick.html), and
  [Qt licensing](https://doc.qt.io/qt-6/licensing.html). Qt documents open-source
  licensing on a module-by-module basis, so PD1 must record exactly which
  modules Prismdrake uses.

### Qt 6 Widgets

- **Strengths:** mature desktop controls, keyboard behavior, accessibility,
  C++ APIs, and test facilities. It is practical for conventional settings UI.
- **Risks:** widget painting and styling are less natural for the shell's
  composited, fluid visual model. Achieving Lustre materials could encourage
  custom painting that works against standard controls. It remains a plausible
  control-center or utility choice, not the leading shell surface choice.
- **Evidence:** [Qt Widgets accessibility](https://doc.qt.io/qt-6/accessible.html)
  and [Qt Test](https://doc.qt.io/qt-6/qttest-index.html).

### GTK 4

- **Strengths:** mature Linux integration, an explicit X11 backend, strong
  standard-widget accessibility, CSS-driven styling, and reproducible
  accessibility test support. Official documentation correctly assigns window
  state authority to the X11 window manager.
- **Risks:** the strongest C++ integration is through wrapper libraries rather
  than the primary C API. A GTK shell would make GTK mandatory for visible core
  surfaces and does not itself eliminate the need for Qt application
  integration. Custom shell widgets still require careful accessibility work.
- **Evidence:** [GTK accessibility](https://docs.gtk.org/gtk4/section-accessibility.html),
  [GTK on X11](https://docs.gtk.org/gtk4/x11.html), and
  [GDK X11 integration](https://docs.gtk.org/gdk4/x11.html).

### Slint

- **Strengths:** declarative UI, generated C++ integration, custom visuals, and
  accessibility properties. Its ahead-of-time C++ workflow aligns with a clean
  UI/model split.
- **Risks:** Prismdrake-specific evidence for X11 desktop-shell windows,
  assistive-technology completeness, mixed-output behavior, packaging, and
  deterministic screenshot infrastructure is not yet available. Selecting it
  would place more integration validation on Prismdrake.
- **Evidence:** [Slint C++ integration](https://docs.slint.dev/latest/docs/cpp/)
  and [Slint accessibility properties](https://docs.slint.dev/latest/docs/slint/reference/common/#accessibility-properties).

### Custom rendering or toolkit layer

- **Strengths:** complete control over scene structure, effects, geometry, and
  dependency selection.
- **Risks:** Prismdrake would own focus traversal, text input, accessibility,
  internationalization, scaling, input methods, rendering fallback, automation,
  and long-term maintenance. That cost conflicts with PD0's risk posture. A
  narrow renderer extension may be justified later, but a general toolkit is
  not.
- **Evidence:** this score is an architectural cost assessment, not a claim
  about a specific library.

## Recommendation and validation path

Propose Qt 6 Quick for visible shell surfaces, modern C++ for models and
integration, and toolkit-neutral non-visual services where practical. This is
not approval of Qt for every component.

Before acceptance and PD1 implementation, build an isolated X11 spike that
demonstrates keyboard traversal, AT-SPI exposure, text scaling, reduced motion,
disabled transparency, deterministic rendering, multiple outputs/scales,
session restart, and package dependency capture. Re-score any criterion whose
observed result differs materially from this qualitative review.

