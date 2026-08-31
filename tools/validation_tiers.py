#!/usr/bin/env python3
"""List and run Prismdrake's explicitly bounded validation selectors."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import signal
import stat
import subprocess
import sys
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Mapping, Sequence


ROOT = Path(__file__).resolve().parent.parent
STATE_ROOT = ROOT / "build" / "validation-state"
PYTHON_EXECUTABLE_INPUT = sys.executable
PYTHON_EXECUTABLE = str(Path(sys.executable).resolve(strict=False))
RUNNABLE_TIERS = frozenset({"T0", "T1"})
SELECTOR_TIMEOUT_SECONDS = 300
RUST_TOOL_DIRECTORY = Path("/opt/rust-bin-1.97.1/bin")
RUST_LINKER = Path("/usr/x86_64-pc-linux-gnu/gcc-bin/15/x86_64-pc-linux-gnu-gcc")
CARGO_EXECUTABLE = str(RUST_TOOL_DIRECTORY / "cargo")
RUST_TOOL_SPECS: Mapping[str, tuple[Path, str]] = {
    "cargo": (
        RUST_TOOL_DIRECTORY / "cargo",
        "828980723df339d62434390e9fb8ef8831036583343ae2316b7ab5646b5c1953",
    ),
    "rustc": (
        RUST_TOOL_DIRECTORY / "rustc",
        "d3a664c970a9fd8361b64194861bebc1ae37b9054e5ee3400dc1c9e691797eea",
    ),
    "rustdoc": (
        RUST_TOOL_DIRECTORY / "rustdoc",
        "3b0c7aef20e0dfdfe2dd19ddb0b40d3b744f76318a44031813f501e1912934df",
    ),
    "rustfmt": (
        RUST_TOOL_DIRECTORY / "rustfmt",
        "30de9e1efcd8f8fe7750e00d0c45ff8f4c480608ef1be5baf9ab6f1b4556e8f8",
    ),
    "cargo-fmt": (
        RUST_TOOL_DIRECTORY / "cargo-fmt",
        "07ce638a5be73e97defbf0864f32a64ed40a0b7682b85625e68790f9e68d19e9",
    ),
    "cargo-clippy": (
        RUST_TOOL_DIRECTORY / "cargo-clippy",
        "a0a3808dddb32f525601e608740ee7c1c5df86db4a7183ea945449f60d29509d",
    ),
    "clippy-driver": (
        RUST_TOOL_DIRECTORY / "clippy-driver",
        "e8fa6ad666c353e4c9248fc67074cf160ff2a5dbd97debd20416b81d7f917373",
    ),
    "linker": (
        RUST_LINKER,
        "80a2605a74272c0ea8c5cf15778167e44c91fa638ea25f4fc7ad0ac2f325330e",
    ),
}
RUST_WORKSPACE_FILES: Mapping[str, str] = {
    "Cargo.toml": "0c20bc18303678f2f20062e2dba2c82ab36043bad9e1b92546885df18608c6a1",
    "Cargo.lock": "8a68fa0a5d311ca4ead40537d9fbd14b1ecb76b180b101d4719b4ec6a9c84c25",
    "crates/prismdrake-foundation/Cargo.toml": (
        "b0f59163857f60fb7f5d541c8c53f1ca011117a895f051b9c833585e978c72f7"
    ),
    "crates/prismdrake-foundation/src/lib.rs": (
        "1b55932d228c0b3b0fabd816a186cb47bab808c17788680911a0f6f6f403a118"
    ),
    "crates/prismdrake-foundation/src/settings.rs": (
        "3fccd2e51a237922d4ff03ffd4958120a018e3cbab090d91964a436a5ba70612"
    ),
}
RUST_SELECTOR_NAMES = frozenset({"rust-format", "rust-check", "rust-clippy", "rust-test"})
FORBIDDEN_RUST_SOURCE = (
    re.compile(r"\bunsafe\b"),
    re.compile(r"\bextern\s*\""),
    re.compile(r"#\s*\[\s*link"),
    re.compile(r"\b(?:asm|global_asm|include|include_bytes|include_str)!"),
    re.compile(r"\bproc_macro\b"),
)


class BoundaryError(RuntimeError):
    """A local selector failed a trust or state-confinement check."""


@dataclass(frozen=True)
class Selector:
    """One fixed validation entry point and its escalation boundary."""

    name: str
    tier: str
    description: str
    command: tuple[str, ...] | None
    runnable_locally: bool
    blocked_reason: str | None = None


SELECTORS = (
    Selector(
        "contracts",
        "T0",
        "Repository documents, schemas, manifests, and local links",
        (PYTHON_EXECUTABLE, "-I", "-B", "tools/validate.py"),
        True,
    ),
    Selector(
        "rust-format",
        "T0",
        "Rust workspace formatting check against the pinned dependency-free boundary",
        (CARGO_EXECUTABLE, "fmt", "--all", "--check"),
        True,
    ),
    Selector(
        "rust-check",
        "T0",
        "Rust dependency-free workspace check with locked offline resolution",
        (
            CARGO_EXECUTABLE,
            "check",
            "--workspace",
            "--all-targets",
            "--locked",
            "--offline",
        ),
        True,
    ),
    Selector(
        "rust-clippy",
        "T0",
        "Rust dependency-free workspace Clippy gate with warnings denied",
        (
            CARGO_EXECUTABLE,
            "clippy",
            "--workspace",
            "--all-targets",
            "--locked",
            "--offline",
            "--",
            "-D",
            "warnings",
        ),
        True,
    ),
    Selector(
        "validation-selector-tests",
        "T1",
        "Unit tests for this validation-tier boundary",
        (
            PYTHON_EXECUTABLE,
            "-I",
            "-B",
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/validation",
            "-p",
            "test_*.py",
        ),
        True,
    ),
    Selector(
        "rust-test",
        "T1",
        "Rust dependency-free workspace tests with locked offline resolution",
        (
            CARGO_EXECUTABLE,
            "test",
            "--workspace",
            "--all-targets",
            "--locked",
            "--offline",
        ),
        True,
    ),
    Selector(
        "gentoo-saved-evidence",
        "T1",
        "Saved Gentoo evidence validators; does not recollect evidence",
        (
            PYTHON_EXECUTABLE,
            "-I",
            "-B",
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/gentoo",
            "-p",
            "test_*evidence.py",
        ),
        True,
    ),
    Selector(
        "performance-saved-evidence",
        "T1",
        "Saved performance evidence validators; does not recollect evidence",
        (
            PYTHON_EXECUTABLE,
            "-I",
            "-B",
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/performance",
            "-p",
            "test_*evidence*.py",
        ),
        True,
    ),
    Selector(
        "compiled-gcc",
        "T2",
        "Configured GCC component, D-Bus, Qt/QML, and X11 CTest suite",
        None,
        False,
        "a committed tier manifest and exact build identity are not yet established",
    ),
    Selector(
        "qml-lint",
        "T2",
        "Configured aggregate Qt QML lint target",
        None,
        False,
        "a committed tier manifest and exact build identity are not yet established",
    ),
    Selector(
        "package-checkpoint",
        "T3",
        "Package, install, and installed-runtime workflow",
        None,
        False,
        "package and installed-runtime work requires a separately authorized workflow",
    ),
    Selector(
        "live-environment",
        "T4",
        "Explicitly authorized hardware or final live-environment workflow",
        None,
        False,
        "hardware and live-environment work requires a separately authorized workflow",
    ),
)

SELECTOR_ENVIRONMENT_ALLOWLIST: Mapping[str, Mapping[str, str]] = {
    entry.name: {} for entry in SELECTORS if entry.tier in RUNNABLE_TIERS
}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "List or run a fixed Prismdrake validation selector. T2-T4 are "
            "never run by this local front end."
        )
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true", help="list selectors")
    mode.add_argument("--selector", help="select one exact entry by name")
    parser.add_argument("--tier", choices=("T0", "T1", "T2", "T3", "T4"))
    parser.add_argument("--run", action="store_true", help="run the selected local entry")
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    return parser


def _selected(name: str | None, tier: str | None) -> list[Selector]:
    entries = list(SELECTORS)
    if name is not None:
        entries = [entry for entry in entries if entry.name == name]
    if tier is not None:
        entries = [entry for entry in entries if entry.tier == tier]
    return entries


def _record(entry: Selector) -> dict[str, object]:
    record = asdict(entry)
    if entry.command is not None:
        record["command"] = list(entry.command)
    return record


def _print_entries(entries: Sequence[Selector], as_json: bool) -> None:
    if as_json:
        print(json.dumps([_record(entry) for entry in entries], indent=2, sort_keys=True))
        return

    for entry in entries:
        command = " ".join(entry.command) if entry.command is not None else "BLOCKED"
        print(f"{entry.tier} {entry.name}: {command}", flush=True)
        print(f"  {entry.description}", flush=True)
        if entry.blocked_reason is not None:
            print(f"  blocked: {entry.blocked_reason}", flush=True)


def _trusted_python_executable() -> Path:
    candidate = Path(PYTHON_EXECUTABLE_INPUT)
    if not candidate.is_absolute():
        raise BoundaryError("the active Python executable is not an absolute path")

    try:
        resolved = candidate.resolve(strict=True)
        metadata = resolved.stat()
    except OSError as error:
        raise BoundaryError("the active Python executable cannot be resolved") from error

    if not stat.S_ISREG(metadata.st_mode) or not os.access(resolved, os.X_OK):
        raise BoundaryError("the active Python executable is not a regular executable file")
    if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise BoundaryError("the active Python executable is group- or world-writable")
    if _is_beneath(resolved, ROOT):
        raise BoundaryError("the active Python executable is inside the writable repository")
    return resolved


def _sha256_file(path: Path) -> str:
    try:
        with path.open("rb") as source:
            return hashlib.file_digest(source, "sha256").hexdigest()
    except OSError as error:
        raise BoundaryError("a trusted executable cannot be hashed") from error


def _trusted_executable(
    candidate: Path,
    expected_sha256: str,
    *,
    expected_basename: str | None = None,
    writable_root: Path = ROOT,
) -> Path:
    if not candidate.is_absolute():
        raise BoundaryError("a reviewed executable path is not absolute")
    if expected_basename is not None and candidate.name != expected_basename:
        raise BoundaryError("a reviewed executable has an incompatible basename")
    if candidate.is_symlink():
        raise BoundaryError("a reviewed executable path is a symbolic link")

    try:
        resolved = candidate.resolve(strict=True)
        metadata = resolved.stat()
    except OSError as error:
        raise BoundaryError("a reviewed executable cannot be resolved") from error
    if _is_beneath(resolved, writable_root):
        raise BoundaryError("a reviewed executable is inside the writable repository")
    if not stat.S_ISREG(metadata.st_mode) or not os.access(resolved, os.X_OK):
        raise BoundaryError("a reviewed executable is not a regular executable file")
    if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise BoundaryError("a reviewed executable is group- or world-writable")
    if metadata.st_uid == os.getuid() or os.access(resolved, os.W_OK):
        raise BoundaryError("a reviewed executable is writable by the invoking user")

    for parent in resolved.parents:
        try:
            parent_metadata = parent.stat()
        except OSError as error:
            raise BoundaryError("a reviewed executable parent cannot be inspected") from error
        if not stat.S_ISDIR(parent_metadata.st_mode):
            raise BoundaryError("a reviewed executable parent is not a directory")
        if parent_metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise BoundaryError("a reviewed executable parent is group- or world-writable")
        if parent_metadata.st_uid == os.getuid() or os.access(parent, os.W_OK):
            raise BoundaryError(
                "a reviewed executable parent is writable by the invoking user"
            )
        if parent == parent.parent:
            break

    if _sha256_file(resolved) != expected_sha256:
        raise BoundaryError("a reviewed executable hash does not match")
    return resolved


def _trusted_rust_toolchain() -> dict[str, Path]:
    tools = {
        name: _trusted_executable(
            path,
            expected_sha256,
            expected_basename="rustc" if name == "rustc" else None,
        )
        for name, (path, expected_sha256) in RUST_TOOL_SPECS.items()
    }
    if tools["cargo"].parent != tools["rustc"].parent:
        raise BoundaryError("the reviewed Cargo and Rust compiler directories differ")
    if tools["cargo-fmt"].parent != tools["cargo"].parent:
        raise BoundaryError("the reviewed Cargo formatter directory differs")
    if tools["cargo-clippy"].parent != tools["cargo"].parent:
        raise BoundaryError("the reviewed Cargo Clippy directory differs")
    if tools["clippy-driver"].parent != tools["rustc"].parent:
        raise BoundaryError("the reviewed Clippy driver directory differs")
    return tools


def _is_beneath(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _read_workspace_file(root: Path, relative: str) -> bytes:
    path = root / relative
    if not _is_beneath(path, root) or path.is_symlink():
        raise BoundaryError("a Rust workspace file escaped through a symbolic link")
    current = root
    for component in Path(relative).parts:
        current = current / component
        if current.is_symlink():
            raise BoundaryError("a Rust workspace path contains a symbolic link")

    flags = os.O_RDONLY | os.O_CLOEXEC
    if not hasattr(os, "O_NOFOLLOW"):
        raise BoundaryError("safe Rust workspace file opening is unavailable")
    flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
        try:
            before = os.fstat(descriptor)
            if not stat.S_ISREG(before.st_mode) or before.st_size > 256 * 1024:
                raise BoundaryError("a Rust workspace file is not a bounded regular file")
            chunks: list[bytes] = []
            total = 0
            while total <= 256 * 1024:
                chunk = os.read(descriptor, min(16 * 1024, 256 * 1024 + 1 - total))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
            if total > 256 * 1024:
                raise BoundaryError("a Rust workspace file exceeds its size boundary")
            after = os.fstat(descriptor)
        finally:
            os.close(descriptor)
    except OSError as error:
        raise BoundaryError("a Rust workspace file cannot be read safely") from error

    identity_before = (
        before.st_dev,
        before.st_ino,
        before.st_mode,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    identity_after = (
        after.st_dev,
        after.st_ino,
        after.st_mode,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if identity_before != identity_after:
        raise BoundaryError("a Rust workspace file changed while it was read")
    return b"".join(chunks)


def _toml_document(data: bytes, reason: str) -> Mapping[str, object]:
    try:
        document = tomllib.loads(data.decode("utf-8"))
    except (UnicodeError, tomllib.TOMLDecodeError) as error:
        raise BoundaryError(reason) from error
    if not isinstance(document, dict):
        raise BoundaryError(reason)
    return document


def _validate_rust_workspace(
    root: Path = ROOT,
    expected_files: Mapping[str, str] = RUST_WORKSPACE_FILES,
) -> None:
    try:
        root = root.resolve(strict=True)
    except OSError as error:
        raise BoundaryError("the Rust workspace root cannot be resolved") from error
    if not root.is_dir() or root.is_symlink():
        raise BoundaryError("the Rust workspace root is not a trusted directory")

    expected_names = set(expected_files)
    discovered = {"Cargo.toml", "Cargo.lock"}
    crates_root = root / "crates"
    if not crates_root.is_dir() or crates_root.is_symlink():
        raise BoundaryError("the Rust crates root is not a trusted directory")
    for path in crates_root.rglob("*"):
        if path.is_symlink():
            raise BoundaryError("the Rust workspace contains a symbolic link")
        if path.is_file():
            discovered.add(path.relative_to(root).as_posix())
        elif not path.is_dir():
            raise BoundaryError("the Rust workspace contains a special file")
    if discovered != expected_names:
        raise BoundaryError("the Rust workspace file boundary changed")

    contents: dict[str, bytes] = {}
    for relative, expected_sha256 in expected_files.items():
        data = _read_workspace_file(root, relative)
        contents[relative] = data
        if hashlib.sha256(data).hexdigest() != expected_sha256:
            raise BoundaryError("the reviewed Rust workspace identity changed")

    for ancestor in (root, *root.parents):
        for name in ("config", "config.toml", "credentials", "credentials.toml"):
            candidate = ancestor / ".cargo" / name
            if candidate.exists() or candidate.is_symlink():
                raise BoundaryError("an ambient Cargo configuration or credential file exists")

    workspace_manifest = _toml_document(
        contents["Cargo.toml"], "workspace_manifest_invalid"
    )
    workspace = workspace_manifest.get("workspace")
    if not isinstance(workspace, dict):
        raise BoundaryError("workspace_manifest_invalid")
    if workspace.get("members") != ["crates/prismdrake-foundation"]:
        raise BoundaryError("workspace_member_boundary_changed")
    if workspace.get("resolver") != "3":
        raise BoundaryError("workspace_resolver_changed")
    forbidden_manifest_keys = {
        "dependencies",
        "dev-dependencies",
        "build-dependencies",
        "patch",
        "replace",
        "target",
    }
    if forbidden_manifest_keys.intersection(
        workspace_manifest
    ) or forbidden_manifest_keys.intersection(workspace):
        raise BoundaryError("workspace_dependency_surface_detected")

    crate_manifest = _toml_document(
        contents["crates/prismdrake-foundation/Cargo.toml"],
        "crate_manifest_invalid",
    )
    if forbidden_manifest_keys.intersection(crate_manifest):
        raise BoundaryError("crate_dependency_surface_detected")
    package = crate_manifest.get("package")
    library = crate_manifest.get("lib")
    if not isinstance(package, dict) or package.get("name") != "prismdrake-foundation":
        raise BoundaryError("crate_identity_changed")
    if "build" in package or "links" in package:
        raise BoundaryError("crate_build_or_native_surface_detected")
    if not isinstance(library, dict) or library.get("path") != "src/lib.rs":
        raise BoundaryError("crate_source_boundary_changed")
    if library.get("proc-macro") is True:
        raise BoundaryError("crate_proc_macro_surface_detected")

    lock = _toml_document(contents["Cargo.lock"], "workspace_lock_invalid")
    packages = lock.get("package")
    if lock.get("version") != 4 or not isinstance(packages, list) or len(packages) != 1:
        raise BoundaryError("workspace_lock_dependency_surface_detected")
    locked_package = packages[0]
    if not isinstance(locked_package, dict):
        raise BoundaryError("workspace_lock_invalid")
    if set(locked_package) != {"name", "version"}:
        raise BoundaryError("workspace_lock_dependency_surface_detected")
    if locked_package.get("name") != "prismdrake-foundation":
        raise BoundaryError("workspace_lock_identity_changed")

    try:
        policy_source = contents[
            "crates/prismdrake-foundation/src/lib.rs"
        ].decode("utf-8")
        rust_sources = "\n".join(
            data.decode("utf-8")
            for relative, data in contents.items()
            if relative.endswith(".rs")
        )
    except UnicodeError as error:
        raise BoundaryError("crate_source_invalid") from error
    if "#![forbid(unsafe_code)]" not in policy_source:
        raise BoundaryError("crate_unsafe_policy_missing")
    rust_sources_without_policy = rust_sources.replace(
        "#![forbid(unsafe_code)]", "", 1
    )
    if any(
        pattern.search(rust_sources_without_policy)
        for pattern in FORBIDDEN_RUST_SOURCE
    ):
        raise BoundaryError("crate_unreviewed_unsafe_or_native_surface_detected")


def _ensure_state_directory(path: Path) -> Path:
    if not path.is_absolute() or not _is_beneath(path, ROOT) or path == ROOT:
        raise BoundaryError("a validation state path is outside the repository")

    relative = path.relative_to(ROOT)
    current = ROOT
    for component in relative.parts:
        current = current / component
        if current.is_symlink():
            raise BoundaryError("a validation state path contains a symbolic link")
        if current.exists():
            if not current.is_dir():
                raise BoundaryError("a validation state path contains a non-directory")
        else:
            try:
                current.mkdir(mode=0o700)
            except FileExistsError:
                pass
            except OSError as error:
                raise BoundaryError("a validation state directory cannot be created") from error
        if current.is_symlink():
            raise BoundaryError("a validation state path contains a symbolic link")

    try:
        resolved = path.resolve(strict=True)
        metadata = path.lstat()
    except OSError as error:
        raise BoundaryError("a validation state directory cannot be resolved") from error
    if not _is_beneath(resolved, ROOT) or resolved != path:
        raise BoundaryError("a validation state path escaped the repository")
    if not stat.S_ISDIR(metadata.st_mode):
        raise BoundaryError("a validation state path is not a directory")

    if path == STATE_ROOT or STATE_ROOT in path.parents:
        if metadata.st_uid != os.getuid():
            raise BoundaryError("a validation state directory has an unexpected owner")
        if stat.S_IMODE(metadata.st_mode) & 0o077:
            raise BoundaryError("a validation state directory is not private")
    return resolved


def _prepare_state_paths(selector_name: str) -> dict[str, Path]:
    if selector_name not in SELECTOR_ENVIRONMENT_ALLOWLIST:
        raise BoundaryError("the selector has no local state policy")

    _ensure_state_directory(STATE_ROOT)
    selector_root = _ensure_state_directory(STATE_ROOT / selector_name)
    paths = {
        name: _ensure_state_directory(selector_root / name)
        for name in ("home", "tmp", "cache", "config", "data", "state", "runtime")
    }
    if selector_name in RUST_SELECTOR_NAMES:
        paths["cargo_home"] = _ensure_state_directory(selector_root / "cargo-home")
        paths["target"] = _ensure_state_directory(selector_root / "target")
    return paths


def _validate_private_state_tree(path: Path, *, cargo_home: bool = False) -> None:
    forbidden_cargo_names = {
        "config",
        "config.toml",
        "credentials",
        "credentials.toml",
        "git",
        "registry",
    }
    for entry in path.rglob("*"):
        if entry.is_symlink():
            raise BoundaryError("a validation state tree contains a symbolic link")
        try:
            metadata = entry.lstat()
        except OSError as error:
            raise BoundaryError("a validation state entry cannot be inspected") from error
        if not (stat.S_ISDIR(metadata.st_mode) or stat.S_ISREG(metadata.st_mode)):
            raise BoundaryError("a validation state tree contains a special file")
        if metadata.st_uid != os.getuid() or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise BoundaryError("a validation state entry is not privately owned")
        if cargo_home and entry.relative_to(path).parts[0] in forbidden_cargo_names:
            raise BoundaryError("the isolated Cargo home contains configuration or registry state")


def _selector_environment(
    entry: Selector,
    rust_tools: Mapping[str, Path] | None = None,
) -> dict[str, str]:
    additions = SELECTOR_ENVIRONMENT_ALLOWLIST.get(entry.name)
    if additions is None:
        raise BoundaryError("the selector has no local environment policy")

    paths = _prepare_state_paths(entry.name)
    environment = {
        "HOME": str(paths["home"]),
        "TMPDIR": str(paths["tmp"]),
        "TMP": str(paths["tmp"]),
        "TEMP": str(paths["tmp"]),
        "XDG_CACHE_HOME": str(paths["cache"]),
        "XDG_CONFIG_HOME": str(paths["config"]),
        "XDG_DATA_HOME": str(paths["data"]),
        "XDG_STATE_HOME": str(paths["state"]),
        "XDG_RUNTIME_DIR": str(paths["runtime"]),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
    }
    if entry.name in RUST_SELECTOR_NAMES:
        if rust_tools is None:
            raise BoundaryError("the Rust selector has no reviewed toolchain")
        _validate_private_state_tree(paths["cargo_home"], cargo_home=True)
        _validate_private_state_tree(paths["target"])
        environment.update(
            {
                "PATH": str(rust_tools["cargo"].parent),
                "CARGO": str(rust_tools["cargo"]),
                "RUSTC": str(rust_tools["rustc"]),
                "RUSTDOC": str(rust_tools["rustdoc"]),
                "RUSTFMT": str(rust_tools["rustfmt"]),
                "CARGO_HOME": str(paths["cargo_home"]),
                "CARGO_TARGET_DIR": str(paths["target"]),
                "CARGO_NET_OFFLINE": "true",
                "CARGO_TERM_COLOR": "never",
                "CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER": str(
                    rust_tools["linker"]
                ),
                "RUST_BACKTRACE": "0",
            }
        )
    environment.update(additions)
    return environment


def _execute_command(command: tuple[str, ...], environment: Mapping[str, str]) -> int:
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        env=dict(environment),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        close_fds=True,
        start_new_session=True,
        umask=0o077,
    )
    try:
        return process.wait(timeout=SELECTOR_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        raise


def _run_selector(entry: Selector) -> int:
    try:
        if entry.command is None:
            raise BoundaryError("the selector has no command")
        if entry.name in RUST_SELECTOR_NAMES:
            rust_tools = _trusted_rust_toolchain()
            _validate_rust_workspace()
            if entry.command[0] != CARGO_EXECUTABLE:
                raise BoundaryError("the selector command is not bound to reviewed Cargo")
            environment = _selector_environment(entry, rust_tools)
            command = (str(rust_tools["cargo"]), *entry.command[1:])
        else:
            python = _trusted_python_executable()
            if entry.command[0] != PYTHON_EXECUTABLE:
                raise BoundaryError("the selector command is not bound to the active Python")
            environment = _selector_environment(entry)
            command = (str(python), *entry.command[1:])
        print(f"{entry.tier} {entry.name}: running", flush=True)
        returncode = _execute_command(command, environment)
    except subprocess.TimeoutExpired:
        print(f"{entry.tier} {entry.name}: timed out", flush=True)
        return 124
    except (BoundaryError, OSError):
        print(
            f"{entry.tier} {entry.name}: blocked by the local execution boundary",
            file=sys.stderr,
            flush=True,
        )
        return 2

    status = "passed" if returncode == 0 else f"failed ({returncode})"
    print(f"{entry.tier} {entry.name}: {status}", flush=True)
    return returncode


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)

    if args.run and args.list:
        parser.error("--run cannot be combined with --list")
    if args.run and args.selector is None:
        parser.error("--run requires --selector")
    if args.json and not args.list:
        parser.error("--json is available only with --list")
    if args.json and args.run:
        parser.error("--json cannot be combined with --run")

    entries = _selected(args.selector, args.tier)
    if not entries:
        print("validation selector not found for the requested tier", file=sys.stderr)
        return 2

    if not args.run:
        _print_entries(entries, args.json)
        return 0

    entry = entries[0]
    if (
        entry.tier not in RUNNABLE_TIERS
        or not entry.runnable_locally
        or entry.command is None
    ):
        reason = entry.blocked_reason or "no local execution policy is defined"
        print(
            f"refusing {entry.tier} selector {entry.name}: {reason}",
            file=sys.stderr,
            flush=True,
        )
        return 2

    return _run_selector(entry)


if __name__ == "__main__":
    raise SystemExit(main())
