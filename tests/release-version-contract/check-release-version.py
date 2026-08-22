#!/usr/bin/env python3
"""Validate the single source and injection path for product versions."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


APPLICATION_VERSION = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$"
)
NUMERIC_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
WINDOWS_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$")
RELEASE_CHANNELS = {"stable", "prerelease"}
VERSION_FIELDS = {
    "schemaVersion",
    "applicationVersion",
    "releaseChannel",
    "cmakeVersion",
    "windowsVersion",
    "wixVersion",
}
MSBUILD_CONTRACT_SKIP = 77
SUBPROCESS_OUTPUT_TAIL_LIMIT = 1200
WINDOWS_PATH = re.compile(r"(?i)(?:[A-Z]:[\\/]|\\\\)[^\r\n]+")
POSIX_PATH = re.compile(r"(?<![A-Za-z0-9])/(?:[^\r\n]+)")


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
    channel = version.get("releaseChannel")
    cmake = version.get("cmakeVersion")
    windows = version.get("windowsVersion")
    wix = version.get("wixVersion")
    contract.require(isinstance(application, str) and bool(APPLICATION_VERSION.fullmatch(application)), "applicationVersion is not a supported semantic version")
    contract.require(isinstance(channel, str) and channel in RELEASE_CHANNELS, "releaseChannel must be stable or prerelease")
    contract.require(isinstance(cmake, str) and bool(NUMERIC_VERSION.fullmatch(cmake)), "cmakeVersion is not a three-part numeric version")
    contract.require(isinstance(windows, str) and bool(WINDOWS_VERSION.fullmatch(windows)), "windowsVersion is not a four-part numeric version")
    contract.require(isinstance(wix, str) and bool(NUMERIC_VERSION.fullmatch(wix)), "wixVersion is not a three-part numeric version")
    if isinstance(application, str) and "-" in application:
        contract.equal(channel, "prerelease", "a semantic prerelease applicationVersion requires releaseChannel prerelease")
    if isinstance(application, str) and bool(NUMERIC_VERSION.fullmatch(application)):
        contract.equal(cmake, application, "numeric CMake version must derive directly from applicationVersion")
        contract.equal(wix, application, "numeric WiX version must derive directly from applicationVersion")
        contract.equal(windows, f"{application}.0", "numeric Windows version must derive directly from applicationVersion")


def check_consumers(root: Path, version: dict[str, Any], contract: Contract) -> None:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    contract.require('"${CMAKE_CURRENT_SOURCE_DIR}/release/product-version.json"' in cmake, "CMake does not read the authoritative version source")
    contract.require("string(JSON" in cmake and "AZZS_APPLICATION_VERSION" in cmake and "AZZS_APPLICATION_RELEASE_CHANNEL" in cmake, "CMake does not parse the application version and release channel")
    contract.require("VERSION ${AZZS_CMAKE_PROJECT_VERSION}" in cmake, "CMake project version is not injected")
    contract.require("configure_file(" in cmake and "app.manifest" in cmake, "CMake does not generate the versioned manifest")

    application_cmake = (root / "src/application/CMakeLists.txt").read_text(encoding="utf-8")
    windows_cmake = (root / "src/adapters/windows/CMakeLists.txt").read_text(encoding="utf-8")
    contract.require('AZZS_APPLICATION_VERSION="${AZZS_APPLICATION_VERSION}"' in application_cmake and 'AZZS_APPLICATION_RELEASE_CHANNEL="${AZZS_APPLICATION_RELEASE_CHANNEL}"' in application_cmake, "application fallback identity is not injected from CMake")
    contract.require('AZZS_APPLICATION_VERSION="${AZZS_APPLICATION_VERSION}"' in windows_cmake and 'AZZS_APPLICATION_RELEASE_CHANNEL="${AZZS_APPLICATION_RELEASE_CHANNEL}"' in windows_cmake, "Windows update identity is not injected from CMake")

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

    for relative in (
        "src/application/src/workbench.cpp",
        "src/adapters/windows/src/windows_application_update_platform.cpp",
    ):
        source = (root / relative).read_text(encoding="utf-8")
        contract.require(
            "AZZS_APPLICATION_VERSION" in source
            and "AZZS_APPLICATION_RELEASE_CHANNEL" in source,
            f"{relative} does not consume the injected version and release channel",
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

    resource_contract = root / "tests/release-version-contract"
    contract.require(
        (resource_contract / "resource-macro.rc").is_file()
        and (resource_contract / "msbuild-property-capture.proj").is_file(),
        "MSBuild resource contract probe files are missing",
    )

    build_script = (root / "eng/build.ps1").read_text(encoding="utf-8")
    contract.require("release/product-version.json" in build_script, "build entry does not read the authoritative version source")
    contract.require("releaseChannel" in build_script, "build entry does not validate the authoritative release channel")
    contract.require(
        "msbuild-arguments.ps1" in build_script
        and "ConvertTo-AzzsWindowsVersionCommasArgument" in build_script,
        "build entry does not use the MSBuild Windows version argument encoder",
    )
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


def powershell_executable() -> str | None:
    return shutil.which("pwsh") or shutil.which("powershell")


def sanitized_output_tail(output: str, limit: int = SUBPROCESS_OUTPUT_TAIL_LIMIT) -> str:
    sanitized = WINDOWS_PATH.sub("<path>", output.replace("\r\n", "\n")).strip()
    sanitized = POSIX_PATH.sub("<path>", sanitized)
    return sanitized[-limit:] if sanitized else "<empty>"


def powershell_diagnostic(result: subprocess.CompletedProcess[str]) -> str:
    return (
        "PowerShell subprocess diagnostic: "
        f"returncode={result.returncode}; "
        f"stdout_tail={sanitized_output_tail(result.stdout)!r}; "
        f"stderr_tail={sanitized_output_tail(result.stderr)!r}"
    )


def run_powershell_helper(
    powershell_command: str,
    helper: Path,
    windows_version: str,
    environment: dict[str, str],
) -> subprocess.CompletedProcess[str]:
    escaped_helper_path = str(helper).replace("'", "''")
    escaped_version = windows_version.replace("'", "''")
    return subprocess.run(
        [
            powershell_command,
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "& { . '"
            + escaped_helper_path
            + "'; ConvertTo-AzzsWindowsVersionCommasArgument -WindowsVersion '"
            + escaped_version
            + "' }",
        ],
        cwd=helper.parents[1],
        env=environment,
        capture_output=True,
        check=False,
        text=True,
        errors="replace",
    )


def discover_resource_compiler(explicit: str | None) -> str | None:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    for command in ("rc.exe", "rc"):
        located = shutil.which(command)
        if located:
            candidates.append(Path(located))

    sdk_root = os.environ.get("WindowsSdkDir")
    if sdk_root:
        sdk_path: Path | None = Path(sdk_root)
    else:
        program_files_x86 = os.environ.get("ProgramFiles(x86)")
        sdk_path = (
            Path(program_files_x86) / "Windows Kits" / "10"
            if program_files_x86
            else None
        )
    sdk_version = os.environ.get("WindowsSDKVersion", "").strip("\\/")
    if sdk_path:
        if sdk_version:
            candidates.append(sdk_path / "bin" / sdk_version / "x64" / "rc.exe")
        bin_path = sdk_path / "bin"
        if bin_path.is_dir():
            for version_path in sorted(bin_path.iterdir(), reverse=True):
                candidates.append(version_path / "x64" / "rc.exe")

    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return None


def validate_powershell_helper(
    root: Path,
    powershell_command: str | None,
    required: bool,
    contract: Contract,
) -> None:
    helper = root / "eng/msbuild-arguments.ps1"
    contract.require(helper.is_file(), "missing MSBuild Windows version argument encoder")
    if not helper.is_file():
        return
    helper_source = helper.read_text(encoding="utf-8")
    contract.require(
        "throw" in helper_source
        and "notmatch" in helper_source
        and "WindowsVersion" in helper_source,
        "MSBuild Windows version argument encoder must fail closed in its production function",
    )
    if powershell_command is None:
        if required:
            contract.require(False, "MSBuild Windows version argument contract requires PowerShell")
        return

    environment = os.environ.copy()
    valid = run_powershell_helper(
        powershell_command, helper, "0.1.0.0", environment
    )
    contract.require(
        valid.returncode == 0,
        "MSBuild Windows version argument encoder failed: "
        + powershell_diagnostic(valid),
    )
    contract.require(
        valid.stdout.strip() == "0%2c1%2c0%2c0",
        "0.1.0.0 must be encoded for an MSBuild property argument; "
        + powershell_diagnostic(valid),
    )

    invalid_inputs = (
        ("missing segment", "0.1.0"),
        ("extra segment", "0.1.0.0.1"),
        ("non-numeric segment", "0.one.0.0"),
        ("pre-encoded commas", "0%2c1%2c0%2c0"),
    )
    for description, value in invalid_inputs:
        result = run_powershell_helper(
            powershell_command, helper, value, environment
        )
        contract.require(
            result.returncode != 0,
            f"MSBuild Windows version argument encoder accepted {description}: {value!r}; "
            + powershell_diagnostic(result),
        )
        contract.require(
            result.stdout.strip() == ""
            and "0%2c1%2c0%2c0" not in result.stderr,
            f"MSBuild Windows version argument encoder emitted a misleading value for {description}; "
            + powershell_diagnostic(result),
        )


def validate_build_rejects_invalid_version_mappings(
    root: Path,
    powershell_command: str | None,
    version: dict[str, Any],
    contract: Contract,
) -> None:
    if powershell_command is None:
        return

    output_parent = root / "out"
    fixture_root: Path | None = None
    invalid_mappings = (
        ("invalid four-part Windows version", {"windowsVersion": "0.1.0"}),
        ("newline-terminated application version", {"applicationVersion": "0.1.0\n"}),
        ("newline-terminated Windows version", {"windowsVersion": "0.1.0.0\n"}),
        ("case-mismatched release channel", {"releaseChannel": "PRErelease"}),
        ("newline-terminated release channel", {"releaseChannel": "prerelease\n"}),
        ("non-string release channel", {"releaseChannel": ["prerelease"]}),
    )
    for description, changes in invalid_mappings:
        fixture_root = None
        invalid_version = dict(version)
        invalid_version.update(changes)
        try:
            output_parent.mkdir(parents=True, exist_ok=True)
            fixture_root = Path(
                tempfile.mkdtemp(prefix="release-version-powershell-repo-", dir=output_parent)
            )
            (fixture_root / "eng").mkdir()
            (fixture_root / "release").mkdir()
            for relative in (
                "eng/build.ps1",
                "eng/portable-artifact-content.ps1",
                "eng/msbuild-arguments.ps1",
            ):
                destination = fixture_root / relative
                shutil.copy2(root / relative, destination)
            source_path = fixture_root / "release/product-version.json"
            build_script = fixture_root / "eng/build.ps1"
            source_path.write_text(
                json.dumps(invalid_version, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment["ProgramFiles(x86)"] = str(
                fixture_root / "missing-program-files-x86"
            )
            result = subprocess.run(
                [
                    powershell_command,
                    "-NoLogo",
                    "-NoProfile",
                    "-NonInteractive",
                    "-File",
                    str(build_script),
                    "-Architecture",
                    "x64",
                    "-SkipCoreSmoke",
                ],
                cwd=fixture_root,
                env=environment,
                capture_output=True,
                check=False,
                text=True,
                errors="replace",
            )
            output = result.stdout + result.stderr
            contract.require(
                result.returncode != 0,
                f"build.ps1 accepted {description}; " + powershell_diagnostic(result),
            )
            contract.require(
                re.search(r"unsupported\s+version\s+mapping", output, re.IGNORECASE)
                is not None,
                f"build.ps1 did not reject {description} at its version gate; "
                + powershell_diagnostic(result),
            )
            contract.require(
                "vswhere.exe was not found" not in output.lower(),
                f"build.ps1 reached toolchain discovery after rejecting {description}; "
                + powershell_diagnostic(result),
            )
        except OSError as error:
            contract.require(False, f"build.ps1 invalid-input contract failed: {error}")
        finally:
            if fixture_root is not None:
                shutil.rmtree(fixture_root, ignore_errors=True)


def validate_build_accepts_suffixless_prerelease(
    root: Path, powershell_command: str | None, contract: Contract
) -> None:
    if powershell_command is None:
        return

    output_parent = root / "out"
    fixture_root: Path | None = None
    suffixless_prerelease = {
        "schemaVersion": 1,
        "applicationVersion": "0.1.0",
        "releaseChannel": "prerelease",
        "cmakeVersion": "0.1.0",
        "windowsVersion": "0.1.0.0",
        "wixVersion": "0.1.0",
    }
    try:
        output_parent.mkdir(parents=True, exist_ok=True)
        fixture_root = Path(
            tempfile.mkdtemp(prefix="release-version-powershell-repo-", dir=output_parent)
        )
        (fixture_root / "eng").mkdir()
        (fixture_root / "release").mkdir()
        for relative in (
            "eng/build.ps1",
            "eng/portable-artifact-content.ps1",
            "eng/msbuild-arguments.ps1",
        ):
            destination = fixture_root / relative
            shutil.copy2(root / relative, destination)
        source_path = fixture_root / "release/product-version.json"
        build_script = fixture_root / "eng/build.ps1"
        source_path.write_text(
            json.dumps(suffixless_prerelease, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        environment = os.environ.copy()
        environment["ProgramFiles(x86)"] = str(
            fixture_root / "missing-program-files-x86"
        )
        result = subprocess.run(
            [
                powershell_command,
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-File",
                str(build_script),
                "-Architecture",
                "x64",
                "-SkipCoreSmoke",
            ],
            cwd=fixture_root,
            env=environment,
            capture_output=True,
            check=False,
            text=True,
            errors="replace",
        )
        output = result.stdout + result.stderr
        contract.require(
            result.returncode != 0,
            "build.ps1 unexpectedly completed with a suffixless prerelease version; "
            + powershell_diagnostic(result),
        )
        contract.require(
            "vswhere.exe was not found" in output.lower(),
            "build.ps1 did not pass the suffixless prerelease version gate before the controlled toolchain stop; "
            + powershell_diagnostic(result),
        )
        contract.require(
            "prerelease application version requires" not in output.lower()
            and "unsupported version mapping" not in output.lower(),
            "build.ps1 rejected a suffixless prerelease version at its version gate; "
            + powershell_diagnostic(result),
        )
    except OSError as error:
        contract.require(False, f"build.ps1 suffixless-prerelease contract failed: {error}")
    finally:
        if fixture_root is not None:
            shutil.rmtree(fixture_root, ignore_errors=True)


def validate_msbuild_windows_version_argument(
    root: Path,
    msbuild_command: str | None,
    resource_compiler: str | None,
    contract: Contract,
) -> bool:
    powershell_command = powershell_executable()
    validate_powershell_helper(
        root, powershell_command, bool(msbuild_command), contract
    )
    if not msbuild_command:
        return True

    capture_project = root / "tests/release-version-contract/msbuild-property-capture.proj"
    contract.require(capture_project.is_file(), "missing MSBuild Windows version capture project")
    if not capture_project.is_file() or powershell_command is None:
        return False

    resolved_resource_compiler = discover_resource_compiler(resource_compiler)
    contract.require(
        resolved_resource_compiler is not None,
        "MSBuild Windows version resource contract requires rc.exe",
    )
    if resolved_resource_compiler is None:
        return False

    output_parent = root / "out"
    test_directory: Path | None = None
    try:
        output_parent.mkdir(parents=True, exist_ok=True)
        test_directory = Path(
            tempfile.mkdtemp(prefix="release-version-msbuild-", dir=output_parent)
        )
        capture_path = test_directory / "captured-windows-version.txt"
        environment = os.environ.copy()
        environment["TEMP"] = str(test_directory)
        environment["TMP"] = str(test_directory)
        encoded = run_powershell_helper(
            powershell_command,
            root / "eng/msbuild-arguments.ps1",
            "0.1.0.0",
            environment,
        )
        encoded_output = encoded.stdout.strip()
        if encoded.returncode != 0 or encoded_output != "0%2c1%2c0%2c0":
            contract.require(
                False,
                "MSBuild Windows version argument encoder did not produce the expected value; "
                + powershell_diagnostic(encoded),
            )
            return

        resource_output_path = test_directory / "resource"
        captured = subprocess.run(
            [
                msbuild_command,
                str(capture_project),
                "/nologo",
                "/t:Capture",
                f"/p:AzzsCapturePath={capture_path}",
                f"/p:AzzsWindowsVersionCommas={encoded_output}",
                f"/p:AzzsResourceCompiler={resolved_resource_compiler}",
                f"/p:AzzsResourceOutputPath={resource_output_path}",
            ],
            cwd=root,
            env=environment,
            capture_output=True,
            check=False,
            text=True,
            errors="replace",
        )
        contract.require(
            captured.returncode == 0,
            "MSBuild rejected the encoded Windows version property: "
            + (captured.stdout + captured.stderr).strip()[-1200:],
        )
        contract.require(
            capture_path.is_file(),
            "MSBuild did not write the captured Windows version property",
        )
        if captured.returncode == 0 and capture_path.is_file():
            contract.equal(
                capture_path.read_text(encoding="utf-8").strip(),
                "0,1,0,0",
                "MSBuild must receive 0,1,0,0 after command-line parsing",
            )
        resource_path = resource_output_path / "version-probe.res"
        contract.require(
            resource_path.is_file(),
            "MSBuild did not produce the compiled Windows version resource",
        )
        if captured.returncode == 0 and resource_path.is_file():
            resource_bytes = resource_path.read_bytes()
            resource_text = resource_bytes.decode("utf-16le", errors="ignore")
            contract.require(
                "0,1,0,0" in resource_text,
                "compiled Windows version resource does not contain 0,1,0,0",
            )
            for forbidden in ("%2c", '"', "\\"):
                contract.require(
                    forbidden not in resource_text and forbidden.encode("ascii") not in resource_bytes,
                    f"compiled Windows version resource contains forbidden token {forbidden!r}",
                )
    except OSError as error:
        contract.require(False, f"MSBuild Windows version argument contract failed: {error}")
    finally:
        if test_directory is not None:
            shutil.rmtree(test_directory, ignore_errors=True)


def target_defines(build_directory: Path, target_name: str) -> set[str] | None:
    reply_directory = build_directory / ".cmake/api/v1/reply"
    indexes = sorted(reply_directory.glob("index-*.json"))
    if not indexes:
        return None
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    codemodel_reply = index.get("reply", {}).get("codemodel-v2")
    if not isinstance(codemodel_reply, dict):
        return None
    codemodel = json.loads(
        (reply_directory / codemodel_reply["jsonFile"]).read_text(encoding="utf-8")
    )
    for configuration in codemodel.get("configurations", []):
        for target in configuration.get("targets", []):
            if target.get("name") != target_name:
                continue
            details = json.loads(
                (reply_directory / target["jsonFile"]).read_text(encoding="utf-8")
            )
            return {
                definition["define"].replace('\\"', '"')
                for group in details.get("compileGroups", [])
                for definition in group.get("defines", [])
                if isinstance(definition.get("define"), str)
            }
    return None


def validate_mutated_cmake_consumers(
    root: Path, cmake_command: str, version: dict[str, Any], contract: Contract
) -> None:
    output_parent = root / "out"
    fixture_root: Path | None = None
    build_directory: Path | None = None
    description = f"{version['applicationVersion']} ({version['releaseChannel']})"
    try:
        output_parent.mkdir(parents=True, exist_ok=True)
        fixture_root = Path(
            tempfile.mkdtemp(prefix="release-version-cmake-repo-", dir=output_parent)
        )
        shutil.copytree(
            root,
            fixture_root,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns(".git", ".gitnexus", ".scratch", "out"),
        )
        source_path = fixture_root / "release/product-version.json"
        build_directory = Path(
            tempfile.mkdtemp(prefix="release-version-contract-", dir=output_parent)
        )
        query_directory = build_directory / ".cmake/api/v1/query"
        query_directory.mkdir(parents=True, exist_ok=True)
        (query_directory / "codemodel-v2").touch()
        version_observation = build_directory / "observe-product-version.cmake"
        version_observation.write_text(
            'file(WRITE "${CMAKE_BINARY_DIR}/product-version-observation.txt" "${CMAKE_PROJECT_VERSION}")\n',
            encoding="utf-8",
        )
        source_path.write_text(
            json.dumps(version, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        result = subprocess.run(
            [
                cmake_command,
                "-S",
                str(fixture_root),
                "-B",
                str(build_directory),
                "-G",
                "Visual Studio 18 2026",
                "-A",
                "x64",
                "-T",
                "v145",
                "-DCMAKE_SYSTEM_VERSION=10.0.28000.0",
                f"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={version_observation}",
                "-DBUILD_TESTING=OFF",
            ],
            cwd=fixture_root,
            capture_output=True,
            check=False,
            text=True,
        )
        output = (result.stdout + result.stderr).strip()
        contract.require(
            result.returncode == 0,
            f"mutated {description} CMake configuration failed: {output[-1200:]}",
        )
        if result.returncode != 0:
            return

        manifest = build_directory / "generated/winui/app.manifest"
        contract.require(
            manifest.is_file()
            and f'version="{version["windowsVersion"]}"'
            in manifest.read_text(encoding="utf-8"),
            f"mutated {description} Windows manifest did not derive from product-version.json",
        )
        contract.equal(
            (build_directory / "product-version-observation.txt").read_text(
                encoding="utf-8"
            ),
            version["cmakeVersion"],
            f"mutated {description} CMake project version did not derive from product-version.json",
        )
        expected = {
            f'AZZS_APPLICATION_VERSION="{version["applicationVersion"]}"',
            f'AZZS_APPLICATION_RELEASE_CHANNEL="{version["releaseChannel"]}"',
        }
        for target, relative in (
            ("azzs_application", "src/application/src/workbench.cpp"),
            (
                "azzs_windows_adapter",
                "src/adapters/windows/src/windows_application_update_platform.cpp",
            ),
        ):
            defines = target_defines(build_directory, target)
            contract.require(
                defines is not None and expected.issubset(defines),
                f"mutated {description} did not inject version and channel into {relative} via {target}",
            )
    except (OSError, json.JSONDecodeError) as error:
        contract.require(False, f"mutated {description} consumer check failed: {error}")
    finally:
        if build_directory is not None:
            shutil.rmtree(build_directory, ignore_errors=True)
        if fixture_root is not None:
            shutil.rmtree(fixture_root, ignore_errors=True)


def mutation_contract(root: Path, cmake_command: str, contract: Contract) -> None:
    for version in (
        {
            "schemaVersion": 1,
            "applicationVersion": "7.13.17",
            "releaseChannel": "stable",
            "cmakeVersion": "7.13.17",
            "windowsVersion": "7.13.17.0",
            "wixVersion": "7.13.17",
        },
        {
            "schemaVersion": 1,
            "applicationVersion": "7.13.17-beta.23",
            "releaseChannel": "prerelease",
            "cmakeVersion": "7.13.17",
            "windowsVersion": "7.13.17.29",
            "wixVersion": "7.13.17",
        },
        {
            "schemaVersion": 1,
            "applicationVersion": "7.13.17",
            "releaseChannel": "prerelease",
            "cmakeVersion": "7.13.17",
            "windowsVersion": "7.13.17.0",
            "wixVersion": "7.13.17",
        },
    ):
        validate_mapping(version, contract)
        validate_mutated_cmake_consumers(root, cmake_command, version, contract)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", required=True, type=Path)
    parser.add_argument("--cmake-command", default="cmake")
    parser.add_argument("--msbuild-command")
    parser.add_argument("--resource-compiler")
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
            "releaseChannel": "prerelease",
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
    stable_semantic_prerelease_mapping_contract = Contract()
    validate_mapping(
        {
            "schemaVersion": 1,
            "applicationVersion": "0.9.0-beta.1",
            "releaseChannel": "stable",
            "cmakeVersion": "0.9.0",
            "windowsVersion": "0.9.0.42",
            "wixVersion": "0.9.0",
        },
        stable_semantic_prerelease_mapping_contract,
    )
    contract.require(
        bool(stable_semantic_prerelease_mapping_contract.failures),
        "a stable channel with a semantic prerelease applicationVersion must be rejected",
    )
    suffixless_prerelease_mapping_contract = Contract()
    validate_mapping(
        {
            "schemaVersion": 1,
            "applicationVersion": "0.1.0",
            "releaseChannel": "prerelease",
            "cmakeVersion": "0.1.0",
            "windowsVersion": "0.1.0.0",
            "wixVersion": "0.1.0",
        },
        suffixless_prerelease_mapping_contract,
    )
    contract.require(
        not suffixless_prerelease_mapping_contract.failures,
        "suffixless prerelease version mappings must remain supported: "
        + "; ".join(suffixless_prerelease_mapping_contract.failures),
    )
    check_consumers(root, version, contract)
    validate_build_rejects_invalid_version_mappings(
        root, powershell_executable(), version, contract
    )
    validate_build_accepts_suffixless_prerelease(
        root, powershell_executable(), contract
    )
    msbuild_contract_skipped = validate_msbuild_windows_version_argument(
        root,
        arguments.msbuild_command,
        arguments.resource_compiler,
        contract,
    )
    # Visual Studio mutation requires the real MSBuild capability passed by the
    # Visual Studio generator; Ninja and other hosts keep the static checks and
    # skip this Windows-only configure probe.
    if source_path.is_file() and bool(arguments.msbuild_command):
        mutation_contract(root, arguments.cmake_command, contract)
    if contract.failures:
        for failure in contract.failures:
            print(f"release version contract: FAIL: {failure}", file=sys.stderr)
        return 1
    if msbuild_contract_skipped:
        print(
            "release version contract: SKIP (MSBuild/rc.exe resource contract "
            "requires a Visual Studio generator)",
            file=sys.stderr,
        )
        return MSBUILD_CONTRACT_SKIP
    print("release version contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
