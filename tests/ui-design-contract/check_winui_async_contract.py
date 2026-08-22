#!/usr/bin/env python3
"""Static contracts for WinUI fire-and-forget event boundaries."""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


class ContractFailure(RuntimeError):
    pass


MSBUILD_NS = "http://schemas.microsoft.com/developer/msbuild/2003"
MSBUILD = {"m": MSBUILD_NS}
EXPECTED_WINUI_CONFIGURATIONS = {
    "Debug|x64",
    "Debug|ARM64",
    "Release|x64",
    "Release|ARM64",
}
MSBUILD_CONDITION = re.compile(
    r"['\"]\$\((Configuration|Platform)\)['\"]\s*==\s*['\"]([^'\"]+)['\"]"
)
CPP_NON_CODE = re.compile(
    r'//[^\r\n]*|/\*.*?\*/|'
    r'R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)"|'
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    flags=re.DOTALL,
)
REAL_WWINMAIN = re.compile(
    r"\bint\s+(?:__stdcall|WINAPI)\s+wWinMain\s*\([^)]*\)\s*\{"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def read(path: Path) -> str:
    require(path.is_file(), f"missing required file: {path}")
    return path.read_text(encoding="utf-8")


def parse_xml(path: Path) -> ET.Element:
    require(path.is_file(), f"missing required XML file: {path}")
    try:
        return ET.parse(path).getroot()
    except ET.ParseError as error:
        raise ContractFailure(f"invalid XML in {path}: {error}") from error


def msbuild_condition_applies(condition: str, configuration: str,
                              platform: str) -> bool:
    condition = condition.strip()
    if not condition:
        return True

    clauses = list(MSBUILD_CONDITION.finditer(condition))
    require(
        clauses,
        f"unsupported WinUI MSBuild condition (expected Configuration/Platform equality): {condition}",
    )
    remainder = MSBUILD_CONDITION.sub("", condition)
    remainder = re.sub(r"\bAnd\b", "", remainder, flags=re.IGNORECASE)
    remainder = remainder.replace("(", "").replace(")", "").strip()
    require(
        not remainder,
        f"unsupported WinUI MSBuild condition syntax: {condition}",
    )
    expected = {"Configuration": configuration, "Platform": platform}
    return all(expected[match.group(1)] == match.group(2) for match in clauses)


def strip_cpp_non_code(source: str) -> str:
    def blank(match: re.Match[str]) -> str:
        return "".join("\n" if character == "\n" else " "
                       for character in match.group(0))

    return CPP_NON_CODE.sub(blank, source)


def braced_body(source: str, opening_brace: int) -> str:
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace + 1:index]
    raise ContractFailure("C++ function has an unterminated body")


def cpp_function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"missing C++ function signature: {signature}")
    opening_brace = source.find("{", start)
    require(opening_brace >= 0,
            f"missing opening brace for C++ function: {signature}")
    return braced_body(source, opening_brace)


def verify_winui_project(project_path: Path) -> ET.Element:
    project = parse_xml(project_path)
    configurations = project.findall(
        "./m:ItemGroup[@Label='ProjectConfigurations']/m:ProjectConfiguration",
        MSBUILD,
    )
    configuration_keys: list[str] = []
    for item in configurations:
        include = item.attrib.get("Include")
        configuration = item.findtext("m:Configuration", namespaces=MSBUILD)
        platform = item.findtext("m:Platform", namespaces=MSBUILD)
        require(
            include is not None and configuration is not None and platform is not None,
            "each WinUI ProjectConfiguration must declare Include, Configuration, and Platform",
        )
        key = f"{configuration}|{platform}"
        require(
            include == key,
            f"WinUI ProjectConfiguration Include drifted: expected {key!r}, got {include!r}",
        )
        configuration_keys.append(key)

    require(
        len(configuration_keys) == len(set(configuration_keys)),
        f"WinUI project contains duplicate configurations: {configuration_keys}",
    )
    require(
        set(configuration_keys) == EXPECTED_WINUI_CONFIGURATIONS,
        "WinUI project must declare exactly Debug/Release x64/ARM64 configurations; "
        f"found {sorted(configuration_keys)}",
    )

    definition_groups = project.findall("./m:ItemDefinitionGroup", MSBUILD)
    for key in sorted(EXPECTED_WINUI_CONFIGURATIONS):
        configuration, platform = key.split("|", 1)
        definitions: list[str] = []
        for group in definition_groups:
            if not msbuild_condition_applies(
                group.attrib.get("Condition", ""), configuration, platform
            ):
                continue
            definition = group.find(
                "m:ClCompile/m:PreprocessorDefinitions", MSBUILD
            )
            if definition is not None and definition.text:
                definitions.extend(
                    token.strip() for token in definition.text.split(";")
                )
        require(
            any(
                token == "DISABLE_XAML_GENERATED_MAIN"
                or token.startswith("DISABLE_XAML_GENERATED_MAIN=")
                for token in definitions
            ),
            f"WinUI {key} ClCompile definitions must define DISABLE_XAML_GENERATED_MAIN; "
            f"effective definitions were {definitions}",
        )

    dependencies = project.findall(
        ".//m:Link/m:AdditionalDependencies", MSBUILD
    )
    require(
        any(
            "user32.lib" in {
                token.strip() for token in (dependency.text or "").split(";")
            }
            for dependency in dependencies
        ),
        "the WinUI host must link user32 for the startup last-resort MessageBoxW fallback",
    )

    clcompile_paths = [
        item.attrib.get("Include", "").replace("\\", "/")
        for item in project.findall(".//m:ClCompile", MSBUILD)
    ]
    startup_entries = [path for path in clcompile_paths if path == "startup_entry.cpp"]
    require(
        len(startup_entries) == 1,
        "Azzs.WinUI.vcxproj must include startup_entry.cpp exactly once as a ClCompile source; "
        f"found {startup_entries}",
    )
    return project


def verify_startup_entry(source: str) -> None:
    code = strip_cpp_non_code(source)
    entry_definitions = list(REAL_WWINMAIN.finditer(code))
    require(
        len(entry_definitions) == 1,
        "startup_entry.cpp must contain exactly one real int __stdcall/ WINAPI wWinMain definition; "
        f"found {len(entry_definitions)}",
    )
    entry_body = braced_body(code, entry_definitions[0].end() - 1)
    require(
        re.search(r"\breturn\s+wXamlGeneratedMain\s*\(", entry_body) is not None,
        "wWinMain must delegate to wXamlGeneratedMain",
    )
    require(
        re.search(r"\bcatch\s*\(\s*\.\.\.\s*\)\s*\{", entry_body) is not None,
        "wWinMain must catch all early startup exceptions",
    )
    catch_header = re.search(
        r"\bcatch\s*\(\s*\.\.\.\s*\)\s*\{",
        entry_body,
    )
    require(catch_header is not None, "wWinMain catch-all body is missing")
    catch_body = braced_body(entry_body, catch_header.end() - 1)
    require(
        re.search(r"\breturn\s+1\s*;", catch_body) is not None,
        "wWinMain catch-all fallback must return a non-zero status",
    )
    require(
        re.search(r"\bshow_startup_failure\s*\(\s*\)", catch_body) is not None,
        "wWinMain catch-all fallback must report the startup failure",
    )
    require(
        re.search(r"\bOutputDebugStringW\s*\(", code) is not None
        and re.search(r"\bMessageBoxW\s*\(", code) is not None,
        "startup failure reporting must retain both OutputDebugStringW and MessageBoxW",
    )


def verify_file(root: Path, relative_path: str, expected_handlers: int,
                guard_name: str, expected_catches: int | None = None) -> None:
    path = root / relative_path
    source = read(path)
    handlers = re.findall(
        r"winrt::fire_and_forget\s+[\w:]+\s*\([^)]*\)\s*\{", source,
        flags=re.DOTALL,
    )
    require(len(handlers) == expected_handlers,
            f"{relative_path} must keep {expected_handlers} fire-and-forget handlers")
    require(source.count("co_await dialog.ShowAsync()") == expected_handlers,
            f"{relative_path} must guard every dialog await")
    if expected_catches is not None:
        require(source.count("catch (...) {") == expected_catches,
                f"{relative_path} must keep {expected_catches} catch-all boundaries")
    require(source.count("OutputDebugStringW") >= expected_handlers,
            f"{relative_path} must leave a no-throw diagnostic for each catch")
    require(source.count(guard_name) >= expected_handlers,
            f"{relative_path} must gate every dialog with {guard_name}")


def verify(root: Path) -> None:
    verify_file(root, "src/adapters/ui/winui/MainWindow.xaml.cpp", 1,
                "catalog_close_dialog_open_")
    verify_file(root, "src/adapters/ui/winui/Pages/ApplicationSettingsPage.xaml.cpp",
                4, "confirmation_dialog_open_", expected_catches=4)
    verify_file(root, "src/adapters/ui/winui/Pages/HistoryAndLogsPage.xaml.cpp", 1,
                "confirmation_dialog_open_", expected_catches=1)
    verify_file(root, "src/adapters/ui/winui/Pages/SoftwareCatalogEditorPage.xaml.cpp",
                2, "confirmation_dialog_open_", expected_catches=2)
    verify_file(root, "src/adapters/ui/winui/Pages/SoftwareOptimizationPage.xaml.cpp",
                1, "confirmation_dialog_open_", expected_catches=1)
    verify_file(root, "src/adapters/ui/winui/Pages/SystemOptimizationPage.xaml.cpp",
                1, "confirmation_dialog_open_", expected_catches=1)

    for relative_path in (
        "src/adapters/ui/winui/Pages/ApplicationSettingsPage.xaml.h",
        "src/adapters/ui/winui/Pages/HistoryAndLogsPage.xaml.h",
        "src/adapters/ui/winui/Pages/SoftwareCatalogEditorPage.xaml.h",
        "src/adapters/ui/winui/Pages/SoftwareOptimizationPage.xaml.h",
        "src/adapters/ui/winui/Pages/SystemOptimizationPage.xaml.h",
    ):
        require("bool confirmation_dialog_open_{false};" in read(root / relative_path),
                f"{relative_path} must own the dialog gate")

    composition = read(root / "src/composition/windows/composition_root.cpp")
    require(
        "software_catalog_file_(" in composition and
        "bundled_catalog_resources.software_catalog.bytes()" in composition,
        "startup services must initialize the bundled software catalog in the constructor scope",
    )
    require(
        "software_catalog_file_{" not in composition,
        "startup services must not initialize a member from an out-of-scope constructor parameter",
    )

    motion_header = read(
        root / "src/adapters/ui/winui/DesignSystem/motion_preferences.hpp"
    )
    require(
        "std::optional<winrt::Microsoft::UI::Dispatching::DispatcherQueue>" in motion_header,
        "motion preferences must make the dispatcher queue explicitly optional",
    )
    motion_source = read(
        root / "src/adapters/ui/winui/DesignSystem/motion_preferences.cpp"
    )
    require(
        "dispatcher_queue_->HasThreadAccess()" in motion_source and
        "dispatcher_queue_->TryEnqueue" in motion_source,
        "motion preferences must dereference the optional dispatcher queue only after a presence check",
    )

    verify_winui_project(root / "src/adapters/ui/winui/Azzs.WinUI.vcxproj")
    verify_startup_entry(read(root / "src/adapters/ui/winui/startup_entry.cpp"))

    resources = read(root / "src/adapters/ui/winui/Strings/zh-CN/Resources.resw")
    require(
        'name="ApplicationUpdateTitle.Text"' in resources and
        'name="ApplicationUpdateTitle"' not in resources,
        "WinUI resources must not define ApplicationUpdateTitle as both a scope and a resource",
    )

    app_header = read(root / "src/adapters/ui/winui/App.xaml.h")
    require(
        "windows_device_data_environment.hpp" not in app_header and
        "StartupAssemblyStatus" in app_header,
        "App must retain startup failures through the application-layer status type",
    )
    app_source = read(root / "src/adapters/ui/winui/App.xaml.cpp")
    require(
        "startup_status_ = std::move(startup.status)" in app_source,
        "App must retain the typed startup status returned by the composition root",
    )

    main_window = read(root / "src/adapters/ui/winui/MainWindow.xaml.cpp")
    main_window_header = read(root / "src/adapters/ui/winui/MainWindow.xaml.h")
    main_window_xaml = read(root / "src/adapters/ui/winui/MainWindow.xaml")
    navigate_body = cpp_function_body(
        main_window, "bool MainWindow::navigate_and_commit(")
    prepare_body = cpp_function_body(
        main_window, "MainWindow::prepare_application_settings_page()")
    commit_body = cpp_function_body(
        main_window, "void MainWindow::commit_application_settings_page(")
    recovery_body = cpp_function_body(
        main_window, "void MainWindow::restore_settings_navigation_state(")
    failure_body = cpp_function_body(
        main_window, "void MainWindow::handle_settings_navigation_failure()")
    retry_body = cpp_function_body(
        main_window, "void MainWindow::OnRetrySettingsNavigationClick(")
    return_body = cpp_function_body(
        main_window, "void MainWindow::OnReturnToCurrentPageClick(")
    require(
        "if (page == PageId::application_settings)" in navigate_body and
        "settings_navigation_bridge_.navigate({" in navigate_body,
        "application-settings navigation must use the recovery bridge at its user-navigation boundary",
    )
    require(
        "prepare_application_settings_page()" in navigate_body and
        "commit_application_settings_page(prepared," in navigate_body and
        "restore_settings_navigation_state(" in navigate_body,
        "application-settings preparation, commit, and recovery must stay in one boundary",
    )
    require(
        "ContentFrame().Content(" not in prepare_body and
        "workbench_->navigate(" not in prepare_body and
        "workbench_->snapshot()" in prepare_body and
        "settings.snapshot()" in prepare_body and
        "->bind(" in prepare_body,
        "settings preparation must read snapshots and bind a candidate without publishing visible/core state",
    )
    require(
        "ContentFrame().Content(prepared);" in commit_body and
        "displayed_page_ = PageId::application_settings;" in commit_body and
        "workbench_->navigate(PageId::application_settings);" in commit_body,
        "settings commit must publish the candidate only after preparation succeeds",
    )
    require(
        "navigate_to(previous_page)" not in navigate_body and
        "navigate_and_commit(previous_page)" not in navigate_body,
        "settings failure recovery must use the single recovery owner without rebinding the old page",
    )
    require(
        "ContentFrame().Content(previous_content);" in recovery_body and
        "workbench_->navigate(previous_core_page);" in recovery_body and
        "PrimaryNavigation().SelectedItem" in recovery_body,
        "settings recovery must restore the prior frame, core page, and selection",
    )
    require(
        "MainWindowSettingsNavigationFailed.Title" in failure_body and
        "MainWindowSettingsNavigationFailed.Message" in failure_body and
        "SettingsNavigationFailureInfoBar().IsOpen(true)" in failure_body and
        "PrimaryNavigation().SelectedItem" not in failure_body,
        "settings failures must project a localized, discoverable InfoBar after state restore",
    )
    require(
        "settings_navigation_bridge_.retry()" in retry_body and
        "settings_navigation_bridge_.return_to_current()" in return_body and
        "OnRetrySettingsNavigationClick" in main_window_header and
        "OnReturnToCurrentPageClick" in main_window_header,
        "application-settings recovery actions must be declared by MainWindow",
    )
    require(
        "record_settings_navigation_failure(failure);" in navigate_body and
        "handle_settings_navigation_failure();" in navigate_body and
        "workbench_->navigate(PageId::application_settings)" not in failure_body,
        "application-settings failure handling must record and project without committing the failed page",
    )
    for automation_id in (
        "AzzsSettingsNavigationFailureInfoBar",
        "AzzsRetrySettingsNavigation",
        "AzzsReturnToCurrentPage",
    ):
        require(
            automation_id in main_window_xaml,
            f"settings navigation recovery must expose {automation_id}",
        )
    for resource_key in (
        "MainWindowSettingsNavigationFailed.Title",
        "MainWindowSettingsNavigationFailed.Message",
        "MainWindowRetrySettingsNavigation.Content",
        "MainWindowReturnToCurrentPage.Content",
    ):
        require(
            f'name="{resource_key}"' in resources,
            f"settings navigation recovery must define resource {resource_key}",
        )


def main() -> int:
    try:
        require(len(sys.argv) == 2, "usage: check_winui_async_contract.py <repo-root>")
        verify(Path(sys.argv[1]).resolve())
    except ContractFailure as error:
        print(f"FAILED: {error}", file=sys.stderr)
        return 1
    print("WinUI async boundary contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
