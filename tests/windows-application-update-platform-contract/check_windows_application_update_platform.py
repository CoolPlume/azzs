"""Contract for the WinRT JSON projection used by the update adapter."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


class ContractFailure(Exception):
    """A repository contract is not satisfied."""


SOURCE_RELATIVE_PATH = Path(
    "src/adapters/windows/src/windows_application_update_platform.cpp"
)
JSON_OBJECT_PROJECTION = re.compile(
    r"\.as\s*<\s*winrt::Windows::Data::Json::JsonObject\s*>"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def read_source(root: Path) -> str:
    path = root / SOURCE_RELATIVE_PATH
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ContractFailure(f"update adapter source is not readable: {error}") from error


def verify(root: Path, *, source: str | None = None) -> None:
    source = read_source(root) if source is None else source
    require(
        source.count("root.GetObjectAt(index)") == 1,
        "release JSON objects must use JsonArray::GetObjectAt",
    )
    require(
        source.count("assets.GetObjectAt(asset_index)") == 1,
        "asset JSON objects must use JsonArray::GetObjectAt",
    )
    require(
        not JSON_OBJECT_PROJECTION.search(source),
        "update JSON parsing must not use IJsonValue::as<JsonObject>()",
    )


def verify_drift_rejection(root: Path) -> None:
    source = read_source(root)
    replacement = "root.GetAt(index).as<winrt::Windows::Data::Json::JsonObject>()"
    mutated = source.replace("root.GetObjectAt(index)", replacement, 1)
    require(mutated != source, "release projection fixture could not be mutated")
    try:
        verify(root, source=mutated)
    except ContractFailure:
        return
    raise ContractFailure("the contract accepted an IJsonValue-to-JsonObject cast")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", required=True, type=Path)
    args = parser.parse_args()
    root = args.repository_root.resolve()
    try:
        verify(root)
        verify_drift_rejection(root)
    except ContractFailure as error:
        print(f"Windows application update platform contract failed: {error}", file=sys.stderr)
        return 1
    print("Windows application update platform contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
