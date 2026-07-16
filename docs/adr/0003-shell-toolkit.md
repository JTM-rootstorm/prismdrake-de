# ADR 0003: Visible shell toolkit and language direction

- **Status:** Proposed
- **Date:** 2026-07-15
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

Propose Qt 6 Quick for visible `prismdrake-shell` surfaces and modern C++ for
models, services, and integration. Keep persistent policy, parsing, system
integration, and authoritative models outside QML. Use QML for layout, visual
state, bindings, accessibility metadata, and brief interruptible animation.

Keep non-visual services toolkit-neutral where practical. This proposal does
not select Qt for every component, accept CMake, or make Qt and GTK jointly
mandatory.

## Consequences

The shell can share one declarative component set across Lustre and Forge, but
must police QML business-logic islands, UI-thread work, global singletons, and
hard-coded theme values. Qt modules and transitive packages require explicit
license and dependency review. Custom controls require accessibility tests.

## Validation or evidence

Current evidence is documented in the research matrix and official toolkit
references. Acceptance requires an isolated PD1 X11 spike covering keyboard
navigation, AT-SPI, scaling, reduced motion, disabled transparency,
deterministic rendering, restart behavior, and an actual Gentoo dependency
manifest. No production toolkit dependency is added by PD0.

## Revisit conditions

Revisit if the spike fails a required accessibility or X11 behavior, if the
mandatory dependency graph violates policy, if deterministic tests cannot be
made reliable, or if another candidate demonstrates materially better evidence
on the weighted criteria.

## References

- [Toolkit evaluation](../research/toolkit-evaluation.md)
- [Dependency policy](../architecture/dependency-policy.md)
- [Project specification section 12](../../Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md#12-technology-and-dependency-policy)

