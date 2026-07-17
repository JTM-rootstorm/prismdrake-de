# Prismdrake Desktop Environment

Prismdrake is a traditional desktop environment for Linux, initially focused on
X11. It aims for a polished, expressive, accessible desktop with a
standards-based baseline and optional, capability-negotiated enhancements when
running with Glasswyrm.

Prismdrake is currently in **PD1: X11 shell skeleton and settings foundation**.
The PD0 architecture decisions and PD1 scope were approved on 2026-07-16, and
prototype implementation is active. This repository does not yet contain a
usable desktop shell or production-ready desktop environment.

## Profiles

- **Prismdrake Lustre** (`lustre`) is the translucent, layered, prismatic
  profile. Compositor blur is optional and always has a readable non-blur
  fallback.
- **Prismdrake Forge** (`forge`) is the mostly opaque, tactile, compact profile.

Both profiles use one semantic token system and shared component code. They are
original Prismdrake designs; familiar desktop ergonomics do not imply copied
artwork, names, or geometry.

## Architecture boundary

Prismdrake owns the session and desktop experience: the panel, launcher,
desktop surface, task presentation, settings, notifications, toolkit
integration, and fallback selection. Glasswyrm owns X11 server behavior,
window-management policy, authoritative window state, composition, blur
execution, capture, and other compositor effects.

The standard X11 and freedesktop path is the baseline. Optional Glasswyrm
interfaces use generic, versioned `GW_*` names and require capability
negotiation. Prismdrake never implements backdrop blur by capturing and
blurring the desktop itself.

## Project map

- [Project-wide specification](docs/PRISMDRAKE_PROJECT_SPECIFICATION.md)
- [Documentation index](docs/index.md)
- [Architecture Decision Records](docs/adr/README.md)
- [Roadmap](docs/roadmap/milestones.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

Canonical names and namespaces are `Prismdrake Desktop Environment`,
`prismdrake-*`, and `org.prismdrake.*`. The canonical repository is
`JTM-rootstorm/prismdrake-de`.

## Validation

Repository contracts are validated with:

```sh
make validate
```

The validator uses Python 3.11 or newer and the standard library. It validates
schemas, examples, interface XML, theme parity, Architecture Decision Record
structure, and repository-local links. This remains contract validation; PD1
compiled targets use the separately documented CMake and CTest paths as they
are introduced.

## License

Prismdrake is licensed under the [GNU General Public License, version 3](LICENSE).
Third-party assets require compatible licensing and recorded provenance.
