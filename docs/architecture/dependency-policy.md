# Dependency policy

This policy implements `PD-DEP-001` through `PD-DEP-010`. PD0 introduces no
production runtime dependency; Python 3.11 and GNU Make are development-only
contract-validation tools.

## Dependency classes

Every dependency declaration must identify:

- the consuming component and feature;
- build-only, test-only, mandatory runtime, or optional runtime scope;
- direct use versus transitive arrival;
- minimum and tested versions, once measured;
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

## Future manifest

When compiled code is introduced, each component should provide a
machine-readable dependency manifest with this conceptual shape:

```json
{
  "schema_version": 1,
  "component": "prismdrake-shell",
  "dependencies": [
    {
      "name": "example-library",
      "scope": "mandatory_runtime",
      "feature": "visible_shell",
      "minimum_version": "measured-by-packaging",
      "fallback": null
    }
  ]
}
```

The literal placeholder values above describe a future schema and are not a
dependency claim. Validation will reject forbidden desktop-stack components in
mandatory runtime scope when manifests exist.

