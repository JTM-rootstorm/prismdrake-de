# Rust-first transition baseline

This P0 working record maps the current Prismdrake implementation into the
workspace Rust-first transition. It is an inventory and parity aid, not an
acceptance result. The authoritative sequencing plan is workspace-root
`PRISMDRAKE_RUST_FIRST_AND_WYRROOT_PORTING_PLAN.md`.

## Revision and preservation boundary

- Branch: local `main`.
- Current-history behavioral oracle:
  `98e19c79e0cdc9ccb980d35dc68d671e1d4fe520`.
- Historical tree-equivalent pre-signing oracle:
  `77a82492789fc46a9aee27343abb7357ac968ffb`.
- Oracle tree: `9c1be7f80aae9c704236a7ac416f2d21bd8b26e4`.
- The local head and dirty state must be recorded again when P0 freezes its
  exact accepted baseline. A current checkout is not automatically that
  baseline, and documentation-only descendants do not transfer old execution
  evidence.
- Preserve repository-local `vm-passthrough/` exactly as untracked
  operator/evidence material. It is not a build input or cleanup target.
- Historical PD1/PD2 records retain only the claims, source revision,
  environment, and limitations written in those records.

The immediate superseded workspace plan has SHA-256
`5e3c5644fd3a84fa9d78f806276ca58e456eebc9974ea74a5eebaed694f89ef7`.
The superseded repository-local Rust transition plan has SHA-256
`12324b15092f2d072512fe6f233fb0d620e62c8cb6510f5bdf70d1042ec56b32`.
The former PD2 execution plan has SHA-256
`64675f024c260bb47bde29fe57e8ff1d2cf298ae387f612afb6b8d402fc3217c`.
Deleted older untracked plan bytes are not recoverable from this repository;
their aggregate sorted checksum-manifest hash is retained only in the workspace
plan's consolidation record.

## Component and migration map

| Slice | Current behavior and authority | Boundary and contract | Current parity anchors | Security and failure anchors | Rust cutover gate and outstanding evidence |
|---|---|---|---|---|---|
| Configuration, theme, and settings | `prismdrake-settingsd` owns validated configuration, profile resolution, immutable generations, and runtime snapshots. The dependency-free Rust foundation now contains the first settings-publication state slice, but it is not an authority cutover. | Executable plus `Settings1`; TOML configuration and config/theme/runtime JSON schemas. | `src/config`, `src/theme`, `src/settings`, `src/settingsd`; unit, settings D-Bus, restart, schema, theme, and shell-consumer tests; Rust publication-state unit tests. | Bounded parsing, path/permission checks, atomic writes, same-user control, owner epochs, typed redacted errors. | P3 still ports the complete authority and positive/negative/restart consumers before removing the C++ service. Installed activation, rollback after new-state writes, and final lifecycle remain unevidenced. |
| Session supervision | `prismdrake-session` owns exact-child startup, readiness, restart/backoff, safe mode, logout, and bounded teardown. | Executable plus `Session1`; child environment and process lifecycle. | `src/session`; supervisor unit tests, child fixture, D-Bus/control, restart, logout, exact-environment, and integration lanes. | Shell-free exact argv/environment, monotonic time, exact-child cleanup, bounded signals, same-user control. | P4 ports after Rust settingsd is authoritative. Package checkpoint B, interruption rollback, and final installed lifecycle remain outstanding. |
| Launcher and application identity | Toolkit-neutral discovery, visibility, identity, ranking, generations, cancellation, provenance, and launch planning feed Qt presentation. | Desktop Entry/XDG inputs and in-process Qt adapters; no separate daemon. | `src/launcher`, application-identity, launcher-controller, launcher-QML, detached-application, and provenance fixtures. | Untrusted desktop entries, field-code bounds, literal argv, no implicit shell, path/provenance validation, cancellation and stale-generation rejection. | P5 ports policy/model cores while retaining narrow Qt presentation. Fixture parity, package ownership, and installed demonstrations remain required. |
| Notifications | `prismdrake-notifyd` owns freedesktop notifications, history, IDs, replacement/action policy, DND override, and notification-center generations. | Executable; freedesktop Notifications plus private `NotificationCenter1`; history/state schemas; Qt client/presentation. | `src/notifications`, `src/notifyd`, shell notification modules; model, D-Bus, sender-lifecycle, replacement, QML, history, and negative fixtures. | Bounded untrusted text/hints, sender unique-name lifetime, retention/clearing, typed actions, private state permissions, owner epochs. | P5 ports policy/model cores without broadening the freedesktop contract. Member-level authorization/signal matrix, rollback after writes, and installed lifecycle remain outstanding. |
| X11 and output/task models | XCB/RandR/EWMH adapters provide topology, task state, struts, properties, and requests; the active WM remains authoritative. | Native XCB connection/resource lifetime and standards-based X11 contracts. | `src/x11`, shell output/task/window/controller modules; synthetic protocol fixtures, Xvfb/Openbox, hotplug, stale-window, geometry, and property-worker tests. | Untrusted lengths/properties/IDs, checked geometry, explicit nonfatal X11 errors, stale-window removal, bounded worker cancellation. | P6 ports only pure state and interpretation first. Native connection/resource lifetime stays justified until one audited bridge is accepted; live multi-output remains outstanding. |
| Qt/QML visible shell | `prismdrake-shell` owns presentation, surface placement, panel, launcher, quick settings, notification center, desktop, accessibility metadata, and composition requests. | Qt 6.11 Quick/QML, internal C++ models/adapters, `Shell1`, Experimental session metadata. | Shell runtime/component tests, Qt Core model tests, Quick Test/QML lanes, Tier-C replay, visual/accessibility fixtures. | UI-thread bounds, typed adapters, literal text, deterministic focus, reduced motion, high contrast, no-blur fallback, owner loss/recovery. | Qt/QML remains. P5/P6 narrow retained adapters; one bridge must pass ABI, sanitizer, teardown, and security gates. Live AT-SPI, real screen reader, and two-output installed proof remain outstanding. |
| Shell control | `prismdrake-shellctl` is a narrow command client for Experimental shell/session control. | Executable consuming `Shell1` and `Session1`; no state authority. | `src/shellctl`, shell-control and D-Bus negative tests. | Typed bounded requests, explicit service loss/timeouts, no arbitrary command surface. | Retain or port only after the service contracts and authorization matrix freeze; package and lifecycle ownership remain outstanding. |
| Evidence and lifecycle | Python collectors currently capture and validate PD2 demonstrations, accessibility, performance, Portage lifecycle, and installed attestation. | JSON schemas, bounded collectors, normalized records, exact source/artifact/test identities. | `tests/accessibility`, `tests/performance`, `tests/gentoo`, `tests/integration`; positive and negative fixtures plus saved-evidence validators. | No-follow/regular-file checks, mode constraints, fixed error categories, redaction, source and artifact binding. | P1/P7 split capture, normalization, schema, semantics, and custody; add independent anchoring and explicit retention. Current saved records are not current-head acceptance. |
| Build and package | CMake/CTest owns C++/Qt/QML; the live Gentoo ebuild installs the current five-executable Experimental product. | Root CMake, presets, `make validate`, dependency manifests, ebuild, `DESTDIR`, source-revision provenance. | Contract validation; GCC/Clang presets; sanitizer, format, QML-lint, archive, ebuild, and synthetic lifecycle tests. | System dependencies only, no hidden downloads, exact revision input, sandbox exclusions recorded separately. | P2 adds Cargo and a tracked lockfile only after ADR acceptance and dependency review. Offline package checkpoint A, mixed checkpoint B, and P9 remain outstanding. |

## Exact current contract hashes

These hashes bind the files at the inventory checkout. They do not say that a
runtime or installed test exercised them.

| Contract | SHA-256 |
|---|---|
| `interfaces/dbus/org.prismdrake.NotificationCenter1.xml` | `3bd367d7bb4db46f15aa75bdeabf1fde78231cf60af1e0bd6a70ad6be128d0a8` |
| `interfaces/dbus/org.prismdrake.Session1.xml` | `7e5bc7b6492727cfe5a42116fe836861161eee1ef542f7066f8f638eca290f5d` |
| `interfaces/dbus/org.prismdrake.Settings1.xml` | `192f17046af4de87aa7d697088b8ceddf95981d9a3115fb997ea6993e96c3438` |
| `interfaces/dbus/org.prismdrake.Shell1.xml` | `3150456a9b7c294dec0a2ec8834f8834cbb7b5bc2b2e2d14708a9b43dbbe01a1` |
| `schemas/prismdrake-capabilities.schema.json` | `f7fd9766d003f24743fe5f3e9feb39eea6b97741b0a65aac518c7760966d1c2e` |
| `schemas/prismdrake-config.schema.json` | `4fd5972e3185f6f68e5276f242403ffe89960d64d180dbb7dfc1e6aa2a02fca8` |
| `schemas/prismdrake-dependency-manifest.schema.json` | `2763ea3022c091fae72045aee725652196435dfe25eaade7098a46f7c8ac8ac3` |
| `schemas/prismdrake-notification-history.schema.json` | `3b69cb4710747314f2761648313a6b7295175d426de687863fad44efdfc0afe0` |
| `schemas/prismdrake-notification-state.schema.json` | `e87e32d8515b0af411da11c0e06b470c4795bbefb8ed9017a36b55d5efa4628c` |
| `schemas/prismdrake-pin-state.schema.json` | `a0f797c054810f4755201abf8e497fe46b5cc2deb4245db13add8a807a8c4191` |
| `schemas/prismdrake-runtime-snapshot.schema.json` | `6bf1319e190b00aa95aaba3c05a9592d4f8549a26bb54b11a5a586b34be094d3` |
| `schemas/prismdrake-theme-tokens.schema.json` | `b5b8e05adea017e6372360d5abba889ebc0457dd3b3759bf11f0ebf516945360` |
| `tests/gentoo/pd2-ctest-inventory-v1.json` | `ccdd944bc18fc1adab95cabc1a31b15be060701c74d004f5b4b3027c9919b3dd` |
| Product ebuild | `6938734ec7bd2295051df1f6bb71863c9c909bd566fe980510470343f9498c99` |

The seven dependency manifests are also current transition inputs:

| Manifest | SHA-256 |
|---|---|
| `prismdrake-foundation.json` | `ed5222628ccf671ceb48a94a95189520def94c91d1447af0a63546e2631a2008` |
| `prismdrake-notifyd.json` | `567e939dbf86e5103a83a5c2551bf5d23a335e0a826592d6e884a9c730ba49ca` |
| `prismdrake-session.json` | `92ad2d806974667e8e3dd13027d4de4aad9a8b0ac686024f7ac75988f84053ce` |
| `prismdrake-settingsd.json` | `9cc5f17e09a017273793aee5066f153399db8c90967b5c857fbb86d0aceae680` |
| `prismdrake-shell.json` | `1c7da5134e00e261da9000c130e2a28b77cc56962ede3598af61b5d06df99edc` |
| `prismdrake-shellctl.json` | `69b13533c0143f92dabb5f7a7f4986badf4af85120ed582aaa8dacc04b7ac578` |
| `prismdrake-x11.json` | `56a96acc592bc98d2f30d15a856414ad7a41797d8201a950e239c1a69889dee6` |

## Test and install accounting

The checked-in PD2 inventory declares exactly 838 CTest registrations, ordered
names hash `df1a5190c607329895a077f41c105e0bd036e584310b3a9e43f251b0941689f9`,
and canonical inventory hash
`58c076af1a167d4e808dab7a79cf5fa018225ac9a24b6384b2ed409a287c126e`.
The ebuild excludes four named sandbox-incompatible registrations from its
834-test package selection; those four require separate execution. The one
historical non-root ownership skip remains evidence-bound and is not a general
permission to skip a future Rust or retained-native test.

The present install contract includes these five executables under the selected
GNU install bindir:

- `prismdrake-session`
- `prismdrake-settingsd`
- `prismdrake-notifyd`
- `prismdrake-shell`
- `prismdrake-shellctl`

It also installs the four D-Bus XML files, six runtime schemas selected by
CMake, default configuration, four theme-token documents, translations, build
provenance, and the X session desktop file. The capabilities and dependency
manifest schemas are source/build validation contracts rather than runtime
files in the current CMake install list. P0/P8 must account for every deliberate
change to this split.

## P0 disposition

- ADR 0010 is Accepted. Rust is the default toolkit-neutral language while Qt
  6.11 Quick/QML and justified Qt/native C++ boundaries remain.
- `prismdrake-vm` on `qemu:///system` is the designated Prismdrake build and
  test environment. Its recorded SSH/transfer state still requires preflight,
  and package mutation remains separately scoped to the active task.
- Full current-head GCC/Clang, sanitizer, QML-lint, 838-test, package, live
  accessibility, performance, and installed-lifecycle results have not been
  produced by this inventory.
- Product conversion begins only after the workspace plan's remaining P0
  exact-baseline and documentation gates close. Initial non-graphical Wyrmroot
  integration opens at approximate functional Rust parity with the current C++
  product; it does not wait for unrelated Glasswyrm migration, while graphical
  integration still waits for the Glasswyrm and Qt/QPA contracts it consumes.
