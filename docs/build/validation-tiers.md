# Validation tiers and selectors

Prismdrake separates fast local feedback from package and live-environment
work. The project-owned front end lists exact commands, runs only fixed
shell-free T0-T2 entries, and refuses implicit T3 or T4 escalation.

| Tier | Scope | Local selector behavior |
| --- | --- | --- |
| T0 | Contracts and other immediate static feedback | May run a named fixed command. |
| T1 | Unit, pure-model, parser, validator, and saved-evidence tests | May run a named fixed command. |
| T2 | Component, D-Bus, Qt/QML, X11, accessibility-adapter, and mixed-language contracts | May run a named fixed command against an already configured build. |
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

The optional tier assertion fails closed when it does not match the named
selector. Commands are fixed project entries rather than user-supplied command
fragments, and the runner never invokes a shell. A selector preserves the
documented direct Python, CMake, and CTest commands as the debugging primitive;
it does not hide configuration prerequisites or silently collect new evidence.

T3 and T4 names are boundary markers, not runnable commands. Their eventual
project-owned workflows require the plan's exact authorization, environment,
lease, artifact, privacy, and evidence checks. This front end has no override
that can turn a local test request into package or live work.
