# Testing Prismdrake PD1

Contract validation and compiled tests are separate required layers. Run both
for production implementation changes:

```sh
make validate
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

Most of the current CTest suite is display-free. In addition to foundation,
settings, launcher, session, X11, and synthetic notification-model coverage,
it tests the Qt Core presentation bridge without a GUI application or display
connection. Those tests verify ordered immutable mirroring, defensive
revalidation, stable unaffected objects, reentrancy and thread rejection,
disabled affordances, replacement refresh, and typed action and dismissal
intents whose 64-bit identity never round-trips through JavaScript. A bounded
Qt Quick Test lane uses the offscreen platform and software renderer to verify
literal text, keyboard order, accessible metadata, non-color urgency text,
reduced motion, high contrast, opaque resolved token inputs, launcher state
presentation, right-to-left mirroring, bounded search input, and focus recovery
after result reorder or removal. GoogleTest and Qt are obtained only from the
system package manager and are never downloaded.
Focused launcher-controller tests verify worker-thread execution, replacement
cancellation, stale-generation rejection even when a backend ignores
cancellation, initial error/recovery presentation, typed launch-intent gating,
literal metacharacter handling, exact `%k` provenance, and rejection of forged
provenance without starting a process.

The checked-in Clang path is:

```sh
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

The final Gentoo package lifecycle has a separate strict evidence contract.
`GentooPortageLifecycleEvidenceTest` exercises only synthetic positive and
negative fixtures; it does not claim that Portage installation ran. Validate a
real private result after the complete lifecycle with:

```sh
python3 tests/gentoo/portage_lifecycle_evidence.py \
  /absolute/private/evidence/pd1-portage-lifecycle.json
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
cmake --build --preset clang-debug --target prismdrake-shell-notification-qml_qmllint
```

The authoritative formatting and QML-lint paths run in the Gentoo reference
environment with the supported Qt 6.11-or-newer toolkit. Set
`PRISMDRAKE_CLANG_FORMAT_EXECUTABLE` when the reviewed executable has a
versioned name. A missing formatter causes the explicit `format-check` target
to fail rather than claiming a skipped success. GitHub Actions remains a
repository-contract lane while its Ubuntu system Qt is below the supported
floor; it does not claim product compilation, formatting, or QML validation.

Clang-Tidy is opt-in and requires either `clang-tidy` on `PATH` or an explicit
`PRISMDRAKE_CLANG_TIDY_EXECUTABLE`. Required environment-dependent checks must
report an exact failure or skip reason; no required test may pass silently.
