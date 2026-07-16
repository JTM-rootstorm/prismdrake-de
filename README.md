# Prismdrake Desktop Environment

Prismdrake is a traditional desktop environment for Linux, initially focused on
X11. It aims for a polished, expressive, accessible desktop with a
standards-based baseline and optional, capability-negotiated enhancements when
running with Glasswyrm.

Prismdrake is currently in **PD0: identity, contracts, and repository
foundation**. This repository does not yet contain a usable desktop shell. PD0
defines the project boundaries, schemas, visual language, interface drafts, and
validation needed before implementation begins.

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

- [Project-wide specification](Docs/PRISMDRAKE_PROJECT_SPECIFICATION.md)
- [Documentation index](docs/index.md)
- [Architecture Decision Records](docs/adr/README.md)
- [Roadmap](docs/roadmap/milestones.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

Canonical names and namespaces are `Prismdrake Desktop Environment`,
`prismdrake-*`, and `org.prismdrake.*`. The canonical repository is
`JTM-rootstorm/prismdrake-de`.

## Validation

PD0 contracts are validated with:

```sh
make validate
```

The validator uses Python 3.11 or newer and the standard library. It validates
schemas, examples, interface XML, theme parity, Architecture Decision Record
structure, and repository-local links. This is contract validation, not a
desktop build.

## License

Prismdrake is licensed under the [GNU General Public License, version 3](LICENSE).
Third-party assets require compatible licensing and recorded provenance.

