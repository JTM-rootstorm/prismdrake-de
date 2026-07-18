# ADR 0003: Visible shell toolkit and language direction

- **Status:** Accepted
- **Date:** 2026-07-16
- **Compatibility floor amended:** 2026-07-18
- **Owners:** Prismdrake maintainers

## Context

Prismdrake needs a visible-shell toolkit that supports original composited
surfaces without moving policy into view code or adding uncontrolled mandatory
dependencies. An early experiment must not silently become permanent
architecture.

## Decision drivers

- Composited rendering, translucency, and controlled animation.
- Typed C++ models and separation of UI from policy.
- Keyboard and assistive-technology accessibility.
- X11, HiDPI, multi-output, input, and test practicality.
- Dependency, licensing, and Gentoo packaging reviewability.
- One token-driven implementation for Lustre and Forge.

## Considered options

The weighted [toolkit evaluation](../research/toolkit-evaluation.md) compares Qt
6 Quick, Qt 6 Widgets, GTK 4, Slint, and a custom rendering/toolkit layer. Qt 6
Quick scores highest for the visible shell. Qt Widgets and GTK remain credible
for conventional applications, while Slint warrants reconsideration if its
desktop integration evidence improves. A custom toolkit imposes unacceptable
accessibility and input ownership for the current scope.

## Decision

Use Qt 6.11 Quick or newer for visible `prismdrake-shell` surfaces and modern
C++ for models, services, and integration. Keep persistent policy, parsing,
system integration, and authoritative models outside QML. Use QML for layout,
visual state, bindings, accessibility metadata, and brief interruptible
animation.

Require Qt 6.11 or newer for the visible shell and its Qt-bound presentation
adapters. Qt 6.11.1 in the Gentoo reference environment is the verified
component baseline. Qt 6.4 compatibility is not a supported target and must not
constrain implementation or validation.

Keep non-visual services toolkit-neutral where practical. This decision does
not select Qt for every component or make Qt and GTK jointly mandatory. The
build and language baseline is governed separately by
[ADR 0008](0008-build-language-and-testing-baseline.md).

## Consequences

The shell can share one declarative component set across Lustre and Forge, but
must police QML business-logic islands, UI-thread work, global singletons, and
hard-coded theme values. Qt modules and transitive packages require explicit
license and dependency review. Custom controls require accessibility tests.
Distribution packages and build environments must supply the declared Qt 6.11
floor; older hosted environments may validate repository contracts but cannot
provide product compatibility evidence.

## Validation or evidence

The [PD1 toolkit spike](../research/pd1-toolkit-spike.md) and
[Gentoo dependency evidence](../research/pd1-gentoo-dependency-evidence.md)
cover the observed single-output X11, keyboard, AT-SPI, scaling, reduced-motion,
disabled-transparency, restart, and dependency behavior used for this decision.
Real multi-output, mixed-DPI, real-GPU, alternate-WM, and full screen-reader
behavior remains PD1 validation work rather than established support.

## Revisit conditions

Revisit if the spike fails a required accessibility or X11 behavior, if the
mandatory dependency graph violates policy, if deterministic tests cannot be
made reliable, or if another candidate demonstrates materially better evidence
on the weighted criteria.

## References

- [Toolkit evaluation](../research/toolkit-evaluation.md)
- [Dependency policy](../architecture/dependency-policy.md)
- [Project specification section 12](../PRISMDRAKE_PROJECT_SPECIFICATION.md#12-technology-and-dependency-policy)
