# Building Prismdrake

Prismdrake uses CMake 3.24 or newer, C++20 without compiler extensions,
and system-packaged dependencies. Configure and build operations do not fetch
or vendor third-party source. GCC and Clang are the supported Linux compiler
families.

The configuration target requires the system toml++ package discovered through
its CMake package configuration. On Gentoo this is `dev-cpp/tomlplusplus`; the
build never downloads or vendors a parser.

The theme target requires system nlohmann JSON 3.11 or newer through its CMake
package configuration. On Gentoo this is `dev-cpp/nlohmann_json`; on Ubuntu it
is `nlohmann-json3-dev`. The dependency is header-only and is compiled into the
internal theme target. The configured 3.11 constraint is declared but is not a
verified lower bound.

The integrated Experimental build contains internal foundation, configuration,
theme, settings, launcher, notification, X11, shell-presentation, desktop,
quick-settings, control, and session targets. The shell consumes complete
settings/theme, task, application-catalog, pin, notification, and output
snapshots. Its Qt/X11 hosts publish standard desktop and dock roles while the
event-driven task controller sends only checked standard WM requests tied to a
current task lifetime and generation. Presentation adapters emit typed intents;
none becomes authoritative for window, notification, settings, or session
state. Filesystem discovery, search, application launch, wallpaper decoding,
D-Bus waits, and X11 observations remain bounded and do not become QML business
logic. These internal libraries are not installed or exposed as stable C++
ABIs. The five Experimental executables and their read-only runtime data have
one CMake install contract. The isolated toolkit experiment remains a separate
CMake project and is not linked into installed targets.

The shell settings client integrates the selected sd-bus provider with Qt's
socket and monotonic-timeout event sources. It treats generation signals as
refetch hints, clears state on every well-known-name ownership gap, and parses
only complete canonical version-2 snapshots into immutable typed C++ state.
The inverse parser uses the system nlohmann JSON headers, applies explicit
depth, node, container, string, and 1 MiB limits, rejects duplicate or unknown
shape, and verifies an exact canonical serializer round trip before publication.

Shell presentation requires system Qt Core, GUI, QML, Quick, and Quick Controls
6.11 or newer through the Qt CMake packages; tests also require Quick Test.
The Gentoo reference environment verifies Qt 6.11.1 and qtdeclarative
6.11.1-r1. Ubuntu 24.04's Qt 6.4 packages are below the supported floor and do
not provide product build or compatibility evidence. The Experimental shell
composition root wires the settings, output, launcher, panel, task,
notification-center, desktop, quick-settings, Shell1, and Session1 clients and
surfaces. Installed mode uses the separate toolkit-neutral
`prismdrake-notifyd` authority; the retired PD1 synthetic notification owner is
not part of the production runtime, while synthetic models remain test-only.
See the
[PD2 implementation status](../architecture/pd2-core-shell.md).

## Installation contract

Configure the final prefix before building; distribution staging must use
`DESTDIR` rather than changing the prefix during installation. A runtime-only
staging build is:

```sh
cmake -S . -B build-package \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_TESTING=OFF \
  -DPRISMDRAKE_USE_INSTALL_PATHS=ON
cmake --build build-package --parallel
DESTDIR="$PWD/package-root" cmake --install build-package
```

The install contract contains `prismdrake-session`, `prismdrake-settingsd`,
`prismdrake-notifyd`, `prismdrake-shell`, and `prismdrake-shellctl`; packaged
defaults, theme tokens, schemas, Experimental Prismdrake interfaces,
translations, reviewed architecture documents, and one validated X11 session
entry under `share/xsessions`. The session entry starts the init-neutral
supervisor. The supervisor itself publishes the canonical Prismdrake desktop
identity to its children and requires an inherited X display, private runtime
directory, and session bus. Installation never creates or modifies user
configuration.

## Canonical developer builds

Use the checked-in presets for ordinary development:

```sh
cmake --preset gcc-debug
cmake --build --preset gcc-debug
```

For Clang, replace `gcc-debug` with `clang-debug`. Both presets create output
below the ignored `build/` directory. In the Gentoo reference VM, use a
guest-local directory such as `/var/tmp/prismdrake-build` when invoking CMake
manually; compiler output should not be placed on the virtiofs share.

`VERSION` is the sole tracked product-version source. It is independent from
the project specification, schemas, D-Bus interfaces, and Git metadata. The
build therefore remains valid when produced from an archive without `.git`.

## Build options

All optional behavior is disabled unless selected explicitly:

| Option | Default | Effect |
|---|---:|---|
| `BUILD_TESTING` | `ON` | Build display-free tests using system GoogleTest |
| `PRISMDRAKE_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer on project-owned targets |
| `PRISMDRAKE_ENABLE_UBSAN` | `OFF` | Enable UndefinedBehaviorSanitizer on project-owned targets |
| `PRISMDRAKE_ENABLE_LTO` | `OFF` | Enable checked interprocedural optimization |
| `PRISMDRAKE_ENABLE_CLANG_TIDY` | `OFF` | Run Clang-Tidy on project-owned targets |
| `PRISMDRAKE_WARNINGS_AS_ERRORS` | `OFF` | Promote project-owned warnings to errors |
| `PRISMDRAKE_ENABLE_DEVELOPER_OVERRIDES` | `OFF` | Compile non-production diagnostics and mock-capability override support; callers must also request developer policy |
| `PRISMDRAKE_USE_INSTALL_PATHS` | `OFF` | Embed configured read-only install locations instead of source-tree development data; distribution packages must enable this |

Warnings, sanitizers, LTO, and Clang-Tidy apply only to project-owned targets.
Warnings-as-errors is enabled in controlled development and CI configurations,
not forced on distribution packagers. Developer overrides must remain disabled
in production builds.

Missing required Qt modules, toml++, or nlohmann JSON is always a configure
error with package guidance from the corresponding system package discovery.
When tests are enabled, missing system GoogleTest or Qt Quick Test is also a
configure error. Use `-DBUILD_TESTING=OFF` for a runtime-only build;
Prismdrake never downloads dependencies during configuration.

The Gentoo evidence environment has tested CMake 4.3.3-r1, GCC 15.3.0, and
Clang 22.1.8 against the internal foundation target. The configured CMake
constraint is 3.24, but that exact lower bound and minimum compiler versions
will not be claimed as verified until lower-bound testing supplies measured
evidence. See the [toolchain evidence report](../research/pd1-build-toolchain-evidence.md).
