#include "pch.h"

#include "SystemOptimizationPage.xaml.h"

#include <string>
#include <string_view>

#include "azzs/application/system_settings_apply.hpp"

#if __has_include("Pages/SystemOptimizationPage.g.cpp")
#include "Pages/SystemOptimizationPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {
namespace {

[[nodiscard]] std::wstring resource_with_token(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader
        const& resources,
    wchar_t const* key, std::wstring_view token,
    std::wstring_view replacement) {
  auto value = std::wstring{resources.GetString(key)};
  auto const position = value.find(token);
  if (position != std::wstring::npos) {
    value.replace(position, token.size(), replacement);
  }
  return value;
}

[[nodiscard]] std::wstring windows_version_text(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader
        const& resources,
    azzs::application::settings_domain::WindowsVersion const& version) {
  auto value = std::wstring{resources.GetString(L"SystemSettingsWindowsVersion")};
  auto replace = [&](std::wstring_view token, std::wstring replacement) {
    auto const position = value.find(token);
    if (position != std::wstring::npos) {
      value.replace(position, token.size(), replacement);
    }
  };
  replace(L"{generation}", std::to_wstring(static_cast<std::uint8_t>(
                              version.generation)));
  replace(L"{year}", std::to_wstring(version.feature_update_year));
  replace(L"{half}", std::to_wstring(version.feature_update_half));
  return value;
}

[[nodiscard]] std::wstring known_range_text(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader
        const& resources,
    azzs::application::settings_domain::WindowsVersionRange const& range) {
  auto const minimum = range.minimum.has_value()
                           ? windows_version_text(resources, *range.minimum)
                           : std::wstring{};
  auto const maximum = range.maximum.has_value()
                           ? windows_version_text(resources, *range.maximum)
                           : std::wstring{};
  if (!minimum.empty() && !maximum.empty()) {
    return resource_with_token(
        resources, L"SystemSettingsKnownRange", L"{range}",
        minimum + L" - " + maximum);
  }
  if (!minimum.empty()) {
    return resource_with_token(resources, L"SystemSettingsMinimumRange",
                               L"{version}", minimum);
  }
  if (!maximum.empty()) {
    return resource_with_token(resources, L"SystemSettingsMaximumRange",
                               L"{version}", maximum);
  }
  return std::wstring{resources.GetString(L"SystemSettingsUnknownRange")};
}

}  // namespace

SystemOptimizationPage::SystemOptimizationPage() {
  InitializeComponent();
}

void SystemOptimizationPage::bind(
    std::shared_ptr<azzs::application::SystemSettingsApplyService> service) {
  service_ = std::move(service);
  project(service_->refresh());
}

void SystemOptimizationPage::OnSelectRecommended(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!service_) {
    return;
  }
  auto const snapshot = service_->snapshot();
  if (snapshot.recommended_plan.has_value()) {
    static_cast<void>(
        service_->select_recommended_plan(*snapshot.recommended_plan));
  }
  project(service_->snapshot());
}

void SystemOptimizationPage::OnApplySelected(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!service_) {
    return;
  }
  static_cast<void>(service_->apply_selected());
  project(service_->snapshot());
}

void SystemOptimizationPage::OnRestartExplorerNow(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!service_) {
    return;
  }
  static_cast<void>(service_->restart_explorer_now());
  project(service_->snapshot());
}

void SystemOptimizationPage::OnSettingSelectionChanged(
    winrt::Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!service_) {
    return;
  }
  auto const check_box =
      sender.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
  if (!check_box) {
    return;
  }
  auto const id = winrt::unbox_value<winrt::hstring>(check_box.Tag());
  auto const checked = check_box.IsChecked();
  static_cast<void>(service_->set_selected(
      azzs::application::settings_domain::StableId{winrt::to_string(id)},
      checked ? checked.Value() : false));
  project(service_->snapshot());
}

void SystemOptimizationPage::project(
    azzs::application::SystemSettingsApplySnapshot const& snapshot) {
  using azzs::application::SystemSettingApplyState;
  using azzs::application::SystemSettingsSnapshotStatus;
  using winrt::Microsoft::UI::Xaml::Application;
  using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
  using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
  using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

  auto const resources = ResourceLoader{};

  if (snapshot.status == SystemSettingsSnapshotStatus::unavailable) {
    SystemSettingsStatus().Severity(InfoBarSeverity::Warning);
    SystemSettingsStatus().Message(winrt::to_hstring(snapshot.detail));
    SystemSettingsStatus().IsOpen(true);
    RecommendedSummary().Text(
        resources.GetString(L"SystemSettingsUnavailableSummary"));
    SelectRecommendedButton().IsEnabled(false);
    ApplySelectedButton().IsEnabled(false);
    RestartExplorerButton().Visibility(
        winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    RestartExplorerNotice().Visibility(
        winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    SettingsItems().Children().Clear();
    return;
  }

  SystemSettingsStatus().IsOpen(false);
  SelectRecommendedButton().IsEnabled(snapshot.recommended_plan.has_value());
  RecommendedSummary().Text(
      snapshot.selected_plan.has_value()
          ? winrt::to_hstring(snapshot.plan_description)
          : resources.GetString(L"SystemSettingsNoPlanSummary"));
  ApplySelectedButton().IsEnabled(snapshot.can_apply);
  RestartExplorerButton().Visibility(
      snapshot.waiting_for_explorer_restart
          ? winrt::Microsoft::UI::Xaml::Visibility::Visible
          : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
  RestartExplorerNotice().Visibility(
      snapshot.waiting_for_explorer_restart
          ? winrt::Microsoft::UI::Xaml::Visibility::Visible
          : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

  auto const metadata_style = Application::Current().Resources().Lookup(
      winrt::box_value(L"AzzsMetadataTextStyle"))
                                  .as<winrt::Microsoft::UI::Xaml::Style>();
  SettingsItems().Children().Clear();
  for (auto const& item : snapshot.settings) {
    auto row = winrt::Microsoft::UI::Xaml::Controls::StackPanel{};
    row.Spacing(4);
    auto check_box = winrt::Microsoft::UI::Xaml::Controls::CheckBox{};
    check_box.Content(winrt::box_value(winrt::to_hstring(item.display_name)));
    check_box.IsChecked(item.selected);
    check_box.IsEnabled(item.applicable);
    check_box.Tag(winrt::box_value(winrt::to_hstring(item.id.value)));
    check_box.Checked(
        {this, &SystemOptimizationPage::OnSettingSelectionChanged});
    check_box.Unchecked(
        {this, &SystemOptimizationPage::OnSettingSelectionChanged});
    AutomationProperties::SetName(
        check_box,
        winrt::hstring{resource_with_token(
            resources, L"SystemSettingsSelectionName", L"{name}",
            std::wstring{winrt::to_hstring(item.display_name)})});
    auto description = TextBlock{};
    description.Style(metadata_style);
    description.Text(winrt::to_hstring(item.description));
    auto state = TextBlock{};
    state.Style(metadata_style);
    state.Text(winrt::to_hstring(
        item.state == SystemSettingApplyState::already_effective
            ? winrt::to_string(
                  resources.GetString(L"SystemSettingsAlreadyEffective"))
            : item.state == SystemSettingApplyState::waiting_explorer_restart
                  ? winrt::to_string(resources.GetString(
                        L"SystemSettingsWaitingExplorerRestart"))
                  : item.detail));
    auto scope = TextBlock{};
    scope.Style(metadata_style);
    scope.Text(winrt::hstring{known_range_text(resources,
                                                item.known_windows_range)});
    auto restart = TextBlock{};
    restart.Style(metadata_style);
    restart.Text(item.restart_requirement ==
                         azzs::application::settings_domain::
                             RestartRequirement::explorer
                     ? resources.GetString(
                           L"SystemSettingsExplorerRestartRequired")
                     : item.restart_requirement ==
                               azzs::application::settings_domain::
                                   RestartRequirement::windows
                           ? resources.GetString(
                                 L"SystemSettingsWindowsRestartRequired")
                           : resources.GetString(
                                 L"SystemSettingsNoRestartRequired"));
    auto recovery = TextBlock{};
    recovery.Style(metadata_style);
    recovery.Text(item.recovery_available
                      ? resources.GetString(L"SystemSettingsRecoveryAvailable")
                      : resources.GetString(
                            L"SystemSettingsRecoveryUnavailable"));
    row.Children().Append(check_box);
    row.Children().Append(description);
    row.Children().Append(state);
    row.Children().Append(scope);
    row.Children().Append(restart);
    row.Children().Append(recovery);
    if (item.source_url.has_value()) {
      auto source = TextBlock{};
      source.Style(metadata_style);
      source.Text(winrt::hstring{resource_with_token(
          resources, L"SystemSettingsSource", L"{source}",
          std::wstring{winrt::to_hstring(*item.source_url)})});
      row.Children().Append(source);
    }
    SettingsItems().Children().Append(row);
  }
  AutomationProperties::SetName(
      SettingsItems(),
      winrt::hstring{resource_with_token(
          resources, L"SystemSettingsItemsName", L"{count}",
          std::to_wstring(snapshot.settings.size()))});
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
