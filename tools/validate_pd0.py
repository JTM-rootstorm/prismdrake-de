#!/usr/bin/env python3
"""Validate Prismdrake PD0 documents and machine-readable contracts."""

from __future__ import annotations

import copy
import json
import re
import sys
import tomllib
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parent.parent
SUPPORTED_SCHEMA_VERSION = 1
PROFILE_NAMES = {
    "lustre": "Prismdrake Lustre",
    "forge": "Prismdrake Forge",
}
ALLOWED_DBUS_BASES = ("org.prismdrake", "org.freedesktop")
LICENSE_SHA256 = "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"
DEPENDENCY_MANIFEST_COMPONENTS = {
    "prismdrake-foundation": "internal_library",
    "prismdrake-session": "core_service",
    "prismdrake-settingsd": "core_service",
    "prismdrake-shell": "visible_shell",
}
FORBIDDEN_CORE_RUNTIME_DEPENDENCIES = {
    "gnome-shell",
    "mutter",
    "gnome-settings-daemon",
    "gnome-control-center",
    "libadwaita",
}

REQUIRED_FILES = (
    ".editorconfig",
    ".gitignore",
    "AGENTS.md",
    "CONTRIBUTING.md",
    "docs/PRISMDRAKE_PROJECT_SPECIFICATION.md",
    "LICENSE",
    "Makefile",
    "README.md",
    "SECURITY.md",
    ".github/ISSUE_TEMPLATE/architecture.yml",
    ".github/ISSUE_TEMPLATE/bug.yml",
    ".github/ISSUE_TEMPLATE/feature.yml",
    ".github/pull_request_template.md",
    ".github/workflows/pd0-validate.yml",
    "docs/index.md",
    "docs/vision/product.md",
    "docs/vision/naming.md",
    "docs/vision/design-principles.md",
    "docs/vision/non-goals.md",
    "docs/architecture/overview.md",
    "docs/architecture/component-model.md",
    "docs/architecture/process-model.md",
    "docs/architecture/dependency-policy.md",
    "docs/architecture/configuration.md",
    "docs/architecture/compatibility.md",
    "docs/architecture/glasswyrm-integration.md",
    "docs/architecture/failure-and-fallbacks.md",
    "docs/design/visual-language.md",
    "docs/design/theme-tokens.md",
    "docs/design/accessibility.md",
    "docs/design/mockups/shell-overview.svg",
    "docs/design/mockups/launcher-lustre.svg",
    "docs/design/mockups/launcher-forge.svg",
    "docs/design/mockups/window-states.svg",
    "docs/design/mockups/notifications.svg",
    "docs/design/mockups/quick-settings.svg",
    "docs/research/toolkit-evaluation.md",
    "docs/roadmap/milestones.md",
    "docs/roadmap/pd1.md",
    "docs/adr/README.md",
    "docs/adr/0001-project-identity.md",
    "docs/adr/0002-component-and-process-model.md",
    "docs/adr/0003-shell-toolkit.md",
    "docs/adr/0004-configuration-format.md",
    "docs/adr/0005-standards-and-glasswyrm-integration.md",
    "docs/adr/0006-theme-token-model.md",
    "docs/adr/0007-licensing-and-original-assets.md",
    "examples/config/lustre.toml",
    "examples/config/forge.toml",
    "examples/config/accessible.toml",
    "examples/capabilities/x11-standard.json",
    "examples/capabilities/glasswyrm-enhanced.json",
    "interfaces/README.md",
    "interfaces/dbus/org.prismdrake.Settings1.xml",
    "schemas/prismdrake-config.schema.json",
    "schemas/prismdrake-theme-tokens.schema.json",
    "schemas/prismdrake-capabilities.schema.json",
    "schemas/prismdrake-dependency-manifest.schema.json",
    "manifests/dependencies/prismdrake-foundation.json",
    "manifests/dependencies/prismdrake-session.json",
    "manifests/dependencies/prismdrake-settingsd.json",
    "manifests/dependencies/prismdrake-shell.json",
    "docs/build/dependencies.md",
    "themes/base.tokens.json",
    "themes/lustre.tokens.json",
    "themes/forge.tokens.json",
    "themes/accessibility.tokens.json",
    "tools/validate_pd0.py",
)

ADR_SECTIONS = (
    "Context",
    "Decision drivers",
    "Considered options",
    "Decision",
    "Consequences",
    "Validation or evidence",
    "Revisit conditions",
    "References",
)
ADR_STATUSES = {"Proposed", "Accepted", "Rejected", "Superseded", "Deprecated"}
SEMANTIC_COLOR_KEYS = {
    "panel_surface",
    "elevated_surface",
    "window_frame",
    "border_active",
    "border_inactive",
    "text_primary",
    "text_muted",
    "selection",
    "focus_ring",
    "danger",
    "warning",
    "success",
}
MATERIAL_KEYS = {"panel", "launcher", "notification", "menu"}
COMPONENT_KEYS = {
    "task_button",
    "launcher_tile",
    "titlebar_button",
    "notification_card",
    "quick_setting",
    "tooltip",
    "menu_item",
}


class Validation:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def error(self, location: str, message: str) -> None:
        self.errors.append(f"{location}: {message}")

    def require(self, condition: bool, location: str, message: str) -> None:
        if not condition:
            self.error(location, message)


def load_json(path: Path, validation: Validation) -> Any | None:
    try:
        with path.open(encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        validation.error(path.relative_to(ROOT).as_posix(), f"invalid JSON: {error}")
        return None


def load_toml(path: Path, validation: Validation) -> Any | None:
    try:
        with path.open("rb") as stream:
            return tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        validation.error(path.relative_to(ROOT).as_posix(), f"invalid TOML: {error}")
        return None


def resolve_ref(root_schema: dict[str, Any], reference: str) -> dict[str, Any]:
    if not reference.startswith("#/"):
        raise ValueError(f"only local schema references are supported: {reference}")
    value: Any = root_schema
    for raw_part in reference[2:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        value = value[part]
    if not isinstance(value, dict):
        raise ValueError(f"schema reference is not an object: {reference}")
    return value


def is_json_type(instance: Any, expected: str) -> bool:
    if expected == "null":
        return instance is None
    if expected == "boolean":
        return isinstance(instance, bool)
    if expected == "integer":
        return isinstance(instance, int) and not isinstance(instance, bool)
    if expected == "number":
        return isinstance(instance, (int, float)) and not isinstance(instance, bool)
    if expected == "string":
        return isinstance(instance, str)
    if expected == "array":
        return isinstance(instance, list)
    if expected == "object":
        return isinstance(instance, dict)
    return False


def validate_schema(
    instance: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    location: str,
) -> list[str]:
    """Validate the JSON Schema subset used by the PD0 contracts."""
    if "$ref" in schema:
        try:
            schema = resolve_ref(root_schema, schema["$ref"])
        except (KeyError, TypeError, ValueError) as error:
            return [f"{location}: invalid schema reference: {error}"]

    errors: list[str] = []
    expected = schema.get("type")
    if expected is not None:
        expected_types = expected if isinstance(expected, list) else [expected]
        if not any(is_json_type(instance, item) for item in expected_types):
            return [f"{location}: expected {' or '.join(expected_types)}, got {type(instance).__name__}"]

    if "const" in schema and instance != schema["const"]:
        errors.append(f"{location}: expected constant {schema['const']!r}, got {instance!r}")
    if "enum" in schema and instance not in schema["enum"]:
        errors.append(f"{location}: expected one of {schema['enum']!r}, got {instance!r}")

    if isinstance(instance, dict):
        for name in schema.get("required", []):
            if name not in instance:
                errors.append(f"{location}: missing required key {name!r}")
        properties = schema.get("properties", {})
        for name, value in instance.items():
            child_location = f"{location}.{name}"
            if name in properties:
                errors.extend(validate_schema(value, properties[name], root_schema, child_location))
            elif schema.get("additionalProperties") is False:
                errors.append(f"{child_location}: unknown key")
            elif isinstance(schema.get("additionalProperties"), dict):
                errors.extend(
                    validate_schema(value, schema["additionalProperties"], root_schema, child_location)
                )
        minimum_properties = schema.get("minProperties")
        if minimum_properties is not None and len(instance) < minimum_properties:
            errors.append(f"{location}: expected at least {minimum_properties} keys")

    if isinstance(instance, list):
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, value in enumerate(instance):
                errors.extend(validate_schema(value, item_schema, root_schema, f"{location}[{index}]"))
        if len(instance) < schema.get("minItems", 0):
            errors.append(f"{location}: too few items")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            errors.append(f"{location}: exceeds maximum of {schema['maxItems']} items")
        if schema.get("uniqueItems"):
            serialized = [json.dumps(value, sort_keys=True) for value in instance]
            if len(serialized) != len(set(serialized)):
                errors.append(f"{location}: items must be unique")

    if isinstance(instance, str):
        if len(instance) < schema.get("minLength", 0):
            errors.append(f"{location}: string is too short")
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            errors.append(f"{location}: string exceeds {schema['maxLength']} characters")
        if "pattern" in schema and re.fullmatch(schema["pattern"], instance) is None:
            errors.append(f"{location}: value {instance!r} does not match {schema['pattern']!r}")

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append(f"{location}: value is below minimum {schema['minimum']}")
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append(f"{location}: value exceeds maximum {schema['maximum']}")

    return errors


def validate_required_files(validation: Validation) -> None:
    for relative in REQUIRED_FILES:
        validation.require((ROOT / relative).is_file(), relative, "required repository file is missing")


def dependency_identity_candidates(dependency: dict[str, Any]) -> set[str]:
    candidates = {str(dependency.get("name", "")).lower().replace("_", "-")}
    atom = dependency.get("gentoo_atom")
    if isinstance(atom, str) and "/" in atom:
        package = atom.split("/", 1)[1].split(":", 1)[0]
        candidates.add(package.lower().replace("_", "-"))
    return candidates


def dependency_manifest_policy_errors(document: dict[str, Any], location: str) -> list[str]:
    errors: list[str] = []
    dependencies = document.get("dependencies", [])
    if not isinstance(dependencies, list):
        return errors

    if document.get("component_kind") in {"core_service", "visible_shell"}:
        if document.get("mandatory_core_startup") is not True:
            errors.append(
                f"{location}.mandatory_core_startup: core services and the visible shell must enforce the core runtime boundary"
            )

    seen_names: set[str] = set()
    for index, dependency in enumerate(dependencies):
        if not isinstance(dependency, dict):
            continue
        dependency_location = f"{location}.dependencies[{index}]"
        name = dependency.get("name")
        if isinstance(name, str):
            if name in seen_names:
                errors.append(f"{dependency_location}.name: duplicate dependency name {name!r}")
            seen_names.add(name)

        scope = dependency.get("scope")
        if document.get("mandatory_core_startup") is True and scope == "mandatory_runtime":
            forbidden = dependency_identity_candidates(dependency) & FORBIDDEN_CORE_RUNTIME_DEPENDENCIES
            if forbidden:
                errors.append(
                    f"{dependency_location}: forbidden mandatory core runtime dependency "
                    f"{sorted(forbidden)[0]!r}"
                )

        version = dependency.get("version", {})
        if not isinstance(version, dict):
            continue
        declared_minimum = version.get("declared_minimum")
        verified_minimum = version.get("verified_minimum")
        observed = version.get("observed")
        evidence = version.get("evidence")
        if dependency.get("requirement_status") == "planned" and verified_minimum is not None:
            errors.append(
                f"{dependency_location}.version.verified_minimum: planned dependencies cannot declare a verified minimum"
            )
        if evidence == "unverified" and (verified_minimum is not None or observed is not None):
            errors.append(
                f"{dependency_location}.version: unverified dependencies cannot declare verified or observed versions"
            )
        if evidence in {"observed_reference", "verified_component"} and observed is None:
            errors.append(
                f"{dependency_location}.version.observed: {evidence} evidence requires an observed version"
            )
        if verified_minimum is not None and evidence != "verified_component":
            errors.append(
                f"{dependency_location}.version.verified_minimum: a verified minimum requires verified component evidence"
            )
        if declared_minimum is not None and dependency.get("requirement_status") == "observed":
            errors.append(
                f"{dependency_location}.version.declared_minimum: observed-only tools cannot set project constraints"
            )
        if scope == "optional_runtime" and not dependency.get("fallback"):
            errors.append(
                f"{dependency_location}.fallback: optional runtime dependencies require a non-empty fallback"
            )

    if document.get("implementation_status") == "planned":
        if document.get("runtime_dependency_state") != "planned_unmeasured":
            errors.append(
                f"{location}.runtime_dependency_state: planned components must remain planned_unmeasured"
            )
        if not document.get("unresolved"):
            errors.append(f"{location}.unresolved: planned components must record unresolved dependency work")

    if document.get("runtime_dependency_state") == "not_applicable":
        runtime_scopes = {"mandatory_runtime", "optional_runtime"}
        if any(isinstance(item, dict) and item.get("scope") in runtime_scopes for item in dependencies):
            errors.append(
                f"{location}.runtime_dependency_state: not_applicable manifests cannot declare runtime dependencies"
            )

    return errors


def validate_contracts(validation: Validation) -> dict[str, Any]:
    schemas: dict[str, Any] = {}
    for name in ("config", "theme-tokens", "capabilities", "dependency-manifest"):
        path = ROOT / "schemas" / f"prismdrake-{name}.schema.json"
        document = load_json(path, validation)
        if isinstance(document, dict):
            schemas[name] = document
            validation.require(
                document.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
                path.relative_to(ROOT).as_posix(),
                "schema must declare JSON Schema draft 2020-12",
            )

    if "config" in schemas:
        for path in sorted((ROOT / "examples/config").glob("*.toml")):
            document = load_toml(path, validation)
            if document is None:
                continue
            location = path.relative_to(ROOT).as_posix()
            validation.errors.extend(validate_schema(document, schemas["config"], schemas["config"], location))
            validation.require(
                document.get("profile") in PROFILE_NAMES,
                f"{location}.profile",
                "profile must be exactly 'lustre' or 'forge'",
            )
            validation.require(
                document.get("schema_version") == SUPPORTED_SCHEMA_VERSION,
                f"{location}.schema_version",
                f"only schema version {SUPPORTED_SCHEMA_VERSION} is supported",
            )
            validation.require(
                document.get("developer", {}).get("diagnostics_enabled") is False
                and document.get("developer", {}).get("mock_capability_overrides") == [],
                f"{location}.developer",
                "examples must keep diagnostics and mock capability overrides disabled",
            )

    theme_documents: list[dict[str, Any]] = []
    if "theme-tokens" in schemas:
        for path in sorted((ROOT / "themes").glob("*.tokens.json")):
            document = load_json(path, validation)
            if not isinstance(document, dict):
                continue
            location = path.relative_to(ROOT).as_posix()
            theme_documents.append(document)
            validation.errors.extend(
                validate_schema(document, schemas["theme-tokens"], schemas["theme-tokens"], location)
            )
            validation.require(
                document.get("schema_version") == SUPPORTED_SCHEMA_VERSION,
                f"{location}.schema_version",
                f"only schema version {SUPPORTED_SCHEMA_VERSION} is supported",
            )
            colors = document.get("semantic", {}).get("colors", {})
            materials = document.get("semantic", {}).get("materials", {})
            components = document.get("component", {})
            validation.require(set(colors) == SEMANTIC_COLOR_KEYS, location, "semantic color keys do not match the required set")
            validation.require(set(materials) == MATERIAL_KEYS, location, "material keys do not match the required set")
            validation.require(set(components) == COMPONENT_KEYS, location, "component keys do not match the required set")
            for material_name, material in materials.items():
                validation.require(
                    isinstance(material, dict) and isinstance(material.get("fallback"), dict),
                    f"{location}.semantic.materials.{material_name}",
                    "every material must define a non-blur fallback",
                )
            accessibility = document.get("accessibility_overrides", {})
            validation.require(
                "high_contrast" in accessibility
                and accessibility.get("high_contrast", {}).get("focus_width_px", 0) >= 1,
                f"{location}.accessibility_overrides",
                "accessibility overrides must retain high-contrast focus emphasis",
            )

        profiles = {document.get("profile_id"): document for document in theme_documents if document.get("layer") == "profile"}
        validation.require(set(profiles) == set(PROFILE_NAMES), "themes", "profile layers must be exactly lustre and forge")
        for profile_id, display_name in PROFILE_NAMES.items():
            document = profiles.get(profile_id, {})
            validation.require(
                document.get("profile_display_name") == display_name,
                f"themes/{profile_id}.tokens.json",
                f"display name must be {display_name!r}",
            )
        if set(profiles) == set(PROFILE_NAMES):
            lustre = profiles["lustre"]
            forge = profiles["forge"]
            validation.require(
                set(lustre["semantic"]["colors"]) == set(forge["semantic"]["colors"]),
                "themes",
                "Lustre and Forge semantic color keys must match",
            )
            validation.require(
                set(lustre["semantic"]["materials"]) == set(forge["semantic"]["materials"]),
                "themes",
                "Lustre and Forge material keys must match",
            )
            validation.require(
                set(lustre["component"]) == set(forge["component"]),
                "themes",
                "Lustre and Forge component keys must match",
            )

    if "capabilities" in schemas:
        native_pattern = re.compile(r"GW_[A-Z0-9_]+_V[1-9][0-9]*")
        for path in sorted((ROOT / "examples/capabilities").glob("*.json")):
            document = load_json(path, validation)
            if not isinstance(document, dict):
                continue
            location = path.relative_to(ROOT).as_posix()
            validation.errors.extend(
                validate_schema(document, schemas["capabilities"], schemas["capabilities"], location)
            )
            for index, capability in enumerate(document.get("native_capabilities", [])):
                name = capability.get("name", "")
                validation.require(
                    native_pattern.fullmatch(name) is not None and "PRISMDRAKE" not in name,
                    f"{location}.native_capabilities[{index}].name",
                    "native names must be generic, explicit-version GW_* identifiers",
                )
            validation.require(
                set(document.get("fallbacks", {})) == {"blur", "thumbnails", "workspaces", "decorations"},
                f"{location}.fallbacks",
                "capability example must define every required fallback",
            )

    if "dependency-manifest" in schemas:
        manifest_paths = sorted((ROOT / "manifests/dependencies").glob("*.json"))
        validation.require(
            {path.stem for path in manifest_paths} == set(DEPENDENCY_MANIFEST_COMPONENTS),
            "manifests/dependencies",
            "dependency manifests must match the required PD1 component set",
        )
        for path in manifest_paths:
            document = load_json(path, validation)
            if not isinstance(document, dict):
                continue
            location = path.relative_to(ROOT).as_posix()
            validation.errors.extend(
                validate_schema(
                    document,
                    schemas["dependency-manifest"],
                    schemas["dependency-manifest"],
                    location,
                )
            )
            validation.require(
                document.get("component") == path.stem,
                f"{location}.component",
                "component must match the manifest filename",
            )
            validation.require(
                document.get("component_kind") == DEPENDENCY_MANIFEST_COMPONENTS.get(path.stem),
                f"{location}.component_kind",
                "component kind must match the reviewed PD1 boundary",
            )
            validation.errors.extend(dependency_manifest_policy_errors(document, location))

    return schemas


def invalid_dbus_interface_names(root: ET.Element) -> list[str]:
    return [
        interface.get("name", "")
        for interface in root.findall(".//interface")
        if not interface.get("name", "").startswith("org.prismdrake.")
    ]


def validate_xml(validation: Validation) -> None:
    for path in sorted((ROOT / "interfaces").rglob("*.xml")):
        location = path.relative_to(ROOT).as_posix()
        try:
            tree = ET.parse(path)
        except (OSError, ET.ParseError) as error:
            validation.error(location, f"invalid XML: {error}")
            continue
        interfaces = tree.findall(".//interface")
        validation.require(bool(interfaces), location, "D-Bus XML must contain an interface")
        for name in invalid_dbus_interface_names(tree.getroot()):
            validation.error(location, f"D-Bus interface {name!r} must begin with 'org.prismdrake.'")

    mockups = sorted((ROOT / "docs/design/mockups").glob("*.svg"))
    validation.require(len(mockups) == 6, "docs/design/mockups", "exactly six required PD0 mockups must exist")
    for path in mockups:
        location = path.relative_to(ROOT).as_posix()
        try:
            tree = ET.parse(path)
        except (OSError, ET.ParseError) as error:
            validation.error(location, f"invalid SVG XML: {error}")
            continue
        text = path.read_text(encoding="utf-8")
        validation.require("LOW-FIDELITY" in text, location, "mockup must be visibly labeled low-fidelity")
        for font in re.findall(r"font-family=[\"']([^\"']+)", text):
            validation.require(
                font.strip().lower() in {"sans-serif", "serif", "monospace"},
                location,
                f"mockup font {font!r} is not an available generic family",
            )
        root = tree.getroot()
        validation.require(root.tag.endswith("svg"), location, "mockup root element must be SVG")


def markdown_files() -> list[Path]:
    files = list(ROOT.glob("*.md"))
    for directory in (ROOT / "Docs", ROOT / "docs", ROOT / "interfaces"):
        files.extend(directory.rglob("*.md"))
    return sorted(set(files))


def validate_markdown_links(validation: Validation) -> None:
    link_pattern = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
    for path in markdown_files():
        location = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        for raw_target in link_pattern.findall(text):
            target = raw_target.strip().strip("<>").split(maxsplit=1)[0]
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            file_part = unquote(target.split("#", 1)[0])
            if not file_part:
                continue
            resolved = (path.parent / file_part).resolve()
            try:
                resolved.relative_to(ROOT)
            except ValueError:
                validation.error(location, f"local link escapes repository: {target}")
                continue
            validation.require(resolved.exists(), location, f"local link does not resolve: {target}")


def validate_adrs(validation: Validation) -> None:
    for path in sorted((ROOT / "docs/adr").glob("[0-9][0-9][0-9][0-9]-*.md")):
        location = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        status_match = re.search(r"^- \*\*Status:\*\* ([A-Za-z]+)", text, re.MULTILINE)
        if not status_match:
            validation.error(location, "ADR must contain a bold Status field")
            continue
        status = status_match.group(1)
        validation.require(status in ADR_STATUSES, location, f"unsupported ADR status {status!r}")
        for section in ADR_SECTIONS:
            validation.require(f"## {section}" in text, location, f"missing ADR section {section!r}")
        if status == "Accepted":
            validation.require(
                re.search(r"\b(?:TODO|TBD)\b|\?\?\?", text) is None,
                location,
                "Accepted ADR contains an unresolved placeholder marker",
            )
        if status == "Proposed":
            validation.require("Proposed" in text, location, "Proposed ADR must be visibly labeled")


def text_contract_files() -> list[Path]:
    roots = [ROOT / ".github", ROOT / "Docs", ROOT / "docs", ROOT / "examples", ROOT / "interfaces", ROOT / "schemas", ROOT / "themes", ROOT / "tools"]
    files = [ROOT / name for name in ("README.md", "CONTRIBUTING.md", "SECURITY.md", "Makefile")]
    for directory in roots:
        if directory.exists():
            files.extend(path for path in directory.rglob("*") if path.is_file())
    return files


def validate_identity_and_hygiene(validation: Validation) -> None:
    required_identity = (
        "Prismdrake Desktop Environment",
        "Prismdrake Lustre",
        "Prismdrake Forge",
        "prismdrake-*",
        "org.prismdrake.*",
        "GW_*",
        "JTM-rootstorm/prismdrake-de",
    )
    for relative in ("README.md", "docs/vision/naming.md", "docs/adr/0001-project-identity.md"):
        text = (ROOT / relative).read_text(encoding="utf-8")
        for expected in required_identity:
            validation.require(expected in text, relative, f"missing canonical identity value {expected!r}")

    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    normalized_readme = " ".join(readme.split())
    validation.require(
        "does not yet contain a usable desktop shell" in normalized_readme,
        "README.md",
        "README must clearly state that Prismdrake is not yet a usable desktop",
    )

    import hashlib

    digest = hashlib.sha256((ROOT / "LICENSE").read_bytes()).hexdigest()
    validation.require(digest == LICENSE_SHA256, "LICENSE", "committed GPL-3.0 license changed from the PD0 baseline")

    for path in text_contract_files():
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        location = path.relative_to(ROOT).as_posix()
        for match in re.finditer(r"\borg\.[A-Za-z0-9_.]+", text):
            name = match.group(0).rstrip(".")
            validation.require(
                any(name == base or name.startswith(f"{base}.") for base in ALLOWED_DBUS_BASES),
                location,
                f"forbidden alternative D-Bus namespace {name!r}",
            )

    font_extensions = {".ttf", ".otf", ".woff", ".woff2", ".eot"}
    font_files = [path for path in ROOT.rglob("*") if path.is_file() and path.suffix.lower() in font_extensions and ".git" not in path.parts]
    validation.require(not font_files, "repository", "font binaries require explicit license review and must not be committed in PD0")


def validate_negative_self_tests(schemas: dict[str, Any], validation: Validation) -> None:
    required = {"config", "theme-tokens", "capabilities", "dependency-manifest"}
    if set(schemas) != required:
        validation.error("self-test", "cannot run negative tests because a schema failed to load")
        return

    config = load_toml(ROOT / "examples/config/lustre.toml", validation)
    theme = load_json(ROOT / "themes/lustre.tokens.json", validation)
    foundation_manifest = load_json(
        ROOT / "manifests/dependencies/prismdrake-foundation.json", validation
    )
    shell_manifest = load_json(ROOT / "manifests/dependencies/prismdrake-shell.json", validation)
    if (
        not isinstance(config, dict)
        or not isinstance(theme, dict)
        or not isinstance(foundation_manifest, dict)
        or not isinstance(shell_manifest, dict)
    ):
        validation.error("self-test", "cannot run negative tests because fixtures failed to load")
        return

    cases: list[tuple[str, Any, dict[str, Any], str]] = []
    invalid_profile = copy.deepcopy(config)
    invalid_profile["profile"] = "sparkle"
    cases.append(("invalid profile", invalid_profile, schemas["config"], "profile"))

    unsupported_version = copy.deepcopy(config)
    unsupported_version["schema_version"] = 99
    cases.append(("unsupported schema version", unsupported_version, schemas["config"], "schema_version"))

    missing_token = copy.deepcopy(theme)
    del missing_token["semantic"]["colors"]["focus_ring"]
    cases.append(("missing token key", missing_token, schemas["theme-tokens"], "focus_ring"))

    missing_fallback = copy.deepcopy(theme)
    del missing_fallback["semantic"]["materials"]["panel"]["fallback"]
    cases.append(("missing blur fallback", missing_fallback, schemas["theme-tokens"], "fallback"))

    unknown_manifest_key = copy.deepcopy(foundation_manifest)
    unknown_manifest_key["implicit_dependencies"] = []
    cases.append(
        (
            "unknown dependency manifest key",
            unknown_manifest_key,
            schemas["dependency-manifest"],
            "unknown key",
        )
    )

    for name, document, schema, expected_fragment in cases:
        errors = validate_schema(document, schema, schema, f"self-test.{name}")
        validation.require(
            bool(errors) and any(expected_fragment in error for error in errors),
            "self-test",
            f"negative case {name!r} was not rejected with an actionable field",
        )

    dependency_policy_cases: list[tuple[str, dict[str, Any], str]] = []
    forbidden_name = copy.deepcopy(shell_manifest)
    forbidden_name["dependencies"][0]["name"] = "gnome-shell"
    forbidden_name["dependencies"][0]["gentoo_atom"] = "gnome-base/gnome-shell"
    dependency_policy_cases.append(("forbidden GNOME Shell runtime", forbidden_name, "gnome-shell"))

    forbidden_atom = copy.deepcopy(shell_manifest)
    forbidden_atom["dependencies"][0]["name"] = "ui-runtime"
    forbidden_atom["dependencies"][0]["gentoo_atom"] = "gui-libs/libadwaita"
    dependency_policy_cases.append(("forbidden libadwaita atom", forbidden_atom, "libadwaita"))

    planned_minimum = copy.deepcopy(shell_manifest)
    planned_minimum["dependencies"][0]["version"]["verified_minimum"] = "6.11.1"
    dependency_policy_cases.append(("unverified planned minimum", planned_minimum, "verified_minimum"))

    optional_without_fallback = copy.deepcopy(shell_manifest)
    optional_without_fallback["dependencies"][0]["scope"] = "optional_runtime"
    optional_without_fallback["dependencies"][0]["fallback"] = None
    dependency_policy_cases.append(("optional runtime without fallback", optional_without_fallback, "fallback"))

    for name, document, expected_fragment in dependency_policy_cases:
        errors = dependency_manifest_policy_errors(document, f"self-test.{name}")
        validation.require(
            bool(errors) and any(expected_fragment in error for error in errors),
            "self-test",
            f"dependency policy case {name!r} was not rejected with an actionable field",
        )

    try:
        wrong_namespace = ET.fromstring('<node><interface name="com.example.Settings1"/></node>')
    except ET.ParseError as error:  # pragma: no cover - literal is known valid
        validation.error("self-test", f"wrong-namespace fixture did not parse: {error}")
    else:
        rejected_names = invalid_dbus_interface_names(wrong_namespace)
        validation.require(
            rejected_names == ["com.example.Settings1"],
            "self-test",
            "wrong D-Bus namespace fixture was not rejected",
        )


def main() -> int:
    if sys.version_info < (3, 11):
        print("error: Prismdrake PD0 validation requires Python 3.11 or newer", file=sys.stderr)
        return 2

    validation = Validation()
    validate_required_files(validation)
    schemas = validate_contracts(validation)
    validate_xml(validation)
    validate_markdown_links(validation)
    validate_adrs(validation)
    validate_identity_and_hygiene(validation)
    validate_negative_self_tests(schemas, validation)

    if validation.errors:
        print(f"PD0 validation failed with {len(validation.errors)} error(s):", file=sys.stderr)
        for error in validation.errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("PD0 validation passed")
    print("  required files: present")
    print("  JSON/TOML/XML/SVG: parsed and structurally validated")
    print("  profiles/themes/fallbacks: consistent")
    print("  ADRs/namespaces/local links/assets: consistent")
    print("  negative self-tests: 10 rejection paths passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
