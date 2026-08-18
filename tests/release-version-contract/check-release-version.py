#!/usr/bin/env python3
"""Validate the single source and injection path for product versions."""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


APPLICATION_VERSION = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$"
)
NUMERIC_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
WINDOWS_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$")
VERSION_FIELDS = {
    "schemaVersion",
    "applicationVersion",
    "cmakeVersion",
    "windowsVersion",
    "wixVersion",
}


class Contract:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def equal(self, actual: Any, expected: Any, message: str) -> None:
        if actual != expected:
            self.failures.append(f"{message}: expected {expected!r}, got {actual!r}")


def validate_mapping(version: dict[str, Any], contract: Contract) -> None:
    contract.equal(set(version), VERSION_FIELDS, "version source fields drifted")
    contract.equal(version.get("schemaVersion"), 1, "version source schema drifted")
    application = version.get("applicationVersion")
    cmake = version.get("cmakeVersion")
    windows = version.get("windowsVersion")
    wix = version.get("wixVersion")
    contract.require(isinstance(application, str) and bool(APPLICATION_VERSION.fullmatch(application)), "applicationVersion is not a supported semantic version")
    contract.require(isinstance(cmake, str) and bool(NUMERIC_VERSION.fullmatch(cmake)), "cmakeVersion is not a three-part numeric version")
    contract.require(isinstance(windows, str) and bool(WINDOWS_VERSION.fullmatch(windows)), "windowsVersion is not a four-part numeric version")
    contract.require(isinstance(wix, str) and bool(NUMERIC_VERSION.fullmatch(wix)), "wixVersion is not a three-part numeric version")
    if isinstance(application, str) and bool(NUMERIC_VERSION.fullmatch(application)):
        contract.equal(cmake, application, "stable CMake version must derive directly from applicationVersion")
        contract.equal(wix, application, "stable WiX version must derive directly from applicationVersion")
        contract.equal(windows, f"{application}.0", "stable Windows version must derive directly from applicationVersion")


def check_consumers(root: Path, version: dict[str, Any], contract: Contract) -> None:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    contract.require('"${CMAKE_CURRENT_SOURCE_DIR}/release/product-version.json"' in cmake, "CMake does not read the authoritative version source")
    contract.require("string(JSON" in cmake and "AZZS_APPLICATION_VERSION" in cmake, "CMake does not parse applicationVersion")
    contract.require("VERSION ${AZZS_CMAKE_PROJECT_VERSION}" in cmake, "CMake project version is not injected")
    contract.require("configure_file(" in cmake and "app.manifest" in cmake, "CMake does not generate the versioned manifest")

    application_cmake = (root / "src/application/CMakeLists.txt").read_text(encoding="utf-8")
    windows_cmake = (root / "src/adapters/windows/CMakeLists.txt").read_text(encoding="utf-8")
    contract.require('AZZS_APPLICATION_VERSION="${AZZS_APPLICATION_VERSION}"' in application_cmake, "application fallback identity is not injected from CMake")
    contract.require('AZZS_APPLICATION_VERSION="${AZZS_APPLICATION_VERSION}"' in windows_cmake, "Windows update identity is not injected from CMake")

    forbidden_versions = {
        value
        for field in ("applicationVersion", "windowsVersion", "wixVersion")
        if isinstance(value := version.get(field), str) and value
    }
    for relative in (
        "src/application/src/workbench.cpp",
        "src/adapters/windows/src/windows_application_update_platform.cpp",
        "src/adapters/ui/winui/app.rc",
        "src/adapters/ui/winui/app.manifest",
        "installer/Package.wxs",
    ):
        source = (root / relative).read_text(encoding="utf-8")
        for forbidden_version in forbidden_versions:
            contract.require(
                forbidden_version not in source,
                f"hard-coded product version {forbidden_version!r} remains in {relative}",
            )

    manifest_template = (root / "src/adapters/ui/winui/app.manifest").read_text(encoding="utf-8")
    resource_template = (root / "src/adapters/ui/winui/app.rc").read_text(encoding="utf-8")
    contract.require('version="@AZZS_WINDOWS_VERSION@"' in manifest_template, "manifest does not consume the generated Windows version")
    contract.require("FILEVERSION AZZS_WINDOWS_VERSION_COMMAS" in resource_template, "PE file version does not consume the Windows version")
    contract.require('VALUE "ProductVersion", AZZS_APPLICATION_VERSION "\\0"' in resource_template, "PE product version does not consume the application version")

    project = ET.parse(root / "src/adapters/ui/winui/Azzs.WinUI.vcxproj").getroot()
    namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    manifests = project.findall(".//m:Manifest", namespace)
    resources = project.findall(".//m:ResourceCompile", namespace)
    contract.equal([item.attrib.get("Include") for item in manifests], ["$(AzzsApplicationManifest)"], "WinUI must consume only the generated manifest")
    contract.equal(
        [item.attrib.get("Include") for item in resources if item.attrib.get("Include")],
        ["app.rc"],
        "WinUI PE resource registration drifted",
    )
    definitions = project.find(".//m:ResourceCompile/m:PreprocessorDefinitions", namespace)
    contract.require(definitions is not None and "AZZS_APPLICATION_VERSION" in (definitions.text or "") and "AZZS_WINDOWS_VERSION_COMMAS" in (definitions.text or ""), "WinUI PE resource does not receive the version properties")
    targets = [item.attrib.get("Name") for item in project.findall(".//m:Target", namespace)]
    contract.require("ValidateAzzsVersionInputs" in targets, "WinUI project does not fail closed without versioned build inputs")

    build_script = (root / "eng/build.ps1").read_text(encoding="utf-8")
    contract.require("release/product-version.json" in build_script, "build entry does not read the authoritative version source")
    for property_name in ("AzzsGeneratedResourceDirectory", "AzzsApplicationVersion", "AzzsWindowsVersion", "AzzsWindowsVersionCommas"):
        contract.require(f"/p:{property_name}=" in build_script, f"build entry does not pass {property_name}")

    wix = ET.parse(root / "installer/Package.wxs").getroot()
    wix_namespace = {"w": "http://wixtoolset.org/schemas/v4/wxs"}
    package = wix.find("w:Package", wix_namespace)
    contract.require(package is not None and package.attrib.get("Version") == "$(var.AzzsWixVersion)", "WiX package version is not injected")
    wix_project = (root / "installer/Azzs.Installer.wixproj").read_text(encoding="utf-8")
    package_script = (root / "eng/package-installer.ps1").read_text(encoding="utf-8")
    contract.require("AzzsWixVersion=$(AzzsWixVersion)" in wix_project, "WiX project does not forward AzzsWixVersion")
    contract.require("release/product-version.json" in package_script and "/p:AzzsWixVersion=" in package_script, "installer entry does not inject the authoritative WiX version")
    contract.require("Test-PortableBuildManifest" in package_script, "installer entry no longer gates WiX on the current build manifest")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", required=True, type=Path)
    arguments = parser.parse_args()
    root = arguments.repository_root.resolve()
    contract = Contract()
    version: dict[str, Any] = {}
    source_path = root / "release/product-version.json"
    contract.require(source_path.is_file(), "missing release/product-version.json")
    if source_path.is_file():
        try:
            version = json.loads(source_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            contract.require(False, f"version source is not valid JSON: {error}")
        else:
            validate_mapping(version, contract)
    beta_mapping_contract = Contract()
    validate_mapping(
        {
            "schemaVersion": 1,
            "applicationVersion": "0.9.0-beta.1",
            "cmakeVersion": "0.9.0",
            "windowsVersion": "0.9.0.42",
            "wixVersion": "0.9.0",
        },
        beta_mapping_contract,
    )
    contract.require(
        not beta_mapping_contract.failures,
        "explicit prerelease version mappings must remain supported: "
        + "; ".join(beta_mapping_contract.failures),
    )
    check_consumers(root, version, contract)
    if contract.failures:
        for failure in contract.failures:
            print(f"release version contract: FAIL: {failure}", file=sys.stderr)
        return 1
    print("release version contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
