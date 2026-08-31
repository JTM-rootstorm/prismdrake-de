# Rust toolchain and workspace proof

Prismdrake's Rust workspace is rooted at [`Cargo.toml`](../../Cargo.toml) and
uses resolver version 3, Rust edition 2024, a tracked `Cargo.lock`, and
`GPL-3.0-or-later` for new first-party code. The workspace `rust-version` is
1.96. This is the declared initial floor, not a broadly verified lower bound.

The first crate, `prismdrake-foundation`, is a dependency-free port of the
current toolkit-neutral generation, error/exit-status, and cancellation
contracts. It forbids unsafe code and is not published. No crates.io, Git,
vendored, build-script, procedural-macro, native, or transitive dependency is
present in this checkpoint, so `Cargo.lock` contains only the first-party
package.

## Current reviewed host toolchain

The validation selectors are currently pinned to the Gentoo system binary
toolchain re-observed on 2026-08-30:

| Tool | Identity | Resolved executable SHA-256 |
| --- | --- | --- |
| `rustc` | 1.97.1, commit `8bab26f4f68e0e26f0bb7960be334d5b520ea452`, 2026-07-14, `x86_64-unknown-linux-gnu`, LLVM 22.1.6 | `d3a664c970a9fd8361b64194861bebc1ae37b9054e5ee3400dc1c9e691797eea` |
| `cargo` | 1.97.1, commit `c980f4866141969fab6254a680546a277789d6f0`, 2026-06-30 | `828980723df339d62434390e9fb8ef8831036583343ae2316b7ab5646b5c1953` |
| `rustfmt` | 1.9.0-stable, commit `8bab26f4f6`, 2026-07-14 | `30de9e1efcd8f8fe7750e00d0c45ff8f4c480608ef1be5baf9ab6f1b4556e8f8` |
| `clippy` | 0.1.97, commit `8bab26f4f6`, 2026-07-14 | `a0a3808dddb32f525601e608740ee7c1c5df86db4a7183ea945449f60d29509d` (`cargo-clippy`) |

The executables resolve beneath `/opt/rust-bin-1.97.1/bin/`; the workspace
Wyrmroot compiler fork at `../rust/` is not a Prismdrake input. These observed
host hashes identify this probe only and are not portable package requirements.
The earlier 2026-08-23 Rust/Cargo 1.96.1 identities remain historical evidence
for revision `78d33653738c42e3851f5f6b7027b56a92fd313a`; they are no longer the
current executable pin. The workspace `rust-version = "1.96"` floor is unchanged.

## Validation boundary

The reviewed P2 validation selectors bind these exact Cargo operations:

```sh
python3 tools/validation_tiers.py --selector rust-format --tier T0 --run
python3 tools/validation_tiers.py --selector rust-check --tier T0 --run
python3 tools/validation_tiers.py --selector rust-clippy --tier T0 --run
python3 tools/validation_tiers.py --selector rust-test --tier T1 --run
```

They execute `cargo fmt --all --check`, `cargo check --workspace --all-targets
--locked --offline`, `cargo clippy --workspace --all-targets --locked --offline
-- -D warnings`, and `cargo test --workspace --all-targets --locked --offline`,
respectively. The selector front end verifies hash-pinned system tool identities
outside the writable repository, uses only a fixed trusted tool directory for
Cargo subcommand dispatch, binds an absolute reviewed linker, constructs a
credential-free environment, and confines all mutable homes, temporary files,
caches, Cargo state, and target output below ignored
`build/validation-state/<selector>/` paths. Child streams are noninteractive and
discarded, execution is shell-free and time-bounded, and Cargo is forced offline.

The selectors are valid only for the exact reviewed dependency-free workspace
file set. A source or manifest identity change, extra workspace file, ambient or
isolated Cargo configuration or registry state, dependency, build script, proc
macro, native link surface, or unsafe source surface fails closed pending review.
These selectors do not invoke or authorize a package, VM, hardware, live-service,
or network lane.

The next dependency-bearing change must add the complete direct and material
transitive review required by ADR 0010 and the active plan before its lockfile
is accepted. This zero-dependency proof does not establish the Gentoo package
checkpoint or authorize any Portage operation.

## Source-archive proof

The exact revision `78d33653738c42e3851f5f6b7027b56a92fd313a` was exported
with `git archive` on 2026-08-23. The resulting tar archive had SHA-256
`575cd544159713d4a0d16ffa76d13f8e51ecfd3b589e8bfb73a8bc1acef20900`.
After extraction beneath ignored project-local build state, the source tree
contained no `.git` metadata. `cargo check --workspace --all-targets --locked
--offline` and `cargo test --workspace --all-targets --locked --offline` both
passed there with all four foundation tests green.

This proves only that the dependency-free Cargo workspace builds and tests from
the archived source bytes. The archive is not a release artifact, and this
local proof does not replace a supported Gentoo install/uninstall lifecycle.

## Gentoo ebuild ownership

The live product ebuild requires Gentoo Rust 1.96 or newer and uses
`cargo.eclass` to generate an isolated offline Cargo configuration. Because the
current lockfile has no external packages, the ebuild creates an empty vendor
source and never invokes live Cargo dependency acquisition. Before every Cargo
phase it removes ambient compiler wrappers, target runners/linkers, known
Cargo/registry/transport credentials, proxies, agents, and extra eclass
arguments; then it rebinds the eclass-selected Cargo, rustc, rustdoc, Cargo
home, target output, exact workspace manifest, and offline mode. Pre-inherit
crate, path-override, bootstrap, and optional-eclass inputs are explicitly empty.
Portage's compiler flags and sandbox variables remain intact. Cargo owns one
locked, offline workspace
build. When Portage executes `src_test` for the package's authorized test
configuration, Cargo also owns one all-target test invocation. CMake continues
to own the installed C++/Qt product and documentation; no Rust artifact is
installed until a production Rust service or binary has an accepted install
contract. This prevents a recursive or duplicate Cargo build and avoids
installing a library-only migration probe as public product surface.

The ebuild contract is host-testable, but the Portage package checkpoint remains
open until the exact package lifecycle is explicitly authorized and exercised
in the designated Gentoo environment.
