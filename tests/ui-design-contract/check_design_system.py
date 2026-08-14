#!/usr/bin/env python3
"""Static WinUI design-system contract checks with no app or device side effects."""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


XAML_NS = "http://schemas.microsoft.com/winfx/2006/xaml/presentation"
X_NS = "http://schemas.microsoft.com/winfx/2006/xaml"
MSBUILD_NS = "http://schemas.microsoft.com/developer/msbuild/2003"
X_KEY = f"{{{X_NS}}}Key"


class ContractFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def read(path: Path) -> str:
    require(path.is_file(), f"missing required file: {path}")
    return path.read_text(encoding="utf-8")


def parse_xml(path: Path) -> ET.Element:
    try:
        return ET.parse(path).getroot()
    except ET.ParseError as error:
        raise ContractFailure(f"invalid XML in {path}: {error}") from error


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def normalized_project_path(value: str) -> str:
    return value.replace("\\", "/")


def argb(value: str) -> tuple[float, tuple[float, float, float]]:
    match = re.fullmatch(r"#([0-9A-Fa-f]{8})", value)
    require(match is not None, f"expected an ARGB color, found {value}")
    encoded = match.group(1)
    alpha = int(encoded[0:2], 16) / 255
    channels = tuple(int(encoded[index:index + 2], 16) / 255
                     for index in (2, 4, 6))
    return alpha, channels


def contrast_ratio(foreground: str, background: str) -> float:
    foreground_alpha, foreground_channels = argb(foreground)
    background_alpha, background_channels = argb(background)
    require(background_alpha == 1, "contrast backgrounds must be opaque")
    composite = tuple(
        foreground_channel * foreground_alpha +
        background_channel * (1 - foreground_alpha)
        for foreground_channel, background_channel in
        zip(foreground_channels, background_channels)
    )

    def luminance(channels: tuple[float, float, float]) -> float:
        linear = tuple(
            channel / 12.92 if channel <= 0.04045 else
            ((channel + 0.055) / 1.055) ** 2.4
            for channel in channels
        )
        return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]

    lighter, darker = sorted(
        (luminance(composite), luminance(background_channels)), reverse=True)
    return (lighter + 0.05) / (darker + 0.05)


def verify_resource_dictionary(root: Path) -> None:
    theme_path = root / "src/adapters/ui/winui/Themes/DesignSystem.xaml"
    theme_root = parse_xml(theme_path)
    require(local_name(theme_root.tag) == "ResourceDictionary",
            "DesignSystem.xaml must be an independent ResourceDictionary")

    theme_dictionaries = next(
        (child for child in theme_root
         if local_name(child.tag) == "ResourceDictionary.ThemeDictionaries"),
        None,
    )
    require(theme_dictionaries is not None,
            "the design system must define ThemeDictionaries")

    themes: dict[str, ET.Element] = {}
    for dictionary in theme_dictionaries:
        key = dictionary.attrib.get(X_KEY)
        require(key is not None, "each theme dictionary needs x:Key")
        require(key not in themes, f"duplicate theme dictionary: {key}")
        themes[key] = dictionary
    require(set(themes) == {"Light", "Dark", "HighContrast"},
            "Light, Dark, and HighContrast themes must all exist")

    required_theme_keys = {
        "AzzsSurfaceRootBrush",
        "AzzsSurfaceContentBrush",
        "AzzsSurfaceRaisedBrush",
        "AzzsSurfaceBorderBrush",
        "AzzsTextPrimaryBrush",
        "AzzsTextSecondaryBrush",
        "AzzsStatusNeutralBrush",
        "AzzsStatusInformationBrush",
        "AzzsStatusSuccessBrush",
        "AzzsStatusWarningBrush",
        "AzzsStatusErrorBrush",
        "AzzsRiskLowBrush",
        "AzzsRiskMediumBrush",
        "AzzsRiskHighBrush",
        "AzzsRiskCriticalBrush",
        "AzzsCommandPrimaryBrush",
        "AzzsCommandSecondaryBrush",
        "AzzsCommandDangerBrush",
        "AzzsCommandOnAccentBrush",
        "AzzsCommandOnDangerBrush",
        "AzzsMaterialRootFallbackBrush",
        "AzzsMaterialFlyoutFallbackBrush",
        "AzzsFocusBrush",
    }
    theme_types: dict[str, str] = {}
    theme_colors: dict[str, dict[str, str]] = {}
    for theme_name, dictionary in themes.items():
        keys: dict[str, str] = {}
        colors: dict[str, str] = {}
        for child in dictionary:
            key = child.attrib.get(X_KEY)
            require(key is not None,
                    f"all {theme_name} theme entries must have x:Key")
            require(key not in keys,
                    f"duplicate {theme_name} theme key: {key}")
            keys[key] = local_name(child.tag)
            colors[key] = child.attrib.get("Color", "")
        require(set(keys) == required_theme_keys,
                f"{theme_name} theme keys differ from the semantic contract")
        if not theme_types:
            theme_types = keys
        else:
            require(keys == theme_types,
                    f"{theme_name} must keep every semantic key type-stable")
        theme_colors[theme_name] = colors

    contrast_pairs = (
        ("AzzsTextPrimaryBrush", "AzzsSurfaceRootBrush"),
        ("AzzsTextSecondaryBrush", "AzzsSurfaceContentBrush"),
        ("AzzsCommandOnAccentBrush", "AzzsCommandPrimaryBrush"),
        ("AzzsCommandOnDangerBrush", "AzzsCommandDangerBrush"),
    )
    for theme_name in ("Light", "Dark"):
        for foreground, background in contrast_pairs:
            require(contrast_ratio(theme_colors[theme_name][foreground],
                                   theme_colors[theme_name][background]) >= 4.5,
                    f"{theme_name} {foreground} lacks 4.5:1 contrast on "
                    f"{background}")
        for material_key in (
            "AzzsMaterialRootFallbackBrush",
            "AzzsMaterialFlyoutFallbackBrush",
        ):
            require(theme_colors[theme_name][material_key].startswith("#FF"),
                    f"{theme_name} {material_key} must remain an opaque "
                    "reduced-transparency fallback")

    high_contrast = ET.tostring(themes["HighContrast"], encoding="unicode")
    require("SystemColorWindowColor" in high_contrast and
            "SystemColorWindowTextColor" in high_contrast and
            "SystemColorHighlightColor" in high_contrast,
            "HighContrast resources must use Windows system colors")

    direct_keys: dict[str, str] = {}
    for child in theme_root:
        if local_name(child.tag) == "ResourceDictionary.ThemeDictionaries":
            continue
        key = child.attrib.get(X_KEY)
        require(key is not None, "every root resource must have x:Key")
        require(key not in direct_keys, f"duplicate root resource key: {key}")
        direct_keys[key] = local_name(child.tag)

    required_root_keys = {
        "AzzsPagePadding",
        "AzzsPagePaddingNarrow",
        "AzzsSectionPadding",
        "AzzsListRowPadding",
        "AzzsStageItemMargin",
        "AzzsTouchTargetMinHeight",
        "AzzsStageMinimumWidth",
        "AzzsWideLayoutMinWidth",
        "AzzsCornerRadiusSmall",
        "AzzsCornerRadiusMedium",
        "AzzsFontFamily",
        "AzzsIconFontFamily",
        "AzzsFontSizeCaption",
        "AzzsFontSizeBody",
        "AzzsFontSizeSection",
        "AzzsFontSizePageTitle",
        "AzzsIconInformation",
        "AzzsIconSuccess",
        "AzzsIconWarning",
        "AzzsIconError",
        "AzzsIconDisabledReason",
        "AzzsIconRisk",
        "AzzsIconProgress",
        "AzzsIconWaiting",
        "AzzsIconEmergencyWithdrawal",
        "AzzsIconResultLocator",
        "AzzsMotionDurationImmediate",
        "AzzsMotionDurationFeedback",
        "AzzsMotionDurationState",
        "AzzsMotionDurationOverlay",
        "AzzsMotionCurveEnter",
        "AzzsMotionCurveMove",
        "AzzsMotionCurveExit",
        "AzzsPrimaryNavigationStyle",
        "AzzsPageRootGridStyle",
        "AzzsPageTitleTextStyle",
        "AzzsSectionSurfaceStyle",
        "AzzsListRowSurfaceStyle",
        "AzzsDetailSurfaceStyle",
        "AzzsSummarySurfaceStyle",
        "AzzsStatusBandStyle",
        "AzzsInformationStatusBandStyle",
        "AzzsWarningStatusBandStyle",
        "AzzsErrorStatusBandStyle",
        "AzzsInlineErrorStyle",
        "AzzsDisabledReasonTextStyle",
        "AzzsRiskConfirmationSurfaceStyle",
        "AzzsProgressStyle",
        "AzzsWaitingStatusBandStyle",
        "AzzsFailureStatusBandStyle",
        "AzzsPendingStatusBandStyle",
        "AzzsEmergencyWithdrawalStatusBandStyle",
        "AzzsResultLocatorButtonStyle",
    }
    require(required_root_keys <= set(direct_keys),
            "layout, type, icon, motion, and reusable component keys are incomplete")

    durations = {
        element.attrib[X_KEY]: (element.text or "").strip()
        for element in theme_root
        if local_name(element.tag) == "Duration"
    }
    require(durations == {
        "AzzsMotionDurationImmediate": "0:0:0",
        "AzzsMotionDurationFeedback": "0:0:0.083",
        "AzzsMotionDurationState": "0:0:0.167",
        "AzzsMotionDurationOverlay": "0:0:0.250",
    }, "the application must have exactly the 0/83/167/250ms durations")

    curves = {
        element.attrib[X_KEY]: element.attrib.get("EasingMode")
        for element in theme_root
        if local_name(element.tag) == "CubicEase"
    }
    require(curves == {
        "AzzsMotionCurveEnter": "EaseOut",
        "AzzsMotionCurveMove": "EaseOut",
        "AzzsMotionCurveExit": "EaseOut",
    }, "custom motion must use the responsive, end-decelerating platform curve")

    theme_text = read(theme_path)
    nonzero_tracking = re.findall(r'CharacterSpacing"\s+Value="(?!0")([^"]+)',
                                  theme_text)
    require(not nonzero_tracking, "all design-system character spacing must be 0")
    require("AzzsMaterialRootFallbackBrush" in theme_text and
            "AzzsMaterialFlyoutFallbackBrush" in theme_text,
            "root and flyout materials require solid fallback resources")
    corner_radii = [
        float((element.text or "0").split(",")[0])
        for element in theme_root
        if local_name(element.tag) == "CornerRadius"
    ]
    require(corner_radii and max(corner_radii) <= 8,
            "design-system card radii must not exceed 8px")


def verify_app_and_pages(root: Path) -> None:
    ui_root = root / "src/adapters/ui/winui"
    app_text = read(ui_root / "App.xaml")
    controls_index = app_text.find("XamlControlsResources")
    design_index = app_text.find('Source="Themes/DesignSystem.xaml"')
    require(controls_index >= 0 and design_index > controls_index,
            "App.xaml must merge the design dictionary after framework resources")

    page_paths = sorted((ui_root / "Pages").glob("*.xaml"))
    require(len(page_paths) == 7, "the shared design contract expects seven pages")
    for page_path in page_paths:
        text = read(page_path)
        require("AzzsPageScrollViewerStyle" in text and
                "AzzsPageRootGridStyle" in text and
                "AzzsPageTitleTextStyle" in text,
                f"{page_path.name} must consume the shared page shell")
        require("AutomationProperties.AutomationId" in text,
                f"{page_path.name} needs a stable AutomationId")
        require('<RowDefinition Height="Auto"' in text,
                f"{page_path.name} must use content-sized layout")
        require("AdaptiveTrigger" in text and
                "AzzsPagePaddingNarrow" in text and
                "AzzsPagePadding}" in text,
                f"{page_path.name} must consume narrow and wide page states")

    production_xaml = sorted(ui_root.rglob("*.xaml"))
    resource_path = ui_root / "Themes/DesignSystem.xaml"
    automation_ids: dict[str, Path] = {}
    for xaml_path in production_xaml:
        xaml_root = parse_xml(xaml_path)
        if xaml_path == resource_path:
            continue
        text = read(xaml_path)
        require(not re.search(r'#[0-9A-Fa-f]{6,8}', text),
                f"{xaml_path.name} must not copy colors outside the dictionary")
        require(not re.search(r'Duration\s*=|\d+:\d+:\d+\.\d+', text),
                f"{xaml_path.name} must not copy animation durations")
        require('x:Key="Azzs' not in text,
                f"{xaml_path.name} must not redefine design resources")
        require(not re.search(
                    r"StaticResource Azzs(?:Surface|Text|Status|Risk|Command|Material)[A-Za-z]+Brush",
                    text),
                f"{xaml_path.name} must consume theme brushes with ThemeResource")
        tokenized_metrics = {
            "FontSize", "CornerRadius", "Padding", "Margin", "Spacing",
            "RowSpacing", "ColumnSpacing", "MinHeight", "MinWidth",
        }

        def is_semantic_metric(value: str) -> bool:
            return all(component == "0" or
                       component.startswith("{StaticResource Azzs")
                       for component in value.split(","))

        for element in xaml_root.iter():
            for attribute, value in element.attrib.items():
                attribute_name = local_name(attribute)
                if attribute_name in {"Width", "Height"}:
                    require(value in {"0", "Auto", "*"} or
                            value.startswith("{StaticResource Azzs"),
                            f"{xaml_path.name} must not fix {attribute_name} "
                            f"to {value}")
                if attribute_name not in tokenized_metrics:
                    continue
                require(is_semantic_metric(value),
                        f"{xaml_path.name} must use a semantic resource for "
                        f"{attribute_name}={value}")
            if local_name(element.tag) != "Setter":
                continue
            target = element.attrib.get("Property",
                                        element.attrib.get("Target", ""))
            property_name = target.rsplit(".", 1)[-1].rstrip(")")
            if property_name not in tokenized_metrics:
                continue
            value = element.attrib.get("Value", "")
            require(is_semantic_metric(value),
                    f"{xaml_path.name} setter {target} must use a semantic "
                    "resource")
        for automation_id in re.findall(
                r'AutomationProperties\.AutomationId="([^"]+)"', text):
            require(automation_id not in automation_ids,
                    f"duplicate AutomationId {automation_id} in "
                    f"{automation_ids.get(automation_id)} and {xaml_path}")
            automation_ids[automation_id] = xaml_path

    required_shell_ids = {
        "AzzsPrimaryNavigation",
        "AzzsNavigationOverview",
        "AzzsNavigationDrivers",
        "AzzsNavigationSystemOptimization",
        "AzzsNavigationSoftwareInstallation",
        "AzzsNavigationSoftwareOptimization",
        "AzzsNavigationHistoryAndLogs",
        "AzzsNavigationApplicationSettings",
        "AzzsVersionRiskInfoBar",
        "AzzsContentFrame",
    }
    require(required_shell_ids <= set(automation_ids),
            "shell and seven navigation destinations need stable AutomationIds")


def verify_fixture_xaml(root: Path) -> None:
    fixture_path = root / (
        "src/adapters/ui/winui/DesignSystem/Fixtures/"
        "DesignSystemFixturePage.xaml"
    )
    fixture_root = parse_xml(fixture_path)
    fixture_text = read(fixture_path)
    require("VisualStateManager.VisualStateGroups" in fixture_text and
            "AdaptiveTrigger" in fixture_text and
            "AdvancedViewPanel.(Grid.Row)" in fixture_text and
            "ReadOnlyField.(Grid.Column)" in fixture_text,
            "the fixture must stack shared views and settings fields when narrow")
    require("ItemsWrapGrid" in fixture_text,
            "the four-stage fixture must wrap instead of assuming fixed width")
    require("TextWrapping" not in fixture_text or "AzzsBodyTextStyle" in fixture_text,
            "fixture text must inherit the shared wrapping contract")
    require("Click=" not in fixture_text and "Tapped=" not in fixture_text,
            "the fixed fixture must have no side-effect event handlers")

    surfaces = [element for element in fixture_root.iter()
                if local_name(element.tag) == "ReadOnlyPresentationSurface"]
    require(len(surfaces) >= 19,
            "the fixture must compile the reusable projection surface across "
            "lists, detail, status, progress, risk, disabled, and result states")
    require(not any(local_name(element.tag) == "ProgressBar"
                    for element in fixture_root.iter()),
            "the fixture must not duplicate the shared progress projection")

    interactive_tags = {"Button", "Expander", "TextBox"}
    for element in fixture_root.iter():
        if local_name(element.tag) not in interactive_tags:
            continue
        if element.attrib.get("IsEnabled") == "False":
            continue
        require("TabIndex" in element.attrib,
                f"enabled {local_name(element.tag)} needs a stable TabIndex")
        require(element.attrib.get("AutomationProperties.AutomationId"),
                f"enabled {local_name(element.tag)} needs an AutomationId")

    fixture_cpp_path = fixture_path.with_suffix(".xaml.cpp")
    fixture_cpp = read(fixture_cpp_path)
    require(re.search(
                r"project_surface\(StandardSharedSurface\(\).*?"
                r'"fixture\.shared-view".*?ViewMode::standard',
                fixture_cpp, re.DOTALL) is not None and
            re.search(
                r"project_surface\(AdvancedSharedSurface\(\).*?"
                r'"fixture\.shared-view".*?ViewMode::advanced',
                fixture_cpp, re.DOTALL) is not None,
            "standard and advanced fixture views must project one source id")
    require("project_stage" in fixture_cpp and
            "AutomationProperties::SetAutomationId" in fixture_cpp and
            "project_intent" in fixture_cpp,
            "fixture fields and stages must project the typed source")
    require(
        re.search(
            r"project_surface\(DeterminateProgressSurface\(\).*?"
            r'"fixture\.determinate-progress"',
            fixture_cpp,
            re.DOTALL,
        ) is not None and
        re.search(
            r"project_surface\(UnknownProgressSurface\(\).*?"
            r'"fixture\.unknown-progress"',
            fixture_cpp,
            re.DOTALL,
        ) is not None and
        "UnknownProgressBar" not in fixture_cpp,
        "fixture progress cases must consume the reusable projection surface",
    )

    control_path = root / (
        "src/adapters/ui/winui/DesignSystem/Controls/"
        "ReadOnlyPresentationSurface.xaml"
    )
    control_root = parse_xml(control_path)
    require(len([element for element in control_root.iter()
                 if local_name(element.tag) == "InfoBar"]) == 1,
            "the reusable projection surface must own one native status band")
    require(len([element for element in control_root.iter()
                 if local_name(element.tag) == "ProgressBar"]) == 1,
            "the reusable projection surface must own one native progress bar")
    control_cpp = read(control_path.with_suffix(".xaml.cpp"))
    control_header = read(control_path.with_suffix(".xaml.h"))
    require("shared_ptr<azzs::ui::presentation::PresentationSnapshot const>" in
            control_header and "IntentHandler" in control_header and
            "Workbench" not in control_header,
            "the reusable surface must accept immutable presentation data only")
    for token in (
        "button.Click", "presentation_->intent_for", "SetAutomationId",
        "SetName", "SetLiveSetting", "SetHelpText", "default_focus",
        "role != CommandRole::danger", "last_announcement_key_", "get_weak",
        "focus_default_when_loaded_", "component->progress",
        "ProgressKind::determinate", "ProgressKind::indeterminate",
        "ProgressKind::unknown", "clear_numeric_progress",
        "AutomationProperties::SetItemStatus", "ProjectedProgressBar().Maximum",
        "ProjectedProgressBar().Value", "ProgressValueText().Text",
    ):
        require(token in control_cpp,
                f"the reusable typed-intent surface is missing {token}")
    require(control_cpp.count("AutomationProperties::SetItemStatus") == 2,
            "progress status must be set when visible and cleared when hidden")
    require("AutomationProperties::SetValue" not in control_cpp,
            "AutomationProperties has no SetValue API; use ItemStatus")
    require("PointerEntered" not in control_cpp and
            "PointerMoved" not in control_cpp and
            "PointerEntered=" not in read(control_path),
            "hover may only enhance the native control input path")
    forbidden_side_effects = {
        "std::filesystem",
        "std::fstream",
        "std::ofstream",
        "ShellExecute",
        "CreateProcess",
        "RegSetValue",
        "WinHttp",
        "download(",
        "install(",
        "apply_system_change(",
    }
    fixture_implementation = fixture_cpp + control_cpp
    require(not any(token in fixture_implementation
                    for token in forbidden_side_effects),
            "the fixture and reusable surface must not perform system work")

    production_contract = read(root / (
        "src/adapters/ui/winui/DesignSystem/presentation_contract.cpp"
    )) + read(root / (
        "src/adapters/ui/winui/DesignSystem/presentation_contract.hpp"
    ))
    require("make_design_system_fixture" not in production_contract and
            "fixture." not in production_contract,
            "fixed fixture data must stay out of the production contract")

    fixture_data = read(root / (
        "src/adapters/ui/winui/DesignSystem/Fixtures/"
        "design_system_fixture.cpp"
    ))
    for marker in (
        "fixture.long-chinese",
        "fixture.determinate-progress",
        "fixture.unknown-progress",
        "fixture.inline-error",
        "fixture.waiting-restart",
        "fixture.emergency-withdrawal",
        "fixture.pending-confirmation",
        "fixture.local-trial",
        "fixture.shared-view",
        "fixture.recovered-unsaved",
        "fixture.saved-not-applied",
        "fixture.source-handoff",
        "fixture.waiting-network",
        "fixture.catalog-editor",
    ):
        require(marker in fixture_data, f"missing fixed fixture: {marker}")
    stage_positions = [
        fixture_data.find("fixture.stage.drivers"),
        fixture_data.find("fixture.stage.system-optimization"),
        fixture_data.find("fixture.stage.software-installation"),
        fixture_data.find("fixture.stage.software-optimization"),
    ]
    require(stage_positions == sorted(stage_positions) and
            all(position >= 0 for position in stage_positions),
            "the fixture must freeze the four-stage order")


def verify_motion_and_ownership(root: Path) -> None:
    ui_root = root / "src/adapters/ui/winui"
    source_paths = sorted(
        [*ui_root.rglob("*.cpp"), *ui_root.rglob("*.h"), *ui_root.rglob("*.hpp")]
    )
    animations_enabled_owners = []
    source_texts: dict[Path, str] = {}
    for source_path in source_paths:
        text = read(source_path)
        source_texts[source_path] = text
        if "AnimationsEnabled" in text:
            animations_enabled_owners.append(source_path)

    expected_owner = ui_root / "DesignSystem/motion_preferences.cpp"
    require(animations_enabled_owners == [expected_owner],
            "UISettings.AnimationsEnabled must have one centralized owner")
    owner_text = source_texts[expected_owner]
    require("AnimationsEnabledChanged" in owner_text and
            "GetForCurrentThread" in owner_text and
            "weak_ptr" in owner_text and
            "AnimationsEnabledChanged(animations_enabled_changed_token_)" in owner_text,
            "motion preference changes need dispatch, safe lifetime, and revocation")
    require("apply_animations_enabled" in owner_text and "handlers" in owner_text,
            "system disable must synchronously cancel registered visual motion")

    motion_contract = read(ui_root / "DesignSystem/motion_contract.hpp")
    for immediate_path in (
        "keyboard_command", "top_level_navigation", "view_mode_switch",
        "continuous_selection", "progress_value_refresh", "log_refresh",
    ):
        require(immediate_path in motion_contract,
                f"the 0ms motion contract is missing {immediate_path}")

    all_source = "\n".join(source_texts.values())
    require(".Completed(" not in all_source and "Completed +=" not in all_source,
            "animation completion must not become a business boundary")

    xaml_paths = sorted(ui_root.rglob("*.xaml"))
    xaml_text = "\n".join(read(path) for path in xaml_paths)
    require('EnableDependentAnimation="True"' not in xaml_text,
            "dependent layout animations are forbidden")
    require("Completed=" not in xaml_text,
            "XAML animation completion must not become a business boundary")
    layout_animation_properties = (
        "Width", "Height", "Margin", "Padding", "GridLength",
        "RowDefinition", "ColumnDefinition",
    )
    for xaml_path in xaml_paths:
        xaml_root = parse_xml(xaml_path)
        for element in xaml_root.iter():
            element_name = local_name(element.tag)
            for attribute, value in element.attrib.items():
                if local_name(attribute) in {"Scale", "ScaleX", "ScaleY"}:
                    require(not re.fullmatch(r"0(?:\.0+)?", value),
                            f"{xaml_path.name} must not start at scale zero")
            if "Animation" not in element_name:
                continue
            target = " ".join(
                value for attribute, value in element.attrib.items()
                if "TargetProperty" in local_name(attribute) or
                local_name(attribute) == "Property"
            )
            require(not any(token in target
                            for token in layout_animation_properties),
                    f"{xaml_path.name} animates a layout property: {target}")
            if "Scale" in target:
                start = element.attrib.get("From", "")
                require(not re.fullmatch(r"0(?:\.0+)?", start),
                        f"{xaml_path.name} animates scale from zero")
                first_keyframe = next(iter(element), None)
                if first_keyframe is not None:
                    first_value = first_keyframe.attrib.get("Value", "")
                    require(not re.fullmatch(r"0(?:\.0+)?", first_value),
                            f"{xaml_path.name} keyframes scale from zero")

    main_window_cpp = read(ui_root / "MainWindow.xaml.cpp")
    require("SuppressNavigationTransitionInfo" in main_window_cpp,
            "top-level navigation must remain immediate")
    require("NavigationThemeTransition" not in main_window_cpp and
            "DrillInNavigationTransitionInfo" not in main_window_cpp,
            "top-level navigation must not add a page transition")


def verify_xaml_project_metadata(root: Path) -> None:
    project_path = root / "src/adapters/ui/winui/Azzs.WinUI.vcxproj"
    project_root = parse_xml(project_path)
    ns = {"m": MSBUILD_NS}

    pages = {
        normalized_project_path(element.attrib["Include"])
        for element in project_root.findall(".//m:Page", ns)
    }
    cl_includes = {
        normalized_project_path(element.attrib["Include"]): element
        for element in project_root.findall(".//m:ClInclude", ns)
    }
    cl_compiles = {
        normalized_project_path(element.attrib["Include"])
        for element in project_root.findall(".//m:ClCompile", ns)
        if "Include" in element.attrib
    }
    midl = {
        normalized_project_path(element.attrib["Include"])
        for element in project_root.findall(".//m:Midl", ns)
    }

    require("Themes/DesignSystem.xaml" in pages,
            "the independent design ResourceDictionary must be a Page item")
    require("DesignSystem/Controls/ReadOnlyPresentationSurface.xaml" in pages,
            "the typed-intent projection surface must compile on Windows")
    fixture_xaml = "DesignSystem/Fixtures/DesignSystemFixturePage.xaml"
    require(fixture_xaml in pages, "the UI fixture XAML must compile on Windows")

    project_dir = project_path.parent
    for page in pages:
        page_path = project_dir / page
        page_text = read(page_path)
        if "x:Class=" not in page_text:
            continue
        stem = page.removesuffix(".xaml")
        header = f"{stem}.xaml.h"
        source = f"{stem}.xaml.cpp"
        idl = f"{stem}.idl"
        require(header in cl_includes,
                f"{page} is missing its generated-header metadata")
        require(source in cl_compiles, f"{page} is missing its code-behind")
        require(idl in midl, f"{page} is missing its MIDL runtimeclass")
        dependent = cl_includes[header].find("m:DependentUpon", ns)
        require(dependent is not None and dependent.text == Path(page).name,
                f"{header} DependentUpon must be the XAML basename")

    required_native_sources = {
        "DesignSystem/motion_contract.hpp",
        "DesignSystem/motion_preferences.hpp",
        "DesignSystem/presentation_contract.hpp",
        "DesignSystem/Fixtures/design_system_fixture.hpp",
    }
    require(required_native_sources <= set(cl_includes),
            "native design-system contract headers must compile in the host")
    require({
        "DesignSystem/motion_preferences.cpp",
        "DesignSystem/presentation_contract.cpp",
        "DesignSystem/Fixtures/design_system_fixture.cpp",
    } <= cl_compiles, "design-system implementations must compile in the host")
    for portable_source in (
        "DesignSystem/presentation_contract.cpp",
        "DesignSystem/Fixtures/design_system_fixture.cpp",
    ):
        source_item = next(
            element for element in project_root.findall(".//m:ClCompile", ns)
            if normalized_project_path(element.attrib.get("Include", "")) ==
            portable_source
        )
        precompiled_header = source_item.find("m:PrecompiledHeader", ns)
        require(precompiled_header is not None and
                precompiled_header.text == "NotUsing",
                f"{portable_source} must opt out of the WinUI PCH")


def verify_localization_and_workflow_boundary(root: Path) -> None:
    resources_path = root / (
        "src/adapters/ui/winui/Strings/zh-CN/Resources.resw"
    )
    resources = parse_xml(resources_path)
    resource_names = {
        element.attrib["name"] for element in resources.findall("data")
    }
    for required in (
        "MainWindowTitle",
        "NavigationOverview.Content",
        "NavigationApplicationSettings.Content",
        "VersionRiskTitle",
    ):
        require(required in resource_names,
                f"existing localized shell resource disappeared: {required}")

    required_settings_resources = {
        "ApplicationSettingsCatalogHeading.Text",
        "ApplicationSettingsCacheTitle.Text",
        "ApplicationSettingsArchitectureTitle.Text",
        "ApplicationSettingsLogsTitle.Text",
        "ApplicationSettingsRecoveryTitle.Text",
        "ApplicationSettingsDebugTitle.Text",
        "ApplicationSettingsClearCacheDialogTitle",
        "ApplicationSettingsClearLogsDialogTitle",
        "ApplicationSettingsDeleteRecoveryDialogTitle",
        "ApplicationSettingsCatalogDialogTitle",
    }
    require(required_settings_resources <= resource_names,
            "application settings must retain localized section and confirmation text")

    fixture_xaml = read(root / (
        "src/adapters/ui/winui/DesignSystem/Fixtures/"
        "DesignSystemFixturePage.xaml"
    ))
    require("Click=" not in fixture_xaml and "Command=" not in fixture_xaml,
            "the fixture may show commands but must not execute business work")

    settings_xaml = read(root / (
        "src/adapters/ui/winui/Pages/ApplicationSettingsPage.xaml"
    ))
    settings_cpp = read(root / (
        "src/adapters/ui/winui/Pages/ApplicationSettingsPage.xaml.cpp"
    ))
    settings_service_header = read(root / (
        "src/application/application-settings/include/azzs/application/"
        "application_settings.hpp"
    ))
    settings_service_cpp = read(root / (
        "src/application/application-settings/src/application_settings.cpp"
    ))
    workbench_services_header = read(root / (
        "src/application/include/azzs/application/workbench_services.hpp"
    ))
    main_window_cpp = read(root / "src/adapters/ui/winui/MainWindow.xaml.cpp")
    composition_root_cpp = read(root / "src/composition/windows/composition_root.cpp")
    view_preferences_cpp = read(root / (
        "src/adapters/windows/src/windows_view_preferences.cpp"
    ))
    system_optimization_cpp = read(root / (
        "src/adapters/ui/winui/Pages/SystemOptimizationPage.xaml.cpp"
    ))
    require("AzzsApplicationAdvancedView" in settings_xaml and
            "OnAdvancedViewToggled" in settings_cpp,
            "application settings must own the advanced-view toggle")
    require("AdvancedViewPreferences" in main_window_cpp and
            "WindowsViewPreferences" in composition_root_cpp and
            "AzzsAdvancedView" in view_preferences_cpp and
            "LocalSettings" in view_preferences_cpp and
            "bind(system_settings_, advanced_view_)" in main_window_cpp,
            "the platform adapter must persist and the shell must pass one advanced-view preference")
    require("force_attempt_confirmed = true" in system_optimization_cpp and
            "SystemSettingsForceAttemptDialogBody" in system_optimization_cpp,
            "the force attempt must submit the existing typed intent after risk confirmation")
    require("advanced_view_" in system_optimization_cpp and
            "SystemSettingsForceAttemptButtonText" in system_optimization_cpp,
            "only the system-optimization advanced projection may expose force attempt")
    require('x:Uid="ApplicationSettingsCatalogHeading"' in settings_xaml,
            "the application settings catalog heading must resolve its localized text")
    for automation_id in (
        "AzzsApplicationSettingsPage",
        "AzzsApplicationAdvancedView",
        "AzzsApplicationUpdateCommand",
        "AzzsApplicationCacheRetention",
        "AzzsApplicationArchitecturePreference",
        "AzzsApplicationClearCache",
        "AzzsApplicationClearLogs",
        "AzzsApplicationDeleteRecoveryRecord",
        "AzzsApplicationDebugMode",
    ):
        require(automation_id in settings_xaml,
                f"application settings is missing AutomationId {automation_id}")
    require(settings_cpp.count("ContentDialog dialog;") >= 4 and
            "settings_->clear_cache(false)" in settings_cpp and
            "settings_->clear_cache(true)" in settings_cpp and
            "settings_->clear_logs(false)" in settings_cpp and
            "settings_->clear_logs(true)" in settings_cpp and
            "settings_->delete_recovery_record(*record_id, false)" in settings_cpp and
            "settings_->delete_recovery_record(*record_id, true)" in settings_cpp and
            "confirm_catalog_change(" in settings_cpp,
            "application settings must gate destructive work behind native confirmation")
    require(not any(token in settings_xaml or token in settings_cpp for token in (
                "Storyboard", "ConnectedAnimation", "CompositionAnimation")),
            "application settings must keep high-frequency settings changes static")
    require("ApplicationSettingsDebugProvider" in settings_service_header and
            "UnavailableApplicationSettingsDebugProvider" in composition_root_cpp and
            "application_settings()" in workbench_services_header and
            "application_settings()" in main_window_cpp and
            "requires_explicit_confirmation" in settings_service_cpp,
            "settings must use injected debug ownership and application confirmation gates")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_design_system.py <repository-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()

    try:
        verify_resource_dictionary(root)
        verify_app_and_pages(root)
        verify_fixture_xaml(root)
        verify_motion_and_ownership(root)
        verify_xaml_project_metadata(root)
        verify_localization_and_workflow_boundary(root)
    except ContractFailure as error:
        print(f"UI design contract failed: {error}", file=sys.stderr)
        return 1

    print(
        "UI design contract passed: resources, motion, XAML metadata, "
        "AutomationId, fixture, and ownership"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
