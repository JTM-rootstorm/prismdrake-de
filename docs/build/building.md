# Building Prismdrake PD1

Prismdrake PD1 uses CMake 3.24 or newer, C++20 without compiler extensions,
and system-packaged dependencies. Configure and build operations do not fetch
or vendor third-party source. GCC and Clang are the supported Linux compiler
families.

The initial production build contains the internal `prismdrake-foundation`
target. It exposes build diagnostics to other in-tree targets but is not an
installed library or a stable C++ ABI. Qt, X11, and the isolated toolkit
experiment are not part of this initial production target.

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
| `PRISMDRAKE_ENABLE_DEVELOPER_OVERRIDES` | `OFF` | Compile non-production developer override support into diagnostic metadata |

Warnings, sanitizers, LTO, and Clang-Tidy apply only to project-owned targets.
Warnings-as-errors is enabled in controlled development and CI configurations,
not forced on distribution packagers. Developer overrides must remain disabled
in production builds.

When tests are enabled, a missing system GoogleTest installation is a configure
error with package guidance. Use `-DBUILD_TESTING=OFF` for a runtime-only build;
Prismdrake never downloads GoogleTest during configuration.

The Gentoo evidence environment has tested CMake 4.3.3-r1, GCC 15.3.0, and
Clang 22.1.8 against the production foundation target. The configured CMake
constraint is 3.24, but that exact lower bound and minimum compiler versions
will not be claimed as verified until lower-bound testing supplies measured
evidence. See the [toolchain evidence report](../research/pd1-build-toolchain-evidence.md).
