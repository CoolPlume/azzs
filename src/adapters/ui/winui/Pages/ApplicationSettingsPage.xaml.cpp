#include "pch.h"

#include "ApplicationSettingsPage.xaml.h"

#include <string>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#if __has_include("Pages/ApplicationSettingsPage.g.cpp")
#include "Pages/ApplicationSettingsPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {

ApplicationSettingsPage::ApplicationSettingsPage() {
  InitializeComponent();
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  auto const resources = ResourceLoader{};
  ApplicationUpdateCommandButton().Content(
      winrt::box_value(resources.GetString(L"ApplicationUpdateCheckButton")));
  ApplicationUpdateManualButton().Content(
      winrt::box_value(resources.GetString(L"ApplicationUpdateManualButton")));
  ApplicationUpdateDiagnosticButton().Content(
      winrt::box_value(
          resources.GetString(L"ApplicationUpdateDiagnosticButton")));
}

void ApplicationSettingsPage::bind(
    std::shared_ptr<azzs::application::Workbench> workbench) {
  workbench_ = std::move(workbench);
  if (workbench_) {
    project(workbench_->snapshot().update);
  }
}

void ApplicationSettingsPage::OnApplicationUpdateCommandClick(
    Microsoft::UI::Xaml::Controls::Button const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
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
  project(workbench_->handle_update(intent).snapshot);
}

void ApplicationSettingsPage::OnApplicationUpdateRetryClick(
    Microsoft::UI::Xaml::Controls::Button const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (workbench_) {
    project(workbench_->handle_update(
                         azzs::application::UpdateUserIntent::retry_new_version)
                .snapshot);
  }
}

void ApplicationSettingsPage::OnApplicationUpdateRestoreClick(
    Microsoft::UI::Xaml::Controls::Button const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (workbench_) {
    project(workbench_->handle_update(
                         azzs::application::UpdateUserIntent::
                             restore_previous_version)
                .snapshot);
  }
}

void ApplicationSettingsPage::OnApplicationUpdateManualClick(
    Microsoft::UI::Xaml::Controls::Button const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (workbench_) {
    project(workbench_->handle_update(
                         azzs::application::UpdateUserIntent::
                             open_all_github_releases)
                .snapshot);
  }
}

void ApplicationSettingsPage::OnApplicationUpdateDiagnosticClick(
    Microsoft::UI::Xaml::Controls::Button const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (workbench_) {
    project(workbench_->handle_update(
                         azzs::application::UpdateUserIntent::export_diagnostic)
                .snapshot);
  }
}

void ApplicationSettingsPage::project(
    azzs::application::UpdateSnapshot const& snapshot) {
  using azzs::application::UpdateState;
  using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
  using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  using winrt::Microsoft::UI::Xaml::Visibility;

  auto const resources = ResourceLoader{};
  auto title = resources.GetString(L"ApplicationUpdateTitle");
  auto message =
      std::wstring{resources.GetString(L"ApplicationUpdateReadyMessage")};
  auto severity = InfoBarSeverity::Informational;

  ApplicationUpdateRetryButton().Visibility(Visibility::Collapsed);
  ApplicationUpdateRestoreButton().Visibility(Visibility::Collapsed);
  ApplicationUpdateCommandButton().IsEnabled(true);
  ApplicationUpdateCommandButton().Content(
      winrt::box_value(resources.GetString(L"ApplicationUpdateCheckButton")));

  switch (snapshot.state) {
    case UpdateState::idle:
      break;
    case UpdateState::latest_stable:
      message = std::wstring{resources.GetString(L"ApplicationUpdateLatestMessage")};
      break;
    case UpdateState::update_available:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateAvailableMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(resources.GetString(L"ApplicationUpdateRequestButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::stable_switch_available:
      message = std::wstring{
          resources.GetString(L"ApplicationUpdateStableSwitchMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(resources.GetString(L"ApplicationUpdateRequestButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::no_matching_stable_asset:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateNoMatchMessage")};
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::awaiting_user_confirmation:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateUnsignedMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(resources.GetString(L"ApplicationUpdateConfirmButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::deferred_initialization_operation:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateDeferredMessage")};
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::update_unavailable:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateUnavailableMessage")};
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::update_failed_restored:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateRestoredMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(
              resources.GetString(L"ApplicationUpdateConfirmHealthButton")));
      severity = InfoBarSeverity::Error;
      break;
    case UpdateState::candidate_pending_start_health:
      message = std::wstring{
          resources.GetString(L"ApplicationUpdateHealthPendingMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(
              resources.GetString(L"ApplicationUpdateConfirmHealthButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::awaiting_start_recovery_choice:
      message = std::wstring{
          resources.GetString(L"ApplicationUpdateRecoveryChoiceMessage")};
      ApplicationUpdateRetryButton().Content(
          winrt::box_value(resources.GetString(L"ApplicationUpdateRetryButton")));
      ApplicationUpdateRetryButton().Visibility(Visibility::Visible);
      ApplicationUpdateRestoreButton().Content(
          winrt::box_value(resources.GetString(L"ApplicationUpdateRestoreButton")));
      ApplicationUpdateRestoreButton().Visibility(Visibility::Visible);
      severity = InfoBarSeverity::Error;
      break;
    case UpdateState::previous_pending_start_health:
      message = std::wstring{
          resources.GetString(L"ApplicationUpdatePreviousHealthMessage")};
      ApplicationUpdateCommandButton().Content(
          winrt::box_value(
              resources.GetString(L"ApplicationUpdateConfirmHealthButton")));
      severity = InfoBarSeverity::Warning;
      break;
    case UpdateState::recovery_read_only:
      message =
          std::wstring{resources.GetString(L"ApplicationUpdateReadOnlyMessage")};
      if (snapshot.health.has_value()) {
        message += L"\n";
        message += std::wstring{
            resources.GetString(L"ApplicationUpdateReadOnlyDetail")};
        message += L" ";
        message += std::wstring{snapshot.health->previous.version.begin(),
                                snapshot.health->previous.version.end()};
        message += L" / ";
        message += std::wstring{snapshot.health->target.version.begin(),
                                snapshot.health->target.version.end()};
        message += L" / ";
        message += std::to_wstring(
            snapshot.health->started_at.time_since_epoch().count());
      }
      ApplicationUpdateCommandButton().IsEnabled(false);
      severity = InfoBarSeverity::Error;
      break;
  }

  ApplicationUpdateStatus().Title(title);
  ApplicationUpdateStatus().Message(winrt::hstring{message});
  ApplicationUpdateStatus().Severity(severity);
  AutomationProperties::SetName(ApplicationUpdateStatus(), title);
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
