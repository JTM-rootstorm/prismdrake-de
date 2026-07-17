# Building Prismdrake PD1

Prismdrake PD1 uses CMake 3.24 or newer, C++20 without compiler extensions,
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

The PD1 production build contains internal foundation, configuration, theme,
settings, launcher, notification, X11, shell-presentation, and session targets.
The Experimental shell presentation now includes immutable Qt projections of
one complete settings/theme generation, authoritative task snapshots, matching
launcher catalog/search snapshots, and synthetic notification snapshots. It
also contains compiled panel and notification QML modules plus a Qt/X11 panel
window host that publishes standard dock and strut state through the existing
X11 adapter. The adapters emit typed presentation intents; none mutates its
authoritative model or implements window-manager or notification-service
policy. These targets are not installed libraries or stable C++ ABIs. The
isolated toolkit experiment remains a separate CMake project and is not linked
into production targets.

Shell presentation requires system Qt Core, GUI, QML, Quick, and Quick Controls
6.4 or newer through the Qt CMake packages; tests also require Quick Test.
Ubuntu 24.04 CI verifies Qt 6.4.2 as the oldest tested component version;
current host and Gentoo component evidence use Qt 6.11.1. The complete shell
executable, live settings-snapshot client, and runtime wiring among the panel,
launcher, task model, notification fixture, and session remain later PD1
integration. See the [panel-shell evidence](../research/pd1-panel-shell-evidence.md).

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
Clang 22.1.8 against the production foundation target. The configured CMake
constraint is 3.24, but that exact lower bound and minimum compiler versions
will not be claimed as verified until lower-bound testing supplies measured
evidence. See the [toolchain evidence report](../research/pd1-build-toolchain-evidence.md).
