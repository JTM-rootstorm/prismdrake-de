# PD2 installed multi-output demonstration harness

The PD2 installed demonstration is a bounded validation driver for the
Portage-installed Experimental core shell. It is not an interactive desktop
launcher, a replacement for CTest, or evidence that the current revision has
passed. A result becomes PD2 evidence only after it is collected in the Gentoo
reference environment, validates against the tracked schema, and is bound to
the complete package lifecycle record.

The harness addresses the installed integration portions of `PD-OUTPUT-001`
through `PD-OUTPUT-004`, `PD-PANEL-001` through `PD-PANEL-005`,
`PD-LAUNCH-001` through `PD-LAUNCH-005`, `PD-NOTIFY-001` through
`PD-NOTIFY-007`, `PD-QUICK-001` through `PD-QUICK-006`, `PD-SESSION-001`
through `PD-SESSION-006`, `PD-A11Y-001` through `PD-A11Y-012`,
`PD-REL-001` through `PD-REL-010`, and `PD-TEST-001` through `PD-TEST-009`.

## Controlled environment

Each run creates private, bounded instances of:

- Xorg with the dummy video driver and two same-scale outputs;
- Openbox as the Tier B reference window manager;
- one isolated session D-Bus;
- AT-SPI for exact shell-owned controls and actions;
- the five Portage-installed `prismdrake-*` executables; and
- controlled desktop-entry, application-window, notification, configuration,
  state, and wallpaper fixtures.

The adapter observes X11 topology and windows through reviewed `xrandr`,
`xprop`, and `xwininfo` paths. Bounded XWD fingerprints establish selected
opaque surface transitions without retaining screenshots. Gentoo's
`/usr/bin/x11trace` supervises the Tier C proxy and rejects shell-side capture
or forbidden compositor operations. `/usr/bin/xtrace` is not accepted because
that path is Gentoo's unrelated dynamic-loader diagnostic.

The harness attests the private session bus, activates the standard
accessibility bus through its `GetAddress` method, and requires a bounded
registry reply before starting the installed Prismdrake session. The observer
connects lazily after the shell has registered and remains process-scoped
across replay epochs; exact shell-process matching rejects stale accessible
applications.

The environment deliberately provides no Glasswyrm-native capability and does
not require compositor blur. Output names, display addresses, bus addresses,
process identities, XIDs, window titles, notification content, application
arguments, and private paths never enter the normalized result.

## Prerequisites

Run from an exact source checkout matching the installed package. Complete the
`USE=test` Portage install and capture the non-final installed-artifact
attestation first, as documented in the
[Gentoo package lifecycle](../packaging/gentoo-local-repository.md#product-lifecycle-evidence-contract).
The attestation must bind the same clean source revision and bundle `HEAD`,
ebuild bytes, complete harness manifest, controlled fixture, executable set,
installed build-provenance file, ownership set, and packaged data that the
demonstration uses.

For the complete Portage lifecycle, copy the controlled fixture into the
private evidence root before unmerge and use that durable copy for both runs.
After the ordinary reinstall, run the second demonstration from the original
clean exact input checkout after re-verifying its revision and status. Do not
depend on the retained Portage work tree: the ordinary reinstall is permitted
to remove it.

The required installed observer boundary includes Xorg dummy, Openbox,
`dbus-daemon`, `xrandr`, `xprop`, `xwininfo`, `xdotool`, `xwd`, AT-SPI with
Python introspection, and Gentoo `/usr/bin/x11trace`. The driver fails closed
rather than silently skipping a required observer.

## Run and validate

Use an absolute private output path whose parent already exists and whose final
path does not exist. The driver atomically creates the completed record at mode
`0600`; it never replaces retained evidence. The controlled window fixture must
be an absolute regular executable from the matching tested build artifact:

```sh
python3 tests/integration/pd2_demo.py \
  --attestation /absolute/private/evidence/pd2-installed-preflight.json \
  --controlled-window-fixture /absolute/path/prismdrake-controlled-window-fixture \
  --output /absolute/private/evidence/pd2-installed-demonstration.json
```

Revalidate retained normalized evidence without starting a display:

```sh
python3 tests/integration/pd2_demo.py \
  --validate-evidence /absolute/private/evidence/pd2-installed-demonstration.json
```

The live command checks installed executable hashes and the canonical harness
manifest both before and after the run. It refuses a source/attestation
mismatch, changed helper/schema/fixture, missing installed executable,
unreviewed observer path, pre-existing output file, unsafe output parent,
non-private result, or incomplete cleanup.

## Workflow boundary

The primary sequence exercises one coherent installed session:

1. establish the two-output X11 environment and component readiness;
2. observe desktop, wallpaper, and configured panels on both outputs;
3. open the launcher through `prismdrake-shellctl`, search, pin, launch two
   controlled windows, group them, activate, minimize, reactivate, close, and
   observe authoritative removal;
4. create, replace, act on, suppress, bypass, inspect, and clear real
   notifications through the standard and Experimental typed service paths;
5. open quick settings, switch Lustre to Forge and back, and correlate complete
   settings generations while preserving accessibility preferences;
6. remove and re-add the primary output, migrate surfaces, and refresh topology;
7. restart settingsd, notifyd, and shell while checking owner-epoch recovery,
   application-window survival, and notification-state survival; and
8. request and cancel logout once, request and confirm it, observe bounded
   reverse-order Prismdrake shutdown, preserve unrelated processes, and clean
   every private test resource.

The failure phase independently covers topology add/remove/primary/geometry
changes, component restart, WM replacement, no-compositor and missing-blur
operation, invalid wallpaper, malformed pins and persistent notification
history, conflicting notification ownership, X server loss, and safe-mode
entry/exit. A scenario must produce its exact normalized observation or the
complete run fails.

## Tier C replay

The Tier C phase repeats desktop, panel, launcher, task action, notification,
notification-center, quick-settings, and logout operations without a
compositor. Every surface must independently establish:

- an opaque material;
- no shell-side capture;
- no blur attempt;
- complete keyboard operation; and
- readable text with visible focus.

This replay proves only the bounded standards fallback exercised by the
harness. It does not qualify other window managers, physical mixed-DPI
hardware, a real GPU, Glasswyrm integration, or production visual quality.

## Cleanup and evidence privacy

Every phase has one mandatory exact-child cleanup result. Timeout and error
paths use bounded termination and still require the driver to prove that its
Prismdrake children, fixtures, WM, bus, X server, sockets, and runtime tree are
gone. The harness does not kill unrelated applications or the host/guest window
manager.

The version-one evidence contains fixed identifiers, boolean observations,
categorical results, normalized hashes, and opaque process-ownership
distinctions only. It contains no unrestricted command output, screenshots,
accessibility trees, message contents, or user identifiers. The final Portage
lifecycle collector consumes this normalized record; the demonstration cannot
declare unmerge, user-state preservation, ordinary reinstall, linkage, or the
PD2 milestone complete by itself.

## Known harness limits

- The live topology claim is two same-scale outputs. Different device-pixel
  ratios have display-free coverage, but full physical mixed-DPI hardening is
  Deferred.
- Openbox is the single Tier B reference WM for this milestone. Alternate-WM
  qualification is Deferred.
- AT-SPI checks exact reviewed controls and state transitions; they do not run
  a full screen reader or validate natural-language speech output.
- XWD records reviewed sample fingerprints, not approved production visual
  baselines.
- The harness implements no native `GW_*` protocol and cannot be used as
  Glasswyrm-native evidence.
