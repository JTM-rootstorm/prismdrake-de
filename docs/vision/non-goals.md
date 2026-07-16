# Product and PD0 non-goals

PD0 does not implement a production panel, launcher, task list, desktop,
decorator, control center, notification server, settings daemon, portal,
Polkit agent, lock screen, XSettings manager, toolkit style, or Glasswyrm
protocol. Static mockups and contract validation are not prototypes of a usable
desktop.

Core Prismdrake 1.0 also excludes:

- X11 server, window-manager, compositor, focus, stacking, workspace, capture,
  blur, and output-composition policy;
- a required file manager, terminal, browser, editor, office suite, store, or
  login manager;
- Wayland support in the initial release;
- a general third-party in-process plugin ABI without an Accepted security and
  compatibility design;
- systemd as the only supported supervisor;
- a decorative overlay presented as secure session locking;
- forced visual restyling of applications that own their presentation;
- executable theme scripts for 1.0; and
- copied Microsoft artwork, fonts, sounds, logos, terminology, or pixel-exact
  interface geometry.

Future companion applications, Wayland support, secure locking, and extension
models require separate scope and accepted designs. They must not block the
current milestone or appear as implemented features.

Relevant requirements: `PD-SCOPE-001` through `PD-SCOPE-010`.

