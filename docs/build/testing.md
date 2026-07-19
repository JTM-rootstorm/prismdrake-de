# Testing Prismdrake

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

The Gentoo package lifecycles have separate strict preflight and final evidence
contracts. The `GentooInstalledArtifactAttestationTest`,
`GentooPortageLifecycleCollectorTest`, and
`GentooPortageLifecycleEvidenceTest` family retains the historical PD1
contract. `GentooPd2InstalledArtifactAttestationTest`,
`GentooPd2PortageLifecycleCollectorTest`, and
`GentooPd2PortageLifecycleEvidenceTest` exercise only synthetic positive and
negative PD2 fixtures; they do not claim that Portage installation ran. The
`GentooProductEbuildTest` additionally pins the exact-revision timestamp
contract used to make Qt resource metadata deterministic. The PD2 preflight
attestation is captured after the `USE=test` install and binds the exact
installed executables, Portage ownership, source driver, revision, ebuild, and
packaged data before an installed driver executes. The final lifecycle record
then binds every remaining phase. Validate a real private PD2 result with:

```sh
python3 tests/gentoo/pd2_portage_lifecycle_evidence.py \
  /absolute/private/evidence/pd2-portage-lifecycle.json
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
to fail rather than claiming a skipped success. GitHub Actions is not a
required validation lane. The canonical local commands and the Qt
6.11-or-newer Gentoo reference matrix provide milestone evidence; any optional
hosted automation must report only the checks and supported environment it
actually exercised.

Clang-Tidy is opt-in and requires either `clang-tidy` on `PATH` or an explicit
`PRISMDRAKE_CLANG_TIDY_EXECUTABLE`. Required environment-dependent checks must
report an exact failure or skip reason; no required test may pass silently.

## PD2 live X11 observers on Gentoo

The WP14 live integration gate uses `xwininfo` for bounded window-tree and
geometry observations and the real X11 protocol tracer from
`x11-misc/xtrace`. Gentoo renames that tracer executable to
`/usr/bin/x11trace` because `/usr/bin/xtrace` is an unrelated glibc dynamic
loader diagnostic. Live harness configuration must therefore name
`/usr/bin/x11trace` explicitly and must fail closed when it is absent; it must
not fall back to glibc's tool.

Run the PD2 installed demonstration only after capturing a mode-0600 PD2
preflight attestation that hashes `tests/integration/pd2_demo.py` as its exact
driver. The controlled-window fixture must be an absolute regular executable;
the output parent must already exist. The driver derives the source revision
from the attestation and writes only normalized mode-0600 evidence:

```sh
python3 tests/integration/pd2_demo.py \
  --attestation /absolute/private/evidence/pd2-installed-preflight.json \
  --controlled-window-fixture /absolute/path/prismdrake-controlled-window-fixture \
  --output /absolute/private/evidence/pd2-installed-demonstration.json
```

The command binds the fixed five `/usr/bin/prismdrake-*` executables before and
after the run, uses only reviewed absolute X11 observer paths, and reports fixed
failure categories without echoing supplied paths. Revalidate a retained record
without starting X11 by passing only `--validate-evidence`.

The complete environment, workflow, fallback replay, privacy boundary, and
limitations are documented in the
[PD2 installed multi-output demonstration guide](../development/pd2-installed-demonstration.md).

These observers are test-only. They are selected by `USE=test` for the product
ebuild and by `USE=x11` for `dev-util/prismdrake-dev-env`; neither belongs in
`DEPEND` or `RDEPEND`. The Gentoo tracer is currently `~amd64` in the reference
environment. Accept only `x11-misc/xtrace ~amd64` in the VM's package-local
`package.accept_keywords/prismdrake-dev` file. Do not enable `~amd64` globally.
