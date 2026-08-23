#include "pch.h"

#include "ApplicationSettingsPage.xaml.h"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#if __has_include("Pages/ApplicationSettingsPage.g.cpp")
#include "Pages/ApplicationSettingsPage.g.cpp"
#endif

namespace {

using azzs::application::ApplicationSettingsActionCode;
using azzs::application::ApplicationSettingsCatalog;
using azzs::application::ApplicationSettingsCatalogAction;
using azzs::domain::architecture_selection::ArchitecturePreference;
using azzs::domain::offline_package_cache::CacheLocationKind;
using azzs::domain::offline_package_cache::CacheRetentionPolicy;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialog;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult;
using winrt::Microsoft::UI::Xaml::Controls::InfoBar;
using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
using winrt::Microsoft::UI::Xaml::Visibility;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

[[nodiscard]] wchar_t const* resource_fallback(wchar_t const* key) noexcept {
  std::wstring_view const name{key == nullptr ? L"" : key};
  if (name == L"ApplicationUpdateCheckButton") {
    return L"\u68C0\u67E5\u66F4\u65B0";
  }
  if (name == L"ApplicationUpdateManualButton") {
    return L"\u6253\u5F00 GitHub \u4E0B\u8F7D";
  }
  if (name == L"ApplicationUpdateDiagnosticButton") {
    return L"\u5BFC\u51FA\u8BCA\u65AD\u8D44\u6599";
  }
  if (name == L"ApplicationUpdateTitle.Text") {
    return L"\u5E94\u7528\u66F4\u65B0";
  }
  if (name.find(L"ApplicationSettingsAction") == 0) {
    return L"\u64CD\u4F5C\u672A\u5B8C\u6210\uFF1B\u73B0\u6709\u72B6\u6001\u5DF2\u4FDD\u7559\u3002";
  }
  if (name == L"ApplicationSettingsOperationTitle") {
    return L"\u8BBE\u7F6E\u64CD\u4F5C";
  }
  if (name.find(L"ApplicationSettingsCache") == 0) {
    return L"\u7F13\u5B58\u4FE1\u606F\u6682\u65F6\u4E0D\u53EF\u7528";
  }
  if (name == L"ApplicationSettingsArchitectureTitle.Text") {
    return L"\u8F6F\u4EF6\u5305\u67B6\u6784";
  }
  if (name == L"ApplicationSettingsArchitecturePreference.Header") {
    return L"\u8F6F\u4EF6\u5305\u67B6\u504F\u597D";
  }
  if (name == L"ApplicationSettingsArchitecturePrompt.Content") {
    return L"\u4F18\u5148 ARM64\uFF0Cx64 \u56DE\u9000\u524D\u8BE2\u95EE";
  }
  if (name == L"ApplicationSettingsArchitectureAutoFallback.Content") {
    return L"\u4F18\u5148 ARM64\uFF0C\u81EA\u52A8\u56DE\u9000 x64";
  }
  if (name == L"ApplicationSettingsArchitecturePreferX64.Content") {
    return L"\u4F18\u5148 x64 \u517C\u5BB9\u5305";
  }
  if (name.find(L"ApplicationSettingsArchitecture") == 0) {
    return L"\u8F6F\u4EF6\u5305\u67B6\u6784";
  }
  if (name.find(L"ApplicationSettingsCatalog") == 0) {
    return L"\u76EE\u5F55\u4FE1\u606F\u6682\u65F6\u4E0D\u53EF\u7528";
  }
  if (name.find(L"ApplicationSettingsLogs") == 0) {
    return L"\u65E5\u5FD7\u4E0E\u8BCA\u65AD";
  }
  if (name.find(L"ApplicationSettingsRecovery") == 0) {
    return L"\u6062\u590D\u8BB0\u5F55";
  }
  if (name.find(L"ApplicationSettingsDebug") == 0) {
    return L"\u8C03\u8BD5\u5DE5\u5177";
  }
  if (name.find(L"ApplicationUpdate") == 0) {
    return L"\u5E94\u7528\u66F4\u65B0\u4FE1\u606F\u6682\u65F6\u4E0D\u53EF\u7528";
  }
  return L"\u8BBE\u7F6E\u8D44\u6E90\u6682\u65F6\u4E0D\u53EF\u7528";
}

[[nodiscard]] winrt::hstring resource_string(wchar_t const* key) noexcept {
  try {
    auto const value = ResourceLoader{}.GetString(key);
    if (!value.empty()) {
      return value;
    }
    return winrt::hstring{resource_fallback(key)};
  } catch (...) {
    // Resource lookup is presentation-only. A missing PRI must not prevent
    // the already constructed settings page from being displayed.
    return winrt::hstring{resource_fallback(key)};
  }
}

void set_settings_operation_open(InfoBar const& info_bar, bool open) {
  if (open) {
    info_bar.Visibility(Visibility::Visible);
    info_bar.IsOpen(true);
    return;
  }
  info_bar.IsOpen(false);
  info_bar.Visibility(Visibility::Collapsed);
}

void set_update_status_open(InfoBar const& info_bar, bool open) {
  if (open) {
    info_bar.Visibility(Visibility::Visible);
    info_bar.IsOpen(true);
    return;
  }
  info_bar.IsOpen(false);
  info_bar.Visibility(Visibility::Collapsed);
}

void replace_token(std::wstring& value, std::wstring_view token,
                   std::wstring_view replacement) {
  auto position = value.find(token);
  while (position != std::wstring::npos) {
    value.replace(position, token.size(), replacement);
    position = value.find(token, position + replacement.size());
  }
}

[[nodiscard]] std::int32_t retention_index(CacheRetentionPolicy retention) {
  switch (retention) {
    case CacheRetentionPolicy::delete_immediately:
      return 0;
    case CacheRetentionPolicy::retain_seven_days:
      return 1;
    case CacheRetentionPolicy::retain_thirty_days:
      return 2;
    case CacheRetentionPolicy::retain_indefinitely:
      return 3;
  }
  return 1;
}

[[nodiscard]] CacheRetentionPolicy retention_for_index(std::int32_t index) {
  switch (index) {
    case 0:
      return CacheRetentionPolicy::delete_immediately;
    case 1:
      return CacheRetentionPolicy::retain_seven_days;
    case 2:
      return CacheRetentionPolicy::retain_thirty_days;
    case 3:
      return CacheRetentionPolicy::retain_indefinitely;
    default:
      return CacheRetentionPolicy::retain_seven_days;
  }
}

[[nodiscard]] std::int32_t architecture_index(ArchitecturePreference preference) {
  switch (preference) {
    case ArchitecturePreference::prefer_arm64_prompt_fallback:
      return 0;
    case ArchitecturePreference::prefer_arm64_auto_fallback:
      return 1;
    case ArchitecturePreference::prefer_x64:
      return 2;
  }
  return 0;
}

[[nodiscard]] ArchitecturePreference architecture_for_index(std::int32_t index) {
  switch (index) {
    case 0:
      return ArchitecturePreference::prefer_arm64_prompt_fallback;
    case 1:
      return ArchitecturePreference::prefer_arm64_auto_fallback;
    case 2:
      return ArchitecturePreference::prefer_x64;
    default:
      return ArchitecturePreference::prefer_arm64_prompt_fallback;
  }
}

[[nodiscard]] constexpr bool current_package_is_x64() noexcept {
#if defined(_M_X64)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] winrt::hstring cache_location_text(CacheLocationKind kind) {
  switch (kind) {
    case CacheLocationKind::system_directory:
      return resource_string(L"ApplicationSettingsCacheLocationSystem");
    case CacheLocationKind::local_volume:
      return resource_string(L"ApplicationSettingsCacheLocationLocal");
    case CacheLocationKind::network_share:
      return resource_string(L"ApplicationSettingsCacheLocationNetwork");
    case CacheLocationKind::removable_media:
      return resource_string(L"ApplicationSettingsCacheLocationRemovable");
  }
  return resource_string(L"ApplicationSettingsCacheLocationUnavailable");
}

[[nodiscard]] winrt::hstring action_message(ApplicationSettingsActionCode code) {
  switch (code) {
    case ApplicationSettingsActionCode::completed:
      return resource_string(L"ApplicationSettingsActionCompleted");
    case ApplicationSettingsActionCode::confirmation_required:
      return resource_string(L"ApplicationSettingsActionConfirmationRequired");
    case ApplicationSettingsActionCode::no_change:
      return resource_string(L"ApplicationSettingsActionNoChange");
    case ApplicationSettingsActionCode::unavailable:
      return resource_string(L"ApplicationSettingsActionUnavailable");
    case ApplicationSettingsActionCode::protected_operation:
      return resource_string(L"ApplicationSettingsActionProtected");
    case ApplicationSettingsActionCode::rejected:
      return resource_string(L"ApplicationSettingsActionRejected");
    case ApplicationSettingsActionCode::failed:
      return resource_string(L"ApplicationSettingsActionFailed");
  }
  return resource_string(L"ApplicationSettingsActionFailed");
}

[[nodiscard]] InfoBarSeverity action_severity(ApplicationSettingsActionCode code) {
  switch (code) {
    case ApplicationSettingsActionCode::completed:
    case ApplicationSettingsActionCode::no_change:
      return InfoBarSeverity::Success;
    case ApplicationSettingsActionCode::confirmation_required:
    case ApplicationSettingsActionCode::unavailable:
    case ApplicationSettingsActionCode::protected_operation:
      return InfoBarSeverity::Warning;
    case ApplicationSettingsActionCode::rejected:
    case ApplicationSettingsActionCode::failed:
      return InfoBarSeverity::Error;
  }
  return InfoBarSeverity::Error;
}

}  // namespace

namespace winrt::Azzs::Ui::Pages::implementation {

ApplicationSettingsPage::ApplicationSettingsPage() {
  InitializeComponent();
  ApplicationUpdateCommandButton().Content(
      winrt::box_value(resource_string(L"ApplicationUpdateCheckButton")));
  ApplicationUpdateManualButton().Content(
      winrt::box_value(resource_string(L"ApplicationUpdateManualButton")));
  ApplicationUpdateDiagnosticButton().Content(
      winrt::box_value(resource_string(L"ApplicationUpdateDiagnosticButton")));
  ArchitecturePromptItem().Content(winrt::box_value(
      resource_string(L"ApplicationSettingsArchitecturePrompt.Content")));
  ArchitectureAutoFallbackItem().Content(winrt::box_value(
      resource_string(L"ApplicationSettingsArchitectureAutoFallback.Content")));
  ArchitecturePreferX64Item().Content(winrt::box_value(
      resource_string(L"ApplicationSettingsArchitecturePreferX64.Content")));
  ArchitecturePromptItem().Visibility(Visibility::Visible);
  ArchitectureAutoFallbackItem().Visibility(Visibility::Visible);
  ArchitecturePreferX64Item().Visibility(Visibility::Visible);
}

void ApplicationSettingsPage::bind(
    std::shared_ptr<azzs::application::Workbench> workbench,
    azzs::application::ApplicationSettingsService& settings,
    azzs::application::UpdateSnapshot const& update_snapshot,
    azzs::application::ApplicationSettingsSnapshot const& snapshot,
    bool advanced_view,
    AdvancedViewChangedHandler advanced_view_changed,
    CatalogEditorRequestedHandler catalog_editor_requested) {
  workbench_ = std::move(workbench);
  settings_ = std::addressof(settings);
  advanced_view_ = advanced_view;
  advanced_view_changed_ = std::move(advanced_view_changed);
  catalog_editor_requested_ = std::move(catalog_editor_requested);
  project_update(update_snapshot);
  project(snapshot);
}

void ApplicationSettingsPage::OnAdvancedViewToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (projecting_) {
      return;
    }
    if (advanced_view_changed_) {
      advanced_view_ = advanced_view_changed_(AdvancedViewToggle().IsOn());
    } else {
      advanced_view_ = AdvancedViewToggle().IsOn();
    }
    projecting_ = true;
    AdvancedViewToggle().IsOn(advanced_view_);
    AdvancedSettingsPanel().Visibility(
        advanced_view_ ? Visibility::Visible : Visibility::Collapsed);
    projecting_ = false;
  } catch (...) {
    projecting_ = false;
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnCacheRetentionSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  try {
    if (projecting_ || settings_ == nullptr ||
        CacheRetentionComboBox().SelectedIndex() < 0) {
      return;
    }
    project_action(settings_->set_cache_retention(
        retention_for_index(CacheRetentionComboBox().SelectedIndex())));
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnArchitecturePreferenceSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  try {
    if (projecting_ || current_package_is_x64() || settings_ == nullptr ||
        ArchitecturePreferenceComboBox().SelectedIndex() < 0) {
      return;
    }
    project_action(settings_->set_architecture_preference(
        architecture_for_index(ArchitecturePreferenceComboBox().SelectedIndex())));
  } catch (...) {
    show_operation_failure();
  }
}

winrt::fire_and_forget ApplicationSettingsPage::OnClearCacheClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    auto lifetime = get_strong();
    if (settings_ == nullptr || confirmation_dialog_open_) {
      co_return;
    }
    auto proposed = settings_->clear_cache(false);
    if (proposed.code != ApplicationSettingsActionCode::confirmation_required) {
      project_action(proposed);
      co_return;
    }
    confirmation_dialog_open_ = true;
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(winrt::box_value(
        resource_string(L"ApplicationSettingsClearCacheDialogTitle")));
    dialog.Content(winrt::box_value(
        resource_string(L"ApplicationSettingsClearCacheDialogContent")));
    dialog.PrimaryButtonText(
        resource_string(L"ApplicationSettingsClearCacheDialogConfirm"));
    dialog.CloseButtonText(
        resource_string(L"ApplicationSettingsClearCacheDialogCancel"));
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      confirmation_dialog_open_ = false;
      project_action(settings_->clear_cache(true));
    } else {
      confirmation_dialog_open_ = false;
    }
  } catch (...) {
    confirmation_dialog_open_ = false;
    show_operation_failure();
    ::OutputDebugStringW(L"WinUI clear-cache dialog failed.\n");
  }
}

winrt::fire_and_forget ApplicationSettingsPage::OnClearLogsClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    auto lifetime = get_strong();
    if (settings_ == nullptr || confirmation_dialog_open_) {
      co_return;
    }
    auto proposed = settings_->clear_logs(false);
    if (proposed.code != ApplicationSettingsActionCode::confirmation_required) {
      project_action(proposed);
      co_return;
    }
    confirmation_dialog_open_ = true;
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(winrt::box_value(
        resource_string(L"ApplicationSettingsClearLogsDialogTitle")));
    dialog.Content(winrt::box_value(
        resource_string(L"ApplicationSettingsClearLogsDialogContent")));
    dialog.PrimaryButtonText(
        resource_string(L"ApplicationSettingsClearLogsDialogConfirm"));
    dialog.CloseButtonText(
        resource_string(L"ApplicationSettingsClearLogsDialogCancel"));
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      confirmation_dialog_open_ = false;
      project_action(settings_->clear_logs(true));
    } else {
      confirmation_dialog_open_ = false;
    }
  } catch (...) {
    confirmation_dialog_open_ = false;
    show_operation_failure();
    ::OutputDebugStringW(L"WinUI clear-logs dialog failed.\n");
  }
}

void ApplicationSettingsPage::OnExportDiagnosticClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (settings_ != nullptr) {
      project_action(settings_->export_diagnostic());
    }
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnRecoveryRecordSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  try {
    if (!projecting_) {
      project_recovery_selection();
    }
  } catch (...) {
    show_operation_failure();
  }
}

winrt::fire_and_forget ApplicationSettingsPage::OnDeleteRecoveryRecordClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    auto lifetime = get_strong();
    if (settings_ == nullptr || confirmation_dialog_open_) {
      co_return;
    }
    auto const record_id = selected_recovery_record();
    if (!record_id.has_value()) {
      co_return;
    }
    auto proposed = settings_->delete_recovery_record(*record_id, false);
    if (proposed.code != ApplicationSettingsActionCode::confirmation_required) {
      project_action(proposed);
      co_return;
    }
    confirmation_dialog_open_ = true;
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(winrt::box_value(
        resource_string(L"ApplicationSettingsDeleteRecoveryDialogTitle")));
    dialog.Content(winrt::box_value(
        resource_string(L"ApplicationSettingsDeleteRecoveryDialogContent")));
    dialog.PrimaryButtonText(
        resource_string(L"ApplicationSettingsDeleteRecoveryDialogConfirm"));
    dialog.CloseButtonText(
        resource_string(L"ApplicationSettingsDeleteRecoveryDialogCancel"));
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      confirmation_dialog_open_ = false;
      project_action(settings_->delete_recovery_record(*record_id, true));
    } else {
      confirmation_dialog_open_ = false;
    }
  } catch (...) {
    confirmation_dialog_open_ = false;
    show_operation_failure();
    ::OutputDebugStringW(L"WinUI recovery-record dialog failed.\n");
  }
}

winrt::fire_and_forget ApplicationSettingsPage::OnCatalogActionClick(
    Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    auto lifetime = get_strong();
    if (settings_ == nullptr || confirmation_dialog_open_) {
      co_return;
    }
    auto const tag = winrt::unbox_value<winrt::hstring>(
        sender.as<Microsoft::UI::Xaml::Controls::Button>().Tag());
    std::optional<ApplicationSettingsCatalog> catalog;
    std::optional<ApplicationSettingsCatalogAction> action;
    if (tag == L"software-update") {
      catalog = ApplicationSettingsCatalog::software_and_drivers;
      action = ApplicationSettingsCatalogAction::update;
    } else if (tag == L"software-rollback") {
      catalog = ApplicationSettingsCatalog::software_and_drivers;
      action = ApplicationSettingsCatalogAction::rollback;
    } else if (tag == L"system-update") {
      catalog = ApplicationSettingsCatalog::system_settings;
      action = ApplicationSettingsCatalogAction::update;
    } else if (tag == L"system-rollback") {
      catalog = ApplicationSettingsCatalog::system_settings;
      action = ApplicationSettingsCatalogAction::rollback;
    } else if (tag == L"optimization-update") {
      catalog = ApplicationSettingsCatalog::software_optimization;
      action = ApplicationSettingsCatalogAction::update;
    } else if (tag == L"optimization-rollback") {
      catalog = ApplicationSettingsCatalog::software_optimization;
      action = ApplicationSettingsCatalogAction::rollback;
    }
    if (!catalog.has_value() || !action.has_value()) {
      co_return;
    }

    auto proposed = settings_->prepare_catalog_change(*catalog, *action);
    if (proposed.code != ApplicationSettingsActionCode::confirmation_required ||
        !proposed.catalog_change.has_value()) {
      project_action(proposed);
      co_return;
    }

    confirmation_dialog_open_ = true;
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(winrt::box_value(
        resource_string(L"ApplicationSettingsCatalogDialogTitle")));
    dialog.Content(winrt::box_value(
        resource_string(L"ApplicationSettingsCatalogDialogContent")));
    dialog.PrimaryButtonText(
        resource_string(L"ApplicationSettingsCatalogDialogConfirm"));
    dialog.CloseButtonText(
        resource_string(L"ApplicationSettingsCatalogDialogCancel"));
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      confirmation_dialog_open_ = false;
      project_action(settings_->confirm_catalog_change(
          proposed.catalog_change->confirmation_token));
    } else {
      confirmation_dialog_open_ = false;
    }
  } catch (...) {
    confirmation_dialog_open_ = false;
    show_operation_failure();
    ::OutputDebugStringW(L"WinUI catalog action dialog failed.\n");
  }
}

void ApplicationSettingsPage::OnDebugModeToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (!projecting_ && settings_ != nullptr) {
      project_action(settings_->set_debug_enabled(DebugModeToggle().IsOn()));
    }
  } catch (...) {
    show_operation_failure();
  }
}

[[nodiscard]] winrt::hstring debug_granularity_text(
    azzs::application::DebugLogGranularity value) {
  using Granularity = azzs::application::DebugLogGranularity;
  switch (value) {
    case Granularity::maximum:
      return resource_string(L"ApplicationSettingsDebugGranularityMaximum");
    case Granularity::normal:
      return resource_string(L"ApplicationSettingsDebugGranularityNormal");
    case Granularity::unavailable:
      return resource_string(L"ApplicationSettingsDebugGranularityUnavailable");
  }
  return resource_string(L"ApplicationSettingsDebugGranularityUnavailable");
}

void ApplicationSettingsPage::OnOpenCatalogEditorClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (catalog_editor_requested_) {
      catalog_editor_requested_();
    }
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnApplicationUpdateCommandClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (!workbench_) {
      return;
    }
    auto const current = workbench_->snapshot().update.state;
    auto intent = azzs::application::UpdateUserIntent::check_for_update;
    switch (current) {
      case azzs::application::UpdateState::awaiting_user_confirmation:
        intent = azzs::application::UpdateUserIntent::confirm_update;
        break;
      case azzs::application::UpdateState::update_failed_restored:
      case azzs::application::UpdateState::candidate_pending_start_health:
      case azzs::application::UpdateState::previous_pending_start_health:
        intent = azzs::application::UpdateUserIntent::confirm_started_healthy;
        break;
      case azzs::application::UpdateState::update_available:
      case azzs::application::UpdateState::stable_switch_available:
        intent = azzs::application::UpdateUserIntent::request_update;
        break;
      default:
        break;
    }
    project_update(workbench_->handle_update(intent).snapshot);
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnApplicationUpdateRetryClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (workbench_) {
      project_update(workbench_->handle_update(
                               azzs::application::UpdateUserIntent::retry_new_version)
                          .snapshot);
    }
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnApplicationUpdateRestoreClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (workbench_) {
      project_update(workbench_->handle_update(
                               azzs::application::UpdateUserIntent::
                                   restore_previous_version)
                          .snapshot);
    }
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnApplicationUpdateManualClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (workbench_) {
      project_update(workbench_->handle_update(
                               azzs::application::UpdateUserIntent::
                                   open_all_github_releases)
                          .snapshot);
    }
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::OnApplicationUpdateDiagnosticClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    if (workbench_) {
      project_update(workbench_->handle_update(
                               azzs::application::UpdateUserIntent::export_diagnostic)
                          .snapshot);
    }
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::project(
    azzs::application::ApplicationSettingsSnapshot const& snapshot) {
  projecting_ = true;
  struct ProjectionGuard final {
    bool& projecting;
    ~ProjectionGuard() { projecting = false; }
  } guard{projecting_};
  if (snapshot.read_degraded) {
    // A partial read is recoverable: keep the page interactive and make the
    // unavailable state explicit instead of sending the user back to the
    // previous page. The existing localized action text gives a safe Chinese
    // fallback without exposing an internal persistence error string.
    SettingsOperationStatus().Title(
        resource_string(L"ApplicationSettingsOperationTitle"));
    SettingsOperationStatus().Message(
        resource_string(L"ApplicationSettingsActionFailed"));
    SettingsOperationStatus().Severity(InfoBarSeverity::Error);
    set_settings_operation_open(SettingsOperationStatus(), true);
  } else {
    set_settings_operation_open(SettingsOperationStatus(), false);
  }
  AdvancedViewToggle().IsOn(advanced_view_);
  AdvancedSettingsPanel().Visibility(advanced_view_ ? Visibility::Visible
                                                     : Visibility::Collapsed);
  CacheRetentionComboBox().SelectedIndex(retention_index(snapshot.cache.retention));
  auto const architecture_preferences_visible = !current_package_is_x64();
  ArchitecturePreferenceSection().Visibility(
      architecture_preferences_visible ? Visibility::Visible
                                       : Visibility::Collapsed);
  if (architecture_preferences_visible) {
    ArchitecturePreferenceComboBox().SelectedIndex(
        architecture_index(snapshot.architecture_preference));
  }

  auto cache_location = std::wstring{
      resource_string(L"ApplicationSettingsCacheLocationSummary")};
  replace_token(cache_location, L"{location}",
                cache_location_text(snapshot.cache.selected_root.kind));
  CacheLocationText().Text(winrt::hstring{cache_location});

  auto catalog_message = resource_string(L"ApplicationSettingsCatalogReady");
  if (!snapshot.software_catalog.current.has_value() ||
      !snapshot.settings_catalog.current.has_value() ||
      !snapshot.software_optimization_catalog.current.has_value()) {
    catalog_message = resource_string(L"ApplicationSettingsCatalogUnavailable");
    CatalogStatus().Severity(InfoBarSeverity::Warning);
  } else if (snapshot.software_catalog.current->identity ==
             azzs::application::software_catalog::EffectiveCatalogIdentity::
                 local_trial) {
    catalog_message = resource_string(L"ApplicationSettingsCatalogLocalTrial");
    CatalogStatus().Severity(InfoBarSeverity::Warning);
  } else {
    CatalogStatus().Severity(InfoBarSeverity::Informational);
  }
  CatalogStatus().Title(resource_string(L"ApplicationSettingsCatalogTitle"));
  CatalogStatus().Message(catalog_message);
  SoftwareCatalogRollbackButton().IsEnabled(
      snapshot.software_catalog.previous.has_value());
  SystemCatalogRollbackButton().IsEnabled(
      snapshot.settings_catalog.previous.has_value());
  OptimizationCatalogRollbackButton().IsEnabled(
      snapshot.software_optimization_catalog.previous_available);

  LogStatusText().Text(snapshot.history_and_logs.log.available
                            ? resource_string(L"ApplicationSettingsLogsAvailable")
                            : resource_string(L"ApplicationSettingsLogsUnavailable"));

  recovery_records_ = snapshot.recovery_records;
  RecoveryRecordComboBox().Items().Clear();
  for (auto const& record : recovery_records_) {
    auto entry = std::wstring{
        resource_string(L"ApplicationSettingsRecoveryRecordEntry")};
    replace_token(entry, L"{id}", std::to_wstring(record.record_id));
    RecoveryRecordComboBox().Items().Append(winrt::box_value(winrt::hstring{entry}));
  }
  RecoveryRecordComboBox().IsEnabled(!recovery_records_.empty());
  auto recovery_summary = std::wstring{
      resource_string(L"ApplicationSettingsRecoveryRecordCount")};
  replace_token(recovery_summary, L"{count}",
                std::to_wstring(recovery_records_.size()));
  RecoveryRecordSummaryText().Text(winrt::hstring{recovery_summary});
  RecoveryRecordComboBox().SelectedIndex(recovery_records_.empty() ? -1 : 0);
  project_recovery_selection();

  DebugModeToggle().IsEnabled(snapshot.debug.available);
  DebugModeToggle().IsOn(snapshot.debug.enabled);
  auto debug_status = std::wstring{
      !snapshot.debug.available
          ? resource_string(L"ApplicationSettingsDebugUnavailable")
          : snapshot.debug.enabled
                ? resource_string(L"ApplicationSettingsDebugEnabled")
                : resource_string(L"ApplicationSettingsDebugDisabled")};
  debug_status += L" " + std::wstring{debug_granularity_text(
                                  snapshot.debug.log_granularity)};
  DebugModeStatusText().Text(winrt::hstring{debug_status});
  OpenCatalogEditorButton().Visibility(
      snapshot.debug.catalog_editor_available ? Visibility::Visible
                                               : Visibility::Collapsed);
  OpenCatalogEditorButton().IsEnabled(snapshot.debug.catalog_editor_available);
}

void ApplicationSettingsPage::project_update(
    azzs::application::UpdateSnapshot const& snapshot) {
  using azzs::application::UpdateState;
  using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;

  auto const title = resource_string(L"ApplicationUpdateTitle.Text");
  auto message = std::wstring{
      resource_string(L"ApplicationUpdateReadyMessage")};
  auto severity = InfoBarSeverity::Informational;

  ApplicationUpdateRetryButton().Visibility(Visibility::Collapsed);
  ApplicationUpdateRestoreButton().Visibility(Visibility::Collapsed);
  ApplicationUpdateCommandButton().IsEnabled(true);
  ApplicationUpdateCommandButton().Content(
      winrt::box_value(resource_string(L"ApplicationUpdateCheckButton")));

  switch (snapshot.state) {
    case UpdateState::idle:
      break;
    case UpdateState::latest_stable:
      message = std::wstring{resource_string(L"ApplicationUpdateLatestMessage")};
      break;
    case UpdateState::update_available:
      message = std::wstring{resource_string(L"ApplicationUpdateAvailableMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(resource_string(L"ApplicationUpdateRequestButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::stable_switch_available:
      message =
          std::wstring{resource_string(L"ApplicationUpdateStableSwitchMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(resource_string(L"ApplicationUpdateRequestButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::no_matching_stable_asset:
      message = std::wstring{resource_string(L"ApplicationUpdateNoMatchMessage")};
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::awaiting_user_confirmation:
      message = std::wstring{resource_string(L"ApplicationUpdateUnsignedMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(resource_string(L"ApplicationUpdateConfirmButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::deferred_initialization_operation:
      message = std::wstring{resource_string(L"ApplicationUpdateDeferredMessage")};
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::update_unavailable:
      message = std::wstring{resource_string(L"ApplicationUpdateUnavailableMessage")};
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::update_failed_restored:
      message = std::wstring{resource_string(L"ApplicationUpdateRestoredMessage")};
      ApplicationUpdateCommandButton().Content(winrt::box_value(
          resource_string(L"ApplicationUpdateConfirmHealthButton")));
      severity = InfoBarSeverity::Error;
      break;
    case UpdateState::candidate_pending_start_health:
      message =
          std::wstring{resource_string(L"ApplicationUpdateHealthPendingMessage")};
      ApplicationUpdateCommandButton().Content(winrt::box_value(
          resource_string(L"ApplicationUpdateConfirmHealthButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::awaiting_start_recovery_choice:
      message =
          std::wstring{resource_string(L"ApplicationUpdateRecoveryChoiceMessage")};
      ApplicationUpdateRetryButton().Content(
          winrt::box_value(resource_string(L"ApplicationUpdateRetryButton")));
      ApplicationUpdateRetryButton().Visibility(Visibility::Visible);
      ApplicationUpdateRestoreButton().Content(
          winrt::box_value(resource_string(L"ApplicationUpdateRestoreButton")));
      ApplicationUpdateRestoreButton().Visibility(Visibility::Visible);
      severity = InfoBarSeverity::Error;
      break;
    case UpdateState::previous_pending_start_health:
      message =
          std::wstring{resource_string(L"ApplicationUpdatePreviousHealthMessage")};
      ApplicationUpdateCommandButton().Content(winrt::box_value(
          resource_string(L"ApplicationUpdateConfirmHealthButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::recovery_read_only:
      message = std::wstring{resource_string(L"ApplicationUpdateReadOnlyMessage")};
      if (snapshot.health.has_value()) {
        message += L"\n";
        message += std::wstring{
            resource_string(L"ApplicationUpdateReadOnlyDetail")};
        message += L" ";
        message += std::wstring{snapshot.health->previous.version.begin(),
                                snapshot.health->previous.version.end()};
        message += L" / ";
        message += std::wstring{snapshot.health->target.version.begin(),
                                snapshot.health->target.version.end()};
      }
      ApplicationUpdateCommandButton().IsEnabled(false);
      severity = InfoBarSeverity::Error;
      break;
  }

  ApplicationUpdateStatus().Title(title);
  ApplicationUpdateStatus().Message(winrt::hstring{message});
  ApplicationUpdateStatus().Severity(severity);
  AutomationProperties::SetName(ApplicationUpdateStatus(), title);
  set_update_status_open(ApplicationUpdateStatus(),
                         snapshot.state != UpdateState::idle);
}

void ApplicationSettingsPage::project_action(
    azzs::application::ApplicationSettingsActionResult const& result) {
  try {
    project(result.snapshot);
    SettingsOperationStatus().Title(
        resource_string(L"ApplicationSettingsOperationTitle"));
    SettingsOperationStatus().Message(action_message(result.code));
    SettingsOperationStatus().Severity(action_severity(result.code));
    set_settings_operation_open(SettingsOperationStatus(), true);
  } catch (...) {
    show_operation_failure();
  }
}

void ApplicationSettingsPage::show_operation_failure() noexcept {
  try {
    SettingsOperationStatus().Title(
        resource_string(L"ApplicationSettingsOperationTitle"));
    SettingsOperationStatus().Message(
        resource_string(L"ApplicationSettingsActionFailed"));
    SettingsOperationStatus().Severity(InfoBarSeverity::Error);
    set_settings_operation_open(SettingsOperationStatus(), true);
  } catch (...) {
    ::OutputDebugStringW(L"WinUI application-settings operation status failed.\n");
  }
}

void ApplicationSettingsPage::project_recovery_selection() {
  auto const selected = selected_recovery_record();
  if (!selected.has_value()) {
    DeleteRecoveryRecordButton().IsEnabled(false);
    RecoveryRecordDetailsText().Text(
        resource_string(L"ApplicationSettingsRecoveryNoSelection"));
    return;
  }
  auto const index = static_cast<std::size_t>(
      RecoveryRecordComboBox().SelectedIndex());
  if (index >= recovery_records_.size()) {
    DeleteRecoveryRecordButton().IsEnabled(false);
    RecoveryRecordDetailsText().Text(
        resource_string(L"ApplicationSettingsRecoveryNoSelection"));
    return;
  }
  auto const protected_record = recovery_record_is_protected(
      recovery_records_[index].status);
  DeleteRecoveryRecordButton().IsEnabled(!protected_record);
  RecoveryRecordDetailsText().Text(
      protected_record
          ? resource_string(L"ApplicationSettingsRecoveryDeleteBlocked")
          : resource_string(L"ApplicationSettingsRecoveryDeleteAvailable"));
}

std::optional<std::uint64_t> ApplicationSettingsPage::selected_recovery_record()
{
  auto const index = RecoveryRecordComboBox().SelectedIndex();
  if (index < 0 || static_cast<std::size_t>(index) >= recovery_records_.size()) {
    return std::nullopt;
  }
  return recovery_records_[static_cast<std::size_t>(index)].record_id;
}

bool ApplicationSettingsPage::recovery_record_is_protected(
    azzs::application::RecoveryRecordStatus status) noexcept {
  using azzs::application::RecoveryRecordStatus;
  return status == RecoveryRecordStatus::pending ||
         status == RecoveryRecordStatus::restoring ||
         status == RecoveryRecordStatus::waiting_explorer_restart;
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
