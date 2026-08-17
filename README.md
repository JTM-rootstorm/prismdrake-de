# Prismdrake Desktop Environment

Prismdrake is a traditional desktop environment for Linux, initially focused on X11. It aims to provide a polished, expressive, accessible desktop with familiar workflows, clear failure behavior, and a standards-based interoperability baseline.

## Intent

Prismdrake is intended for users who prefer direct, discoverable panel-and-launcher workflows across GTK, Qt, SDL, and traditional X11 applications. It owns the desktop session and shell experience rather than a complete application suite.

The desktop uses original visual language shared by two profiles:

- **Prismdrake Lustre** (`lustre`) is translucent, layered, and prismatic.
- **Prismdrake Forge** (`forge`) is mostly opaque, tactile, and compact.

Both profiles use the same semantic design tokens and component model. Accessibility, legible fallbacks, and predictable interaction remain requirements regardless of visual effects.

Canonical project identifiers use the `prismdrake-*` package and executable prefix and the `org.prismdrake.*` D-Bus namespace. The canonical repository is `JTM-rootstorm/prismdrake-de`.

## Architecture boundary

Prismdrake owns session startup, shell surfaces, application presentation, settings, notifications, toolkit integration, appearance, and fallback selection. External system services remain authoritative for audio, networking, Bluetooth, power, devices, authorization, and login. Prismdrake may present them through typed adapters but does not reimplement their authority.

X11, ICCCM, EWMH, XDG, D-Bus, XSettings, and freedesktop protocols form the portable baseline. [Glasswyrm](https://github.com/JTM-rootstorm/glasswyrm) is optional, not a prerequisite. Any accepted Glasswyrm integration must use explicit, versioned, capability-negotiated interfaces and preserve a standards-based fallback. The architecture requires readable non-effect fallbacks.

Glasswyrm or the active window manager and compositor retain authority over window state, focus, stacking, workspaces, composition, capture, and output policy. Prismdrake requests behavior and presents state without duplicating that authority.

Prismdrake interfaces and builds are Experimental unless a dedicated support document states otherwise. The project makes no README-level claim of secure session locking, privileged controls, stable APIs, or completed reference-environment or hardware validation.

## Documentation

- [Product specification](docs/PRISMDRAKE_PROJECT_SPECIFICATION.md)
- [Documentation index](docs/index.md)
- [Product vision](docs/vision/product.md)
- [Architecture overview](docs/architecture/overview.md)
- [Compatibility architecture](docs/architecture/compatibility.md)
- [Design principles](docs/vision/design-principles.md)
- [Architecture decisions](docs/adr/README.md)
- [Roadmap and project status](docs/roadmap/milestones.md)
- [Building](docs/build/building.md)
- [Testing](docs/build/testing.md)

## License

Prismdrake is licensed under the [GNU General Public License v3.0](LICENSE). Third-party assets require compatible licensing and recorded provenance.
