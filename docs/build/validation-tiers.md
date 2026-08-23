# Validation tiers and selectors

Prismdrake separates fast local feedback from package and live-environment
work. The project-owned front end lists exact commands, runs only reviewed
shell-free T0-T1 Python and dependency-free Rust entries, and refuses T2, T3,
or T4 execution until the corresponding trust and authorization boundary
exists.

| Tier | Scope | Local selector behavior |
| --- | --- | --- |
| T0 | Contracts and other immediate static feedback | May run a named fixed command. |
| T1 | Unit, pure-model, parser, validator, and saved-evidence tests | May run a named fixed command. |
| T2 | Component, D-Bus, Qt/QML, X11, accessibility-adapter, and mixed-language contracts | Listed as blocked until a committed tier manifest and exact build identity are established. |
| T3 | Package, install, and installed-runtime checkpoints | Listed as blocked; never run by this front end. |
| T4 | Explicitly authorized hardware or final live-environment evidence | Listed as blocked; never run by this front end. |

List the complete registry or one tier:

```sh
python3 tools/validation_tiers.py --list
python3 tools/validation_tiers.py --list --tier T1
python3 tools/validation_tiers.py --list --json
```

Inspect a selector without running it, then run a local selector explicitly:

```sh
python3 tools/validation_tiers.py --selector contracts
python3 tools/validation_tiers.py --selector contracts --tier T0 --run
```

The reviewed Rust entries are:

```sh
python3 tools/validation_tiers.py --selector rust-format --tier T0 --run
python3 tools/validation_tiers.py --selector rust-check --tier T0 --run
python3 tools/validation_tiers.py --selector rust-clippy --tier T0 --run
python3 tools/validation_tiers.py --selector rust-test --tier T1 --run
```

The optional tier assertion fails closed when it does not match the named
selector. Python commands use the resolved absolute active Python executable
with isolated interpreter flags and fixed project-owned arguments. Rust commands
use hash-pinned Cargo, Rust, Rustdoc, Rustfmt, Clippy, and linker executables
outside the writable repository. The Rust `PATH` contains only the reviewed
tool directory needed for Cargo subcommand dispatch; Python entries receive no
`PATH`. The runner never invokes a shell.

Every runnable selector receives a minimal constructed environment with no
inherited Python or Rust injection, loader, credential, agent, proxy, service,
or display variables. Its private home, temporary, cache, configuration, data,
state, and runtime paths are non-symlink directories below ignored
`build/validation-state/`. Each Rust selector also receives a separate private
Cargo home and target directory there. Cargo runs offline; check, Clippy, and
test also require the committed lockfile. The boundary refuses execution if the
reviewed dependency-free workspace file set or hashes change, if ambient or
isolated Cargo configuration or registry state appears, or if a dependency,
build script, proc macro, native link surface, or unsafe source surface appears.
Child input and output streams are discarded; each child has a fixed five-minute
timeout, and the front end prints only the selector identity and a bounded final
status.

Stored evidence uses the separate
[validation-record envelope](evidence-records.md). Revalidating that envelope
does not execute the recorded command or recollect evidence.

T2, T3, and T4 names are boundary markers, not runnable commands. T2 requires a
committed tier manifest plus source, configuration, and build identity before a
CMake or CTest entry can be enabled. T3 and T4 workflows additionally require
the plan's exact authorization, environment, lease, artifact, privacy, and
evidence checks. This front end has no override that can turn a local request
into compiled, package, installed-runtime, hardware, or live-environment work.
