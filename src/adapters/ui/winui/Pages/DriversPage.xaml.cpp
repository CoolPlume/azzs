#include "pch.h"

#include "DriversPage.xaml.h"

#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/driver_acquisition.hpp"
#include "azzs/application/hardware_overview.hpp"

#if __has_include("Pages/DriversPage.g.cpp")
#include "Pages/DriversPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {
namespace {

[[nodiscard]] winrt::hstring display_value(std::string_view value,
                                            winrt::hstring const& fallback) {
  return value.empty() ? fallback : winrt::to_hstring(value);
}

[[nodiscard]] winrt::hstring entrypoint_text(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const&
        resources,
    azzs::application::driver_acquisition::DriverEntrypoint entrypoint) {
  using azzs::application::driver_acquisition::DriverEntrypoint;
  switch (entrypoint) {
    case DriverEntrypoint::amd_software:
      return resources.GetString(L"DriverEntrypointAmd");
    case DriverEntrypoint::intel_driver_assistant:
      return resources.GetString(L"DriverEntrypointIntel");
    case DriverEntrypoint::nvidia_drivers:
      return resources.GetString(L"DriverEntrypointNvidia");
    case DriverEntrypoint::dell_support:
      return resources.GetString(L"DriverEntrypointDell");
    case DriverEntrypoint::hp_support:
      return resources.GetString(L"DriverEntrypointHp");
    case DriverEntrypoint::lenovo_support:
      return resources.GetString(L"DriverEntrypointLenovo");
    case DriverEntrypoint::asus_support:
      return resources.GetString(L"DriverEntrypointAsus");
  }
  return {};
}

[[nodiscard]] winrt::hstring recommendation_text(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const&
        resources,
    std::vector<azzs::application::driver_acquisition::DriverEntrypoint> const&
        entries) {
  std::wstring result;
  for (auto const entry : entries) {
    if (!result.empty()) {
      result += L"、";
    }
    result += entrypoint_text(resources, entry).c_str();
  }
  return winrt::hstring{result};
}

void set_visibility(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
                    bool visible) {
  element.Visibility(visible ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                             : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
}

}  // namespace

DriversPage::DriversPage() {
  InitializeComponent();
}

void DriversPage::bind(
    azzs::application::HardwareOverviewSnapshot const& snapshot,
    azzs::application::driver_acquisition::DriverAcquisitionSnapshot const&
        driver_snapshot,
    RefreshHandler refresh_handler, HandoffHandler handoff_handler,
    RescueHandoffHandler rescue_handoff_handler,
    ReturnedHandler returned_handler, DecisionHandler decision_handler) {
  refresh_handler_ = std::move(refresh_handler);
  handoff_handler_ = std::move(handoff_handler);
  rescue_handoff_handler_ = std::move(rescue_handoff_handler);
  returned_handler_ = std::move(returned_handler);
  decision_handler_ = std::move(decision_handler);
  project(snapshot, driver_snapshot);
}

void DriversPage::OnRefreshClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (refresh_handler_) {
    refresh_handler_();
  }
}

void DriversPage::project(
    azzs::application::HardwareOverviewSnapshot const& snapshot) {
  project(snapshot, {});
}

void DriversPage::project(
    azzs::application::HardwareOverviewSnapshot const& snapshot,
    azzs::application::driver_acquisition::DriverAcquisitionSnapshot const&
        driver_snapshot) {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  using azzs::application::driver_acquisition::DriverAcquisitionState;

  auto const resources = ResourceLoader{};
  auto const unrecognized_value =
      resources.GetString(L"HardwareUnrecognizedValue");
  auto const unrecognized =
      snapshot.state == azzs::application::HardwareOverviewState::unrecognized;
  HardwareStatus().IsOpen(unrecognized);
  if (unrecognized) {
    HardwareStatus().Title(resources.GetString(L"HardwareStatusTitle"));
    HardwareStatus().Message(resources.GetString(L"HardwareStatusMessage"));
  }

  auto const facts = snapshot.observation.value_or(
      azzs::application::HardwareObservation{});
  CpuValue().Text(display_value(facts.cpu, unrecognized_value));
  GpuValue().Text(display_value(facts.gpu, unrecognized_value));
  MotherboardValue().Text(display_value(facts.motherboard, unrecognized_value));
  NetworkValue().Text(display_value(facts.network_adapter, unrecognized_value));
  OemValue().Text(display_value(facts.oem_model, unrecognized_value));

  auto const can_start = driver_snapshot.state == DriverAcquisitionState::ready;
  DriverAssistantActionButton().IsEnabled(can_start);
  IntelDriverButton().IsEnabled(can_start);
  NvidiaDriverButton().IsEnabled(can_start);
  DellSupportButton().IsEnabled(can_start);
  HpSupportButton().IsEnabled(can_start);
  LenovoSupportButton().IsEnabled(can_start);
  AsusSupportButton().IsEnabled(can_start);
  GenericNetworkDriverRescueButton().IsEnabled(can_start);
  OfflineNetworkDiagnosticsRescueButton().IsEnabled(can_start);
  DriverAssistantActionText().Text(resources.GetString(
      driver_snapshot.assistant_installed ? L"DriverAssistantLaunchButton"
                                          : L"DriverAssistantInstallButton"));
  DriverAssistantStatus().Text(resources.GetString(
      driver_snapshot.assistant_installed ? L"DriverAssistantInstalled"
                                          : L"DriverAssistantNotInstalled"));

  auto const has_recommendation = !driver_snapshot.recommended_entrypoints.empty();
  DriverRecommendation().IsOpen(has_recommendation);
  if (has_recommendation) {
    DriverRecommendation().Title(resources.GetString(L"DriverRecommendationTitle"));
    DriverRecommendation().Message(recommendation_text(
        resources, driver_snapshot.recommended_entrypoints));
  }

  auto show_surface = false;
  auto show_returned = false;
  auto show_decisions = false;
  if (driver_snapshot.state == DriverAcquisitionState::handoff_in_progress) {
    show_surface = true;
    show_returned = true;
    auto const rescue_handoff = driver_snapshot.active_rescue_target.has_value();
    DriverHandoffHeadline().Text(resources.GetString(
        rescue_handoff ? L"RescueHandoffInProgressTitle"
                       : L"DriverHandoffInProgressTitle"));
    DriverHandoffDescription().Text(resources.GetString(
        rescue_handoff ? L"RescueHandoffInProgressBody"
                       : L"DriverHandoffInProgressBody"));
  } else if (driver_snapshot.state == DriverAcquisitionState::awaiting_user_decision) {
    show_surface = true;
    show_decisions = true;
    DriverHandoffHeadline().Text(resources.GetString(L"DriverHandoffDecisionTitle"));
    DriverHandoffDescription().Text(resources.GetString(L"DriverHandoffDecisionBody"));
  } else if (driver_snapshot.state == DriverAcquisitionState::waiting_for_restart) {
    show_surface = true;
    DriverHandoffHeadline().Text(resources.GetString(L"DriverHandoffRestartTitle"));
    DriverHandoffDescription().Text(resources.GetString(L"DriverHandoffRestartBody"));
  } else if (driver_snapshot.state == DriverAcquisitionState::read_only ||
             driver_snapshot.state == DriverAcquisitionState::not_restored) {
    show_surface = true;
    DriverHandoffHeadline().Text(resources.GetString(L"DriverHandoffUnavailableTitle"));
    DriverHandoffDescription().Text(driver_snapshot.detail.empty()
                                        ? resources.GetString(L"DriverHandoffUnavailableBody")
                                        : winrt::to_hstring(driver_snapshot.detail));
  } else if (!driver_snapshot.detail.empty()) {
    show_surface = true;
    DriverHandoffHeadline().Text(resources.GetString(L"DriverHandoffLaunchFailedTitle"));
    DriverHandoffDescription().Text(winrt::to_hstring(driver_snapshot.detail));
  } else if (driver_snapshot.last_observation.has_value()) {
    show_surface = true;
    DriverHandoffHeadline().Text(resources.GetString(L"DriverObservationTitle"));
    DriverHandoffDescription().Text(resources.GetString(
        driver_snapshot.last_observation->network_available
            ? L"DriverObservationNetworkAvailableBody"
            : L"DriverObservationNetworkUnavailableBody"));
  }
  set_visibility(DriverHandoffSurface(), show_surface);
  set_visibility(ExternalFlowReturnedButton(), show_returned);
  set_visibility(DriverResultActions(), show_decisions);
}

void DriversPage::request_handoff(
    azzs::application::driver_acquisition::DriverEntrypoint entrypoint) {
  if (handoff_handler_) {
    handoff_handler_(entrypoint);
  }
}

void DriversPage::request_rescue_handoff(
    azzs::application::driver_acquisition::RescueToolTarget target) {
  if (rescue_handoff_handler_) {
    rescue_handoff_handler_(target);
  }
}

void DriversPage::OnDriverAssistantClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::amd_software);
}

void DriversPage::OnIntelDriverClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::intel_driver_assistant);
}

void DriversPage::OnNvidiaDriverClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::nvidia_drivers);
}

void DriversPage::OnDellSupportClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::dell_support);
}

void DriversPage::OnHpSupportClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::hp_support);
}

void DriversPage::OnLenovoSupportClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::lenovo_support);
}

void DriversPage::OnAsusSupportClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_handoff(azzs::application::driver_acquisition::DriverEntrypoint::asus_support);
}

void DriversPage::OnGenericNetworkDriverRescueClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_rescue_handoff(
      azzs::application::driver_acquisition::RescueToolTarget::generic_network_driver);
}

void DriversPage::OnOfflineNetworkDiagnosticsRescueClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  request_rescue_handoff(
      azzs::application::driver_acquisition::RescueToolTarget::offline_network_diagnostics);
}

void DriversPage::OnExternalFlowReturnedClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (returned_handler_) {
    returned_handler_();
  }
}

void DriversPage::OnDriverCompletedClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (decision_handler_) {
    decision_handler_(azzs::application::driver_acquisition::DriverHandoffDecision::completed_externally);
  }
}

void DriversPage::OnDriverRestartRequiredClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (decision_handler_) {
    decision_handler_(azzs::application::driver_acquisition::DriverHandoffDecision::restart_required);
  }
}

void DriversPage::OnDriverSkipClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (decision_handler_) {
    decision_handler_(azzs::application::driver_acquisition::DriverHandoffDecision::skip_for_now);
  }
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
