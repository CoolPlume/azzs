#!/usr/bin/env python3
"""Static contracts for WinUI fire-and-forget event boundaries."""

from __future__ import annotations

import re
import sys
from pathlib import Path


class ContractFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def read(path: Path) -> str:
    require(path.is_file(), f"missing required file: {path}")
    return path.read_text(encoding="utf-8")


def verify_file(root: Path, relative_path: str, expected_handlers: int,
                guard_name: str) -> None:
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
    require(source.count("catch (...) {") == expected_handlers,
            f"{relative_path} must catch every fire-and-forget failure")
    require(source.count("OutputDebugStringW") >= expected_handlers,
            f"{relative_path} must leave a no-throw diagnostic for each catch")
    require(source.count(guard_name) >= expected_handlers,
            f"{relative_path} must gate every dialog with {guard_name}")


def verify(root: Path) -> None:
    verify_file(root, "src/adapters/ui/winui/MainWindow.xaml.cpp", 1,
                "catalog_close_dialog_open_")
    verify_file(root, "src/adapters/ui/winui/Pages/ApplicationSettingsPage.xaml.cpp",
                4, "confirmation_dialog_open_")
    verify_file(root, "src/adapters/ui/winui/Pages/HistoryAndLogsPage.xaml.cpp", 1,
                "confirmation_dialog_open_")
    verify_file(root, "src/adapters/ui/winui/Pages/SoftwareCatalogEditorPage.xaml.cpp",
                2, "confirmation_dialog_open_")
    verify_file(root, "src/adapters/ui/winui/Pages/SoftwareOptimizationPage.xaml.cpp",
                1, "confirmation_dialog_open_")
    verify_file(root, "src/adapters/ui/winui/Pages/SystemOptimizationPage.xaml.cpp",
                1, "confirmation_dialog_open_")

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

    project = read(root / "src/adapters/ui/winui/Azzs.WinUI.vcxproj")
    require(
        "user32.lib" in project,
        "the WinUI host must link user32 for the startup last-resort MessageBoxW fallback",
    )

    resources = read(root / "src/adapters/ui/winui/Strings/zh-CN/Resources.resw")
    require(
        'name="ApplicationUpdateTitle.Text"' in resources and
        'name="ApplicationUpdateTitle"' not in resources,
        "WinUI resources must not define ApplicationUpdateTitle as both a scope and a resource",
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
