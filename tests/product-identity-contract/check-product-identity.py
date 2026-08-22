#!/usr/bin/env python3
"""Validate the product identity, release copy, and asset consumption contract."""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath
from typing import Any


EXPECTED_NAME = "Windows 初装工作台"
EXPECTED_DESCRIPTION = (
    "Windows 初装工作台：帮助新装 Windows 完成驱动准备、系统优化、"
    "常用软件安装和软件优化。"
)
EXPECTED_ARTIFACTS = [
    (
        "standard-x64-portable",
        "standard",
        "portable",
        "x64",
        "artifact.standard.x64.portable",
        "{{ASSET_STANDARD_X64_PORTABLE}}",
    ),
    (
        "standard-arm64-portable",
        "standard",
        "portable",
        "ARM64",
        "artifact.standard.arm64.portable",
        "{{ASSET_STANDARD_ARM64_PORTABLE}}",
    ),
    (
        "standard-x64-machine-installer",
        "standard",
        "machine-installer",
        "x64",
        "artifact.standard.x64.machine-installer",
        "{{ASSET_STANDARD_X64_MACHINE_INSTALLER}}",
    ),
    (
        "standard-arm64-machine-installer",
        "standard",
        "machine-installer",
        "ARM64",
        "artifact.standard.arm64.machine-installer",
        "{{ASSET_STANDARD_ARM64_MACHINE_INSTALLER}}",
    ),
    (
        "rescue-x64-portable",
        "rescue",
        "portable",
        "x64",
        "artifact.rescue.x64.portable",
        "{{ASSET_RESCUE_X64_PORTABLE}}",
    ),
    (
        "rescue-arm64-portable",
        "rescue",
        "portable",
        "ARM64",
        "artifact.rescue.arm64.portable",
        "{{ASSET_RESCUE_ARM64_PORTABLE}}",
    ),
    (
        "large-offline-x64-portable",
        "large-offline",
        "portable",
        "x64",
        "artifact.large-offline.x64.portable",
        "{{ASSET_LARGE_OFFLINE_X64_PORTABLE}}",
    ),
    (
        "large-offline-arm64-portable",
        "large-offline",
        "portable",
        "ARM64",
        "artifact.large-offline.arm64.portable",
        "{{ASSET_LARGE_OFFLINE_ARM64_PORTABLE}}",
    ),
]
EXPECTED_DISPLAY_NAMES = {
    "artifact.standard.x64.portable": "Windows 初装工作台 标准版 x64 便携版",
    "artifact.standard.arm64.portable": "Windows 初装工作台 标准版 ARM64 便携版",
    "artifact.standard.x64.machine-installer": "Windows 初装工作台 标准版 x64 机器级安装版",
    "artifact.standard.arm64.machine-installer": "Windows 初装工作台 标准版 ARM64 机器级安装版",
    "artifact.rescue.x64.portable": "Windows 初装工作台 断网救援版 x64 便携版",
    "artifact.rescue.arm64.portable": "Windows 初装工作台 断网救援版 ARM64 便携版",
    "artifact.large-offline.x64.portable": "Windows 初装工作台 超大离线版 x64 便携版",
    "artifact.large-offline.arm64.portable": "Windows 初装工作台 超大离线版 ARM64 便携版",
}
EXPECTED_GRAPHIC_RULES = [
    "简洁设备准备图形",
    "不包含文字",
    "不包含第三方商标",
    "不加载外部图像、字体、脚本或运行时资源",
]
EXPECTED_MINIMUM_VERSION = {
    "target": "Windows 10 22H2",
    "copy": (
        "Windows 10 22H2 是最低目标版本，并同时面向 Windows 11；"
        "这不是跨版本兼容性保证。更早版本会显示风险警告并允许继续运行，"
        "但不提供正常运行保证，实际缺失的系统能力可能使具体功能不可用。"
    ),
}
EXPECTED_RISK_COPY = {
    "unsigned": "本项目提供的 Windows 应用发行包未签名。",
    "verification": (
        "项目不提供发布者或文件完整性校验信息，也不提供 SHA-256 校验值、"
        "源码提交关联或 provenance。"
    ),
    "smartScreen": "下载或启动时可能出现 SmartScreen 提示。",
    "thirdParty": (
        "第三方来源未获项目验证；工作台记录来源、执行与结果事实，"
        "不表示来源、文件或安全已经验证。"
    ),
}
EXPECTED_TEMPLATE_SKELETON_LINES = [
    "# {{PRODUCT_DISPLAY_NAME}} {{VERSION}}",
    "",
    "{{PRODUCT_DESCRIPTION}}",
    "",
    "<!-- 发布时替换双花括号占位符；删除未发布的占位说明后再创建 GitHub Release。 -->",
    "",
    "## 本次发行",
    "",
    "- 版本：`{{VERSION}}`",
    "- 发布日期：`{{RELEASE_DATE}}`",
    "- 发行通道：`{{RELEASE_CHANNEL}}`",
    "- 主要变化：{{CHANGE_SUMMARY}}",
    "- 已知问题：{{KNOWN_ISSUES}}",
    "",
    "## 下载选择",
    "",
    "请根据 Windows 设备架构和需要的发行形态选择制品。x64 默认优先展示；ARM64 Windows 应使用 ARM64 工作台包。实际携带内容以本次 Release 的制品清单为准。",
    "",
    "| 制品 | 下载 |",
    "| --- | --- |",
    "{{ARTIFACT_ROWS}}",
    "",
    "## 使用前请注意",
    "",
    "{{RISK_UNSIGNED}}",
    "",
    "{{RISK_VERIFICATION}}",
    "",
    "{{RISK_SMARTSCREEN}}",
    "",
    "{{RISK_THIRD_PARTY}}",
    "",
    "{{MINIMUM_VERSION_COPY}}",
    "",
    "## 安装与启动",
    "",
    "{{INSTALLATION_AND_STARTUP_NOTES}}",
    "",
    "## 反馈",
    "",
    "请在 GitHub Issues 中说明所用版本、发行形态、架构、Windows 版本、复现步骤和可公开的错误信息；请勿提交密码、验证码、付款信息或其他敏感资料。",
]


class Contract:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def equal(self, actual: Any, expected: Any, message: str) -> None:
        if actual != expected:
            self.failures.append(f"{message}: expected {expected!r}, got {actual!r}")


def repository_path(root: Path, relative: str, contract: Contract) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        contract.require(
            False,
            f"path must use a non-empty repository-relative POSIX path: {relative!r}",
        )
        return root / "__invalid_contract_path__"
    candidate = PurePosixPath(relative)
    invalid = (
        candidate.is_absolute()
        or candidate == PurePosixPath(".")
        or ".." in candidate.parts
        or any(":" in part for part in candidate.parts)
    )
    if invalid:
        contract.require(False, f"path must not escape the repository: {relative}")
        return root / "__invalid_contract_path__"

    repository_root = root.resolve()
    resolved = repository_root.joinpath(*candidate.parts).resolve()
    if resolved != repository_root and repository_root not in resolved.parents:
        contract.require(False, f"path resolves outside the repository: {relative}")
        return root / "__invalid_contract_path__"
    return resolved


def element_text(root: ET.Element, name: str) -> str | None:
    for element in root.iter():
        if element.tag.rsplit("}", 1)[-1] == name:
            return element.text
    return None


def check_identity_source(root: Path, contract: Contract) -> tuple[dict[str, Any], dict[str, Any]]:
    identity_path = root / "release/product-identity.json"
    contract.require(identity_path.is_file(), "missing release/product-identity.json")
    identity = json.loads(identity_path.read_text(encoding="utf-8"))

    contract.equal(identity.get("schemaVersion"), 1, "identity schema version drifted")
    localization = identity.get("localization", {})
    contract.equal(localization.get("defaultLocale"), "zh-CN", "default locale drifted")
    contract.equal(localization.get("availableLocales"), ["zh-CN"], "first release locale set drifted")
    contract.equal(set(identity.get("locales", {})), {"zh-CN"}, "localized copy seam drifted")
    contract.equal(
        identity.get("product", {}).get("internalId"),
        "azzs",
        "internal repository identifier drifted",
    )
    locale = identity["locales"]["zh-CN"]
    contract.equal(locale.get("displayName"), EXPECTED_NAME, "canonical display name drifted")
    contract.equal(locale.get("description"), EXPECTED_DESCRIPTION, "canonical description drifted")

    artifacts = [
        (
            item.get("id"),
            item.get("edition"),
            item.get("packageKind"),
            item.get("architecture"),
            item.get("displayNameKey"),
            item.get("releaseAssetPlaceholder"),
        )
        for item in identity.get("artifactMatrix", [])
    ]
    contract.equal(artifacts, EXPECTED_ARTIFACTS, "the three-edition/eight-artifact matrix drifted")
    contract.equal(len(set(artifacts)), 8, "artifact tuples must be unique")
    contract.equal(locale.get("artifactDisplayNames"), EXPECTED_DISPLAY_NAMES, "artifact display names drifted")
    contract.equal(
        len(set(locale.get("artifactDisplayNames", {}).values())),
        8,
        "artifact display names must be unique",
    )

    graphic = identity.get("product", {}).get("graphic", {})
    contract.equal(graphic.get("rules"), EXPECTED_GRAPHIC_RULES, "graphic rules drifted")
    contract.equal(graphic.get("runtimeDependencies"), [], "product graphic gained a runtime dependency")
    source = repository_path(root, graphic.get("authoritativeSource", ""), contract)
    contract.require(source.is_file(), f"missing authoritative SVG: {source}")

    contract.equal(locale.get("riskCopy"), EXPECTED_RISK_COPY, "release risk policy drifted")
    contract.equal(
        locale.get("minimumVersion"),
        EXPECTED_MINIMUM_VERSION,
        "minimum-version boundary copy drifted",
    )
    return identity, locale


def check_svg_and_derivatives(
    root: Path, identity: dict[str, Any], contract: Contract
) -> None:
    graphic = identity["product"]["graphic"]
    svg_path = repository_path(root, graphic["authoritativeSource"], contract)
    svg_root = ET.parse(svg_path).getroot()
    contract.equal(svg_root.attrib.get("viewBox"), "0 0 256 256", "SVG viewBox drifted")
    forbidden_tags = {"text", "image", "script", "foreignObject", "style", "use"}
    for element in svg_root.iter():
        tag = element.tag.rsplit("}", 1)[-1]
        contract.require(tag not in forbidden_tags, f"SVG contains forbidden element: {tag}")
        for attribute, value in element.attrib.items():
            local_attribute = attribute.rsplit("}", 1)[-1]
            contract.require(local_attribute != "href", "SVG must not reference an external asset")
            contract.require("url(" not in value.lower(), "SVG must not use URL-backed resources")

    windows = graphic["windows"]
    sizes = windows["pngSizes"]
    contract.equal(sizes, [16, 20, 24, 32, 40, 48, 64, 96, 128, 256], "Windows PNG size set drifted")
    png_payloads: dict[int, bytes] = {}
    for size in sizes:
        path = repository_path(root, windows["pngPattern"].format(size=size), contract)
        contract.require(path.is_file(), f"missing generated PNG: {path}")
        if not path.is_file():
            continue
        payload = path.read_bytes()
        contract.require(payload.startswith(b"\x89PNG\r\n\x1a\n"), f"invalid PNG signature: {path}")
        if len(payload) >= 24:
            width, height = struct.unpack(">II", payload[16:24])
            contract.equal((width, height), (size, size), f"PNG dimensions drifted: {path}")
        png_payloads[size] = payload

    icon_path = repository_path(root, windows["icon"], contract)
    contract.require(icon_path.is_file(), f"missing generated ICO: {icon_path}")
    if icon_path.is_file():
        icon = icon_path.read_bytes()
        contract.require(icon.startswith(b"\x00\x00\x01\x00"), "invalid ICO signature")
        if len(icon) >= 6:
            reserved, image_type, count = struct.unpack("<HHH", icon[:6])
            contract.equal((reserved, image_type, count), (0, 1, len(sizes)), "ICO directory drifted")
            entries: dict[int, bytes] = {}
            for index in range(count):
                start = 6 + index * 16
                entry = icon[start : start + 16]
                contract.require(len(entry) == 16, "ICO entry is truncated")
                if len(entry) != 16:
                    continue
                width, height, _, _, planes, bits, length, offset = struct.unpack("<BBBBHHII", entry)
                decoded_width = 256 if width == 0 else width
                decoded_height = 256 if height == 0 else height
                contract.equal(decoded_width, decoded_height, "ICO entry is not square")
                contract.equal((planes, bits), (1, 32), "ICO entry format drifted")
                entries[decoded_width] = icon[offset : offset + length]
            contract.equal(set(entries), set(sizes), "ICO size set drifted")
            for size, png in png_payloads.items():
                contract.equal(entries.get(size), png, f"ICO does not embed the canonical {size}px PNG")

    generated = subprocess.run(
        [sys.executable, str(root / "eng/generate-product-assets.py"), "--check"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    contract.require(
        generated.returncode == 0,
        "asset regeneration check failed: " + (generated.stderr.strip() or generated.stdout.strip()),
    )


def render_release_template(
    identity: dict[str, Any], locale: dict[str, Any], contract: Contract
) -> str:
    template_source = locale["releaseNotesTemplate"]
    skeleton = template_source.get("skeletonLines", [])
    contract.equal(
        skeleton,
        EXPECTED_TEMPLATE_SKELETON_LINES,
        "release template safe skeleton drifted",
    )
    contract.equal(
        skeleton.count("{{ARTIFACT_ROWS}}"),
        1,
        "release template needs one artifact row insertion point",
    )

    display_names = locale["artifactDisplayNames"]
    artifact_rows = [
        f'| {display_names[item["displayNameKey"]]} | {item["releaseAssetPlaceholder"]} |'
        for item in identity["artifactMatrix"]
    ]
    placeholders = [item["releaseAssetPlaceholder"] for item in identity["artifactMatrix"]]
    contract.equal(len(set(placeholders)), 8, "release asset placeholders must be unique")

    replacements = {
        "{{PRODUCT_DISPLAY_NAME}}": locale["displayName"],
        "{{PRODUCT_DESCRIPTION}}": locale["description"],
        "{{RISK_UNSIGNED}}": locale["riskCopy"]["unsigned"],
        "{{RISK_VERIFICATION}}": locale["riskCopy"]["verification"],
        "{{RISK_SMARTSCREEN}}": locale["riskCopy"]["smartScreen"],
        "{{RISK_THIRD_PARTY}}": locale["riskCopy"]["thirdParty"],
        "{{MINIMUM_VERSION_COPY}}": locale["minimumVersion"]["copy"],
    }
    rendered_lines: list[str] = []
    for source_line in skeleton:
        if source_line == "{{ARTIFACT_ROWS}}":
            rendered_lines.extend(artifact_rows)
            continue
        rendered = source_line
        for token, value in replacements.items():
            rendered = rendered.replace(token, value)
        rendered_lines.append(rendered)
    return "\n".join(rendered_lines) + "\n"


def check_release_template(
    root: Path,
    identity: dict[str, Any],
    locale: dict[str, Any],
    contract: Contract,
) -> None:
    template_source = locale["releaseNotesTemplate"]
    template_path = repository_path(root, template_source["path"], contract)
    contract.require(template_path.is_file(), f"missing release template: {template_path}")
    template = template_path.read_text(encoding="utf-8")
    expected_template = render_release_template(identity, locale, contract)
    contract.equal(
        template,
        expected_template,
        "release template is not the exact rendered canonical copy",
    )
    for required in (
        EXPECTED_NAME,
        EXPECTED_DESCRIPTION,
        "{{VERSION}}",
        "{{RELEASE_DATE}}",
        "{{RELEASE_CHANNEL}}",
        "{{CHANGE_SUMMARY}}",
        "{{KNOWN_ISSUES}}",
        "{{INSTALLATION_AND_STARTUP_NOTES}}",
        *EXPECTED_DISPLAY_NAMES.values(),
        *locale["riskCopy"].values(),
        locale["minimumVersion"]["copy"],
    ):
        contract.require(required in template, f"release template is missing canonical copy: {required}")

    artifact_placeholders = [
        item["releaseAssetPlaceholder"] for item in identity["artifactMatrix"]
    ]
    contract.equal(
        sorted(re.findall(r"\{\{ASSET_[A-Z0-9_]+\}\}", template)),
        sorted(artifact_placeholders),
        "release template artifact placeholders drifted",
    )

    for forbidden in (
        "未实机验证",
        "实机验证通过",
        "已完成真实设备验证",
        "已签名",
        "安全已验证",
        "来源已验证",
        "第三方包已验证",
        "完整性已验证",
        "兼容所有 Windows",
    ):
        contract.require(forbidden not in template, f"release template contains a forbidden claim: {forbidden}")


def check_consumers(root: Path, locale: dict[str, Any], identity: dict[str, Any], contract: Contract) -> None:
    name = locale["displayName"]
    description = locale["description"]
    icon_path = repository_path(root, identity["product"]["graphic"]["windows"]["icon"], contract)

    readme = (root / "README.md").read_text(encoding="utf-8")
    contract.require(readme.startswith(f"# {name}\n"), "README title does not consume the canonical name")
    contract.require(description in readme, "README does not consume the canonical description")

    resources_path = root / "src/adapters/ui/winui/Strings/zh-CN/Resources.resw"
    resources_root = ET.parse(resources_path).getroot()
    title = resources_root.find("./data[@name='MainWindowTitle']/value")
    contract.equal(title.text if title is not None else None, name, "WinUI title resource drifted")
    for resource_file in (root / "src/adapters/ui/winui/Strings").rglob("*.resw"):
        contract.require(
            "未实机验证" not in resource_file.read_text(encoding="utf-8"),
            f"developer evidence leaked into a WinUI resource: {resource_file}",
        )
    cjk = re.compile(r"[\u3400-\u9fff]")
    winui_root = root / "src/adapters/ui/winui"
    # Issue 24's isolated fixtures intentionally carry localized stress content.
    fixture_page_root = winui_root / "DesignSystem/Fixtures"
    string_literal = re.compile(
        r'(?:u8|u|U|L)?"(?:\\.|[^"\\])*"',
        re.DOTALL,
    )
    raw_string_literal = re.compile(
        r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})'
        r'\((?P<body>.*?)\)(?P=delimiter)"',
        re.DOTALL,
    )

    def source_passes_localization_contract(source_file: Path, source: str) -> bool:
        if fixture_page_root in source_file.parents:
            return True
        literals = [match.group(0) for match in string_literal.finditer(source)]
        literals.extend(match.group("body") for match in raw_string_literal.finditer(source))
        return all(cjk.search(literal) is None for literal in literals)

    production_probe = winui_root / "DesignSystem/presentation_contract.cpp"
    contract.require(
        not source_passes_localization_contract(
            production_probe,
            'auto const localization_probe = L"生产中文探针";',
        ),
        "production presentation_contract.cpp must reject Chinese string literals",
    )
    for pattern in ("*.cpp", "*.h", "*.idl"):
        for source_file in winui_root.rglob(pattern):
            source = source_file.read_text(encoding="utf-8")
            contract.require(
                source_passes_localization_contract(source_file, source),
                f"WinUI Chinese string literals must be stored in resw: {source_file}",
            )
    for xaml_file in winui_root.rglob("*.xaml"):
        if fixture_page_root in xaml_file.parents:
            continue
        xaml_root = ET.parse(xaml_file).getroot()
        visible_values: list[str] = []
        for element in xaml_root.iter():
            visible_values.extend(element.attrib.values())
            if element.text:
                visible_values.append(element.text)
            if element.tail:
                visible_values.append(element.tail)
        contract.require(
            all(cjk.search(value) is None for value in visible_values),
            f"WinUI XAML Chinese must be stored in resw: {xaml_file}",
        )

    wix_path = root / "installer/Package.wxs"
    wix_root = ET.parse(wix_path).getroot()
    wix_namespace = {"w": "http://wixtoolset.org/schemas/v4/wxs"}
    package = wix_root.find("w:Package", wix_namespace)
    contract.require(package is not None, "WiX package element is missing")
    if package is not None:
        contract.equal(package.attrib.get("Name"), name, "WiX DisplayName drifted")
        contract.equal(package.attrib.get("Scope"), "perMachine", "WiX installer scope drifted")
        standard_directory = package.find("w:StandardDirectory", wix_namespace)
        directory = package.find(".//w:Directory[@Id='INSTALLFOLDER']", wix_namespace)
        feature = package.find(".//w:Feature[@Id='MainFeature']", wix_namespace)
        icon = package.find("w:Icon[@Id='ProductIcon']", wix_namespace)
        arp_icon = package.find("w:Property[@Id='ARPPRODUCTICON']", wix_namespace)
        contract.equal(
            standard_directory.attrib.get("Id") if standard_directory is not None else None,
            "ProgramFiles64Folder",
            "WiX machine installer must target 64-bit Program Files",
        )
        contract.equal(directory.attrib.get("Name") if directory is not None else None, name, "install directory name drifted")
        contract.equal(feature.attrib.get("Title") if feature is not None else None, name, "WiX feature title drifted")
        contract.equal(
            icon.attrib.get("SourceFile") if icon is not None else None,
            "$(var.ProductIconPath)",
            "WiX does not consume ProductIconPath",
        )
        contract.equal(
            arp_icon.attrib.get("Value") if arp_icon is not None else None,
            "ProductIcon",
            "WiX ARP icon registration drifted",
        )

    wix_project = ET.parse(root / "installer/Azzs.Installer.wixproj").getroot()
    project_icon = element_text(wix_project, "ProductIconPath")
    expected_project_icon = (
        "$(MSBuildThisFileDirectory)..\\"
        + icon_path.relative_to(root).as_posix().replace("/", "\\")
    )
    contract.equal(project_icon, expected_project_icon, "WiX project icon path drifted")
    define_constants = element_text(wix_project, "DefineConstants") or ""
    contract.require(
        "ProductIconPath=$(ProductIconPath)" in define_constants,
        "WiX ProductIconPath is not passed to the source",
    )
    contract.require(
        any(
            "!Exists('$(ProductIconPath)')" in element.attrib.get("Condition", "")
            for element in wix_project.iter()
            if element.tag.rsplit("}", 1)[-1] == "Error"
        ),
        "WiX project does not validate the product icon",
    )

    rc_path = root / "src/adapters/ui/winui/app.rc"
    rc = rc_path.read_text(encoding="utf-8")
    relative_icon = os.path.relpath(icon_path, rc_path.parent).replace("/", "\\")
    escaped_relative_icon = relative_icon.replace("\\", "\\\\")
    contract.require(
        f'ICON "{escaped_relative_icon}"' in rc,
        "PE resource does not consume the canonical ICO",
    )
    contract.require(f'VALUE "ProductName", "{name}\\0"' in rc, "PE ProductName drifted")
    contract.require(f'VALUE "FileDescription", "{description}\\0"' in rc, "PE FileDescription drifted")

    project_path = root / "src/adapters/ui/winui/Azzs.WinUI.vcxproj"
    project_root = ET.parse(project_path).getroot()
    msbuild_namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    resources = project_root.findall(".//m:ResourceCompile", msbuild_namespace)
    contract.equal(
        [item.attrib.get("Include") for item in resources if item.attrib.get("Include")],
        ["app.rc"],
        "WinUI PE resource inclusion drifted",
    )
    manifests = project_root.findall(".//m:Manifest", msbuild_namespace)
    contract.equal(
        [item.attrib.get("Include") for item in manifests],
        ["$(AzzsApplicationManifest)"],
        "WinUI manifest inclusion drifted",
    )
    target_name = project_root.find(".//m:TargetName", msbuild_namespace)
    contract.equal(target_name.text if target_name is not None else None, "Azzs.WinUI", "internal executable name drifted")

    manifest_root = ET.parse(root / "src/adapters/ui/winui/app.manifest").getroot()
    manifest_identity = manifest_root.find("{urn:schemas-microsoft-com:asm.v1}assemblyIdentity")
    contract.equal(
        manifest_identity.attrib.get("name") if manifest_identity is not None else None,
        "CoolPlume.Azzs",
        "internal manifest identity drifted",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.repository_root.resolve()
    contract = Contract()

    identity, locale = check_identity_source(root, contract)
    check_svg_and_derivatives(root, identity, contract)
    check_release_template(root, identity, locale, contract)
    check_consumers(root, locale, identity, contract)

    if contract.failures:
        for failure in contract.failures:
            print(f"product identity contract: FAIL: {failure}", file=sys.stderr)
        return 1
    print("product identity contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
