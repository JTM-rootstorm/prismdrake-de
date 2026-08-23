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

## Observed host toolchain

The 2026-08-23 local P2 probe observed the Gentoo system binary toolchain:

| Tool | Identity | Resolved executable SHA-256 |
| --- | --- | --- |
| `rustc` | 1.96.1, commit `31fca3adb283cc9dfd56b49cdee9a96eb9c96ffd`, 2026-06-26, `x86_64-unknown-linux-gnu`, LLVM 22.1.2 | `4a84e05991ad6f2a84c1361c29b52b38c390365bd1fc269b1936c88d482a8928` |
| `cargo` | 1.96.1, commit `356927216`, 2026-06-26 | `a73b2c25573d251489101c0d8f19ad3702eb9761166de5ed8437b472b6c038ce` |

The executables resolve beneath `/opt/rust-bin-1.96.1/bin/`; the workspace
Wyrmroot compiler fork at `../rust/` is not a Prismdrake input. These observed
host hashes identify this probe only and are not portable package requirements.

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
