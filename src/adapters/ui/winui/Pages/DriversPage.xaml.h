#pragma once

#include <functional>
#include <memory>

#include "Pages/DriversPage.g.h"
#include "azzs/application/driver_acquisition.hpp"

namespace azzs::application {
struct HardwareOverviewSnapshot;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct DriversPage : DriversPageT<DriversPage> {
  using RefreshHandler = std::function<void()>;
  using HandoffHandler = std::function<void(
      azzs::application::driver_acquisition::DriverEntrypoint)>;
  using RescueHandoffHandler = std::function<void(
      azzs::application::driver_acquisition::RescueToolTarget)>;
  using ReturnedHandler = std::function<void()>;
  using DecisionHandler = std::function<void(
      azzs::application::driver_acquisition::DriverHandoffDecision)>;

  DriversPage();
  void bind(azzs::application::HardwareOverviewSnapshot const& snapshot,
            azzs::application::driver_acquisition::DriverAcquisitionSnapshot
                const& driver_snapshot,
            RefreshHandler refresh_handler, HandoffHandler handoff_handler,
            RescueHandoffHandler rescue_handoff_handler,
            ReturnedHandler returned_handler, DecisionHandler decision_handler);
  void project(azzs::application::HardwareOverviewSnapshot const& snapshot);
  void project(azzs::application::HardwareOverviewSnapshot const& snapshot,
               azzs::application::driver_acquisition::DriverAcquisitionSnapshot
                   const& driver_snapshot);
  void OnRefreshClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDriverAssistantClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnIntelDriverClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnNvidiaDriverClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDellSupportClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnHpSupportClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLenovoSupportClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnAsusSupportClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnGenericNetworkDriverRescueClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnOfflineNetworkDiagnosticsRescueClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnExternalFlowReturnedClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDriverCompletedClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDriverRestartRequiredClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDriverSkipClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void request_handoff(
      azzs::application::driver_acquisition::DriverEntrypoint entrypoint);
  void request_rescue_handoff(
      azzs::application::driver_acquisition::RescueToolTarget target);
  RefreshHandler refresh_handler_;
  HandoffHandler handoff_handler_;
  RescueHandoffHandler rescue_handoff_handler_;
  ReturnedHandler returned_handler_;
  DecisionHandler decision_handler_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct DriversPage : DriversPageT<DriversPage, implementation::DriversPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
