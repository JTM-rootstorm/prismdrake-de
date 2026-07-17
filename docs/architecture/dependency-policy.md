# Dependency policy

This policy implements `PD-DEP-001` through `PD-DEP-008`. The initial PD1
foundation is an internal static library and introduces no installed production
runtime dependency. Python 3.11 and GNU Make remain development-only
contract-validation tools.

## Dependency classes

Every dependency declaration must identify:

- the consuming component and feature;
- build-only, test-only, mandatory runtime, or optional runtime scope;
- direct use versus transitive arrival;
- separately identified declared constraints, observed versions, and verified
  minimum versions;
- license and distribution packaging status;
- behavior when an optional dependency is absent; and
- the reason a small project utility is insufficient.

System libraries are preferred to hidden vendored copies. Feature detection is
explicit and reproducible; a process or executable name is not capability
detection.

## Mandatory runtime boundary

Core startup must not require GNOME Shell, Mutter,
`gnome-settings-daemon`, `gnome-control-center`, or libadwaita. GTK itself is
not forbidden, but a GTK-specific adapter must not make the GNOME desktop stack
or both GUI toolkit stacks mandatory. Non-visual services avoid GUI toolkits
where practical.

Systemd may be supported as an optional session integration, but the baseline
supervisor remains init-neutral. Theme packages are data-only for 1.0 and must
not pull in an entire shell, control center, or executable scripting runtime.

## Optional adapters

`prismdrake-style-qt`, `prismdrake-theme-gtk`, `prismdrake-portal`, service
adapters, and native Glasswyrm integration are separately installable where
practical. Missing optional dependencies disable only their related feature and
produce actionable diagnostics. Tier B and Tier C fallbacks remain available.

## Review triggers

A new mandatory runtime dependency requires review of ownership, security,
startup, size and performance measurements, packaging availability, licensing,
upgrade behavior, and alternatives. New toolkit types must not leak through a
toolkit-neutral public boundary without an Accepted reason.

Build generators, linters, schema validators, and test harnesses stay out of
runtime metadata. GPU and display-dependent tests remain opt-in when a normal
environment cannot guarantee them; they do not replace display-free checks.

## Component manifests

PD1 component boundaries use strict machine-readable manifests under
[`manifests/dependencies/`](../../manifests/dependencies/), validated against
the [dependency-manifest schema](../../schemas/prismdrake-dependency-manifest.schema.json).
The [dependency-manifest guide](../build/dependencies.md) defines version
evidence, planned versus implemented status, runtime measurement, packaging,
license-review, and fallback semantics.

The implemented foundation manifest records current repository and build/test
tools. Session, settings, and shell manifests are explicitly planned and
unmeasured: observed Gentoo VM versions are evidence, not supported minima or
proof of a production runtime closure. Declared constraints remain distinct
from tested lower bounds. Validation rejects the forbidden GNOME desktop stack
in mandatory core runtime scope and rejects verified minima that lack component
evidence.
