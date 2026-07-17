# Testing Prismdrake PD1

Contract validation and compiled tests are separate required layers. Run both
for production implementation changes:

```sh
make validate
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

The current CTest suite is display-free. It verifies that the generated build
metadata uses the tracked product version, that developer overrides report
their explicit compile-time state, and that the Gentoo VM safety helpers obey
their non-root mutation policy through hermetic Python tests. GoogleTest is
obtained only from the system package manager and is never linked into an
installed runtime target.

The checked-in Clang path is:

```sh
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

To exercise sanitizers without changing ordinary build artifacts:

```sh
cmake -S . -B build/sanitizers -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPRISMDRAKE_ENABLE_ASAN=ON \
  -DPRISMDRAKE_ENABLE_UBSAN=ON
cmake --build build/sanitizers
ctest --test-dir build/sanitizers --output-on-failure
```

Formatting is checked without rewriting source:

```sh
cmake --build --preset clang-debug --target format-check
```

The authoritative CI formatting path uses `clang-format-18` on Ubuntu 24.04.
Set `PRISMDRAKE_CLANG_FORMAT_EXECUTABLE` when the reviewed executable has a
versioned name. A missing formatter causes the explicit `format-check` target
to fail rather than claiming a skipped success.

Clang-Tidy is opt-in and requires either `clang-tidy` on `PATH` or an explicit
`PRISMDRAKE_CLANG_TIDY_EXECUTABLE`. Required environment-dependent checks must
report an exact failure or skip reason; no required test may pass silently.
