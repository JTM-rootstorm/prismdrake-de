# Gentoo reference VM

The Gentoo VM is Prismdrake's PD1 reference environment for dependency
resolution, toolkit evidence, X11 integration, and local Portage QA. It is a
development and system-test environment, not a supported Prismdrake runtime or
a claim that the Proposed toolkit and build decisions are Accepted.

This document implements the environment-facing parts of `PD-DOC-001`,
`PD-DOC-008`, `PD-OBS-004`, and `PD-SEC-008`. The bootstrap helper changes only
guest Portage configuration and packages after an explicit `--apply`; it never
changes host libvirt, QEMU, networking, storage, or package state.

## Maintainer-provided host boundary

The current test domain is:

| Setting | Value |
|---|---|
| libvirt connection | `qemu:///system` |
| domain | `prismdrake-vm` |
| SSH endpoint | `root@127.0.0.1`, TCP port `10023` |
| host transfer directory | repository-local `vm-passthrough/` |
| intended virtiofs mount tag | `prismdrake-shared` |
| guest transfer mount | `/mnt/shared` |

No password, private key, token, or host-specific absolute home path belongs in
the repository. The VM uses the maintainer's existing SSH key configuration.
The loopback-only connection can be checked from the host with:

```sh
virsh --connect qemu:///system domstate prismdrake-vm
ssh -F /dev/null -p 10023 -o BatchMode=yes \
  -o StrictHostKeyChecking=accept-new \
  -o UserKnownHostsFile=/tmp/prismdrake-vm-known-hosts \
  root@127.0.0.1
```

The `-F /dev/null` option isolates this project workflow from host-wide SSH
client configuration. The known-hosts file contains only public host identity
material and remains outside the repository.

The repository checkout is not itself the virtiofs export. Create a disposable,
version-controlled nested clone from the host repository root when guest-side
scripts need the source tree:

```sh
git clone --no-hardlinks . vm-passthrough/prismdrake-de
```

`--no-hardlinks` keeps the nested clone's object storage independent of the host
checkout. The corresponding guest workspace is `/mnt/shared/prismdrake-de` and
retains normal Git status, history, executable bits, and symlink behavior. The
clone contains committed state only: the host's untracked `Plans/` directory
and all uncommitted changes are intentionally absent. Commit the intended test
checkpoint before cloning or refreshing it.

Refresh an existing nested clone from the host with a clean worktree and a
fast-forward-only merge:

```sh
git -C vm-passthrough/prismdrake-de status --short
git -C vm-passthrough/prismdrake-de fetch origin
git -C vm-passthrough/prismdrake-de merge --ff-only origin/main
```

Use the matching `origin/<branch>` when testing a branch other than `main`.
Stop if `status --short` reports guest changes or if the fast-forward is
refused; preserve intentional evidence before retrying. Keep
`vm-passthrough/` untracked and do not add repository or global exclusion rules
for it. Do not commit credentials, package caches, Portage temporary data, or
private logs from the shared clone.

On 2026-07-16, the active guest mount was healthy and read-write at
`/mnt/shared`, owned by UID and GID `1000:1000` with mode `0755`. The guest has
a dedicated locked `prismdrake` account at that UID/GID for unprivileged source
inspection and builds; SSH remains root-only. `findmnt`
reported the source label `glasswyrm-shared`, which differs from the intended
`prismdrake-shared` tag above. The helpers deliberately validate the mount path
and `virtiofs` filesystem type instead of relying on that stale label. Host
domain reconciliation remains a maintainer operation outside these scripts.

Do not use recursive mode `0777` to work around ownership. Prefer matching the
guest development UID and GID to the host owner, or use a narrowly controlled
shared group. The verification helper rejects a world-writable share.

## Observed guest baseline

The following facts were observed on 2026-07-16 and must be rechecked before a
new package run:

- Gentoo Base System release 2.18 on `x86_64`.
- Kernel `6.18.38-gentoo-m10`.
- Profile `default/linux/amd64/23.0/systemd`.
- Portage 3.0.81.2 with the Gentoo repository synchronized at
  2026-07-16 23:15 UTC (`8c90283afaae26f11d8ec5ba786f2b5f3c323af8`).
- GCC 15.3.0, CMake 4.3.3-r1, Ninja 1.13.2-r1, and Python 3.14.6.
- Six virtual CPUs, approximately 7.7 GiB memory, no swap, and 7.3 GiB free on
  the 30 GiB root filesystem.
- `VIDEO_CARDS="virgl"` and `MAKEOPTS="-j4 -l4"`.
- A configured official Gentoo binary package repository.
- A global USE policy that omits `X` and explicitly disables `qt6`.

The systemd profile describes this test guest only. Prismdrake's session design
remains init-neutral. The bootstrap helper does not rewrite `make.conf`, switch
profiles or init systems, globally enable unstable keywords, or execute
`emerge --depclean`.

Disk capacity is the immediate constraint. Review the complete pretend output,
binary-package compatibility, downloads, build time, masks, licenses, keywords,
and USE changes before approving a Qt build.

The execution plan recommends 8 GiB memory and 30 GiB free before bootstrap.
This guest is slightly below the memory recommendation and substantially below
the free-space recommendation. The helpers enforce a 4 GiB/5 GiB hard safety
floor and report the observed values, but that floor is not an endorsement of
the current capacity. Stop before installation unless the reviewed Portage
graph, binary-package choices, downloads, and installed-size estimate fit with
working headroom. Expanding host-managed storage requires separate maintainer
authorization.

## Stage and inspect

After cloning or refreshing a committed checkpoint, connect to the VM and run
the read-only check:

```sh
cd /mnt/shared/prismdrake-de
tools/gentoo/verify-vm.sh \
  --workspace /mnt/shared/prismdrake-de \
  --shared-path /mnt/shared
```

Before bootstrap, failures for an unregistered `prismdrake-local`, missing
`pkgcheck`, and absent test tools are expected detected differences. The script
does not write guest configuration, install packages, or modify the host.

Preview the bootstrap next:

```sh
tools/gentoo/bootstrap-vm.sh \
  --workspace /mnt/shared/prismdrake-de \
  --shared-path /mnt/shared
```

The default mode prints the planned files and reruns read-only verification.
After reviewing the plan, apply guest-local configuration and package changes:

```sh
tools/gentoo/bootstrap-vm.sh --apply \
  --workspace /mnt/shared/prismdrake-de \
  --shared-path /mnt/shared
```

Add `--sync` only when the canonical Gentoo repository should be synchronized
before resolution. The apply path:

1. verifies Gentoo, architecture, memory, disk, virtiofs, ownership mode, and
   local repository metadata;
2. requires the shared checkout owner to map to an ordinary guest account and
   runs repository QA as that account;
3. pretends and asks before installing the canonical Gentoo Layer A QA tools;
4. backs up only project-owned Portage files that it needs to change;
5. writes `prismdrake-local.conf`, `package.use/prismdrake-dev`, and the narrow
   `package.accept_keywords/prismdrake-dev` exception atomically;
6. runs `pkgdev manifest` and `pkgcheck scan` before local package resolution;
7. pretends the default, `-qt6`, `clang`, `implementation-deps`, and
   `visual-tests` development metapackage combinations;
8. stops if Portage reports a mask, keyword, license, or USE conflict; and
9. uses `emerge --ask` before installing the development metapackage.

Both helpers accept `PRISMDRAKE_WORKSPACE` and `PRISMDRAKE_SHARED_PATH` as
environment alternatives to command-line overrides. The default workspace is
derived from the script's checkout, so it never assumes a user's home path.

## Snapshot and artifact boundary

A VM snapshot includes guest `/etc/portage`, installed packages, and guest-local
build directories. It does not include the host-backed `/mnt/shared` contents.
Before reverting a snapshot:

- stop processes that use the shared source;
- preserve or commit source changes on the host;
- copy required logs and reports back through the reviewed transfer directory;
- record the package and USE state associated with the snapshot; and
- remember that reverting the guest cannot undo shared-file changes.

Prefer guest-local compiler output such as `/var/tmp/prismdrake-build`. Do not
put distfiles, binary package caches, or Portage temporary build data in the
shared tree.

The local repository workflow and package-specific USE policy are documented
in [Gentoo local repository](../packaging/gentoo-local-repository.md).
