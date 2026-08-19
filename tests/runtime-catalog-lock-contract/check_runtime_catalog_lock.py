"""Contract for the runtime bundled-catalog locks and release content manifest."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


class ContractFailure(Exception):
    """A repository contract is not satisfied."""


RUNTIME_LOCKS = {
    "software-catalog": (
        "kSoftwareCatalogBytes",
        "kSoftwareCatalogSha256",
    ),
    "software-optimization-catalog": (
        "kSoftwareOptimizationCatalogBytes",
        "kSoftwareOptimizationCatalogSha256",
    ),
}

EXPECTED_RESOURCES = {
    "software-catalog": "catalog/software-catalog.toml",
    "software-optimization-catalog": "catalog/software-optimization-catalog.toml",
}
EXPECTED_ARTIFACTS = {
    "standard-x64-portable",
    "rescue-x64-portable",
    "large-offline-x64-portable",
}

BYTES_PATTERN = re.compile(
    r"constexpr\s+std::uintmax_t\s+(?P<name>k\w+CatalogBytes)\s*="
    r"\s*(?P<value>[0-9]+)\s*;"
)
SHA_PATTERN = re.compile(
    r"constexpr\s+std::array\s*<\s*std::uint8_t\s*,\s*32\s*>\s+"
    r"(?P<name>k\w+CatalogSha256)\s*\{(?P<values>[^}]*)\}\s*;",
    re.DOTALL,
)
SHA_VALUE_PATTERN = re.compile(r"0[xX][0-9a-fA-F]+|[0-9]+")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def read_json(path: Path, context: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ContractFailure(f"{context} is not readable JSON: {error}") from error
    require(isinstance(value, dict), f"{context} must be a JSON object")
    return value


def parse_runtime_locks(source: str) -> dict[str, tuple[int, bytes]]:
    byte_values: dict[str, int] = {}
    for match in BYTES_PATTERN.finditer(source):
        name = match.group("name")
        require(name not in byte_values, f"runtime byte lock {name} is duplicated")
        byte_values[name] = int(match.group("value"))

    sha_values: dict[str, bytes] = {}
    for match in SHA_PATTERN.finditer(source):
        name = match.group("name")
        require(name not in sha_values, f"runtime SHA-256 lock {name} is duplicated")
        tokens = [token for token in SHA_VALUE_PATTERN.findall(match.group("values"))]
        require(len(tokens) == 32, f"runtime SHA-256 lock {name} must contain 32 bytes")
        values = []
        for token in tokens:
            value = int(token, 0)
            require(0 <= value <= 0xFF, f"runtime SHA-256 lock {name} contains an invalid byte")
            values.append(value)
        sha_values[name] = bytes(values)

    parsed: dict[str, tuple[int, bytes]] = {}
    for resource_id, (byte_name, sha_name) in RUNTIME_LOCKS.items():
        require(byte_name in byte_values, f"runtime byte lock {byte_name} is missing")
        require(sha_name in sha_values, f"runtime SHA-256 lock {sha_name} is missing")
        parsed[resource_id] = (byte_values[byte_name], sha_values[sha_name])
    return parsed


def resource_map(content: dict[str, Any], artifact_id: str) -> dict[str, tuple[str, int, str]]:
    resources = content.get("bundledCatalogResources")
    require(isinstance(resources, list), f"artifact {artifact_id} bundledCatalogResources must be an array")
    require(len(resources) == len(EXPECTED_RESOURCES), f"artifact {artifact_id} must declare exactly two catalog resources")

    result: dict[str, tuple[str, int, str]] = {}
    for resource in resources:
        require(isinstance(resource, dict), f"artifact {artifact_id} contains a non-object catalog resource")
        resource_id = resource.get("id")
        require(resource_id in EXPECTED_RESOURCES, f"artifact {artifact_id} contains unexpected catalog resource {resource_id!r}")
        require(resource_id not in result, f"artifact {artifact_id} duplicates catalog resource {resource_id!r}")
        expected_path = EXPECTED_RESOURCES[resource_id]
        require(resource.get("relativePath") == expected_path, f"artifact {artifact_id} has an unexpected relative path for {resource_id}")
        require(resource.get("packagePath") == expected_path, f"artifact {artifact_id} has an unexpected package path for {resource_id}")
        byte_count = resource.get("bytes")
        sha256 = resource.get("sha256")
        require(isinstance(byte_count, int) and not isinstance(byte_count, bool) and byte_count > 0,
                f"artifact {artifact_id} has an invalid byte lock for {resource_id}")
        require(isinstance(sha256, str) and re.fullmatch(r"[0-9a-fA-F]{64}", sha256),
                f"artifact {artifact_id} has an invalid SHA-256 lock for {resource_id}")
        result[resource_id] = (expected_path, byte_count, sha256.lower())

    require(set(result) == set(EXPECTED_RESOURCES), f"artifact {artifact_id} does not contain the fixed catalog resource set")
    return result


def verify(
    root: Path,
    *,
    runtime_source: str | None = None,
    manifest: dict[str, Any] | None = None,
    catalog_bytes: dict[str, bytes] | None = None,
) -> None:
    source_path = root / "src/composition/windows/composition_root.cpp"
    manifest_path = root / "release/artifact-content-manifest.v1.json"
    if runtime_source is None:
        try:
            runtime_source = source_path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise ContractFailure(f"runtime composition source is not readable: {error}") from error
    if manifest is None:
        manifest = read_json(manifest_path, "artifact content manifest")

    runtime_locks = parse_runtime_locks(runtime_source)
    require(manifest.get("schemaVersion") == 1, "artifact content manifest schemaVersion must be 1")
    artifacts = manifest.get("artifacts")
    require(isinstance(artifacts, list), "artifact content manifest artifacts must be an array")
    by_id: dict[str, dict[str, Any]] = {}
    for artifact in artifacts:
        require(isinstance(artifact, dict), "artifact content manifest contains a non-object artifact")
        artifact_id = artifact.get("artifactId")
        require(isinstance(artifact_id, str) and artifact_id not in by_id,
                f"artifact content manifest has a duplicate or invalid artifact id {artifact_id!r}")
        by_id[artifact_id] = artifact
    require(set(by_id) == EXPECTED_ARTIFACTS, "artifact content manifest must contain exactly the three x64 portable artifacts")

    expected_resource_map: dict[str, tuple[str, int, str]] | None = None
    for artifact_id in sorted(EXPECTED_ARTIFACTS):
        artifact = by_id[artifact_id]
        require(artifact.get("packageKind") == "portable", f"artifact {artifact_id} must be portable")
        require(artifact.get("architecture") == "x64", f"artifact {artifact_id} must target x64")
        current = resource_map(artifact, artifact_id)
        if expected_resource_map is None:
            expected_resource_map = current
        else:
            require(current == expected_resource_map,
                    f"artifact {artifact_id} catalog locks differ from the other x64 portable artifacts")

    require(expected_resource_map is not None, "x64 portable catalog resources are missing")
    for resource_id, expected_path in EXPECTED_RESOURCES.items():
        _, manifest_bytes, manifest_sha256 = expected_resource_map[resource_id]
        runtime_bytes, runtime_sha256 = runtime_locks[resource_id]
        require((runtime_bytes, runtime_sha256.hex()) == (manifest_bytes, manifest_sha256),
                f"runtime lock for {resource_id} does not match the release content manifest")

        if catalog_bytes is None:
            source_path = root / expected_path
            try:
                actual = source_path.read_bytes()
            except OSError as error:
                raise ContractFailure(f"catalog source {expected_path} is not readable: {error}") from error
        else:
            actual = catalog_bytes[resource_id]
        actual_sha256 = hashlib.sha256(actual).hexdigest()
        require((len(actual), actual_sha256) == (manifest_bytes, manifest_sha256),
                f"catalog source {expected_path} does not match the release content manifest")


def verify_drift_rejection(root: Path) -> None:
    source_path = root / "src/composition/windows/composition_root.cpp"
    source = source_path.read_text(encoding="utf-8")
    runtime_locks = parse_runtime_locks(source)
    original_bytes = runtime_locks["software-catalog"][0]
    mutated_source = source.replace(
        f"kSoftwareCatalogBytes = {original_bytes};",
        f"kSoftwareCatalogBytes = {original_bytes + 1};",
        1,
    )
    require(mutated_source != source, "runtime drift fixture could not mutate the byte lock")
    try:
        verify(root, runtime_source=mutated_source)
    except ContractFailure:
        pass
    else:
        raise ContractFailure("runtime byte-lock drift was not rejected")

    manifest = read_json(root / "release/artifact-content-manifest.v1.json", "artifact content manifest")
    mutated_manifest = copy.deepcopy(manifest)
    mutated_manifest["artifacts"][0]["bundledCatalogResources"][0]["bytes"] += 1
    try:
        verify(root, manifest=mutated_manifest)
    except ContractFailure:
        pass
    else:
        raise ContractFailure("release manifest byte-lock drift was not rejected")

    actual = {
        resource_id: (root / path).read_bytes()
        for resource_id, path in EXPECTED_RESOURCES.items()
    }
    mutated_catalog = dict(actual)
    mutated_catalog["software-catalog"] = actual["software-catalog"] + b"\n"
    try:
        verify(root, catalog_bytes=mutated_catalog)
    except ContractFailure:
        pass
    else:
        raise ContractFailure("catalog source drift was not rejected")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", required=True, type=Path)
    args = parser.parse_args()
    root = args.repository_root.resolve()
    try:
        verify(root)
        verify_drift_rejection(root)
    except (ContractFailure, OSError) as error:
        print(f"runtime catalog lock contract failed: {error}", file=sys.stderr)
        return 1
    print("runtime catalog lock contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
