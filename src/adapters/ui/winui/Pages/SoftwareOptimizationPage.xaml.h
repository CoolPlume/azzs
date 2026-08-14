#pragma once

#include <string>

#include "Pages/SoftwareOptimizationPage.g.h"

namespace azzs::application::software_optimization_discovery {
class SoftwareOptimizationDiscoveryService;
struct SoftwareOptimizationDiscoverySnapshot;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareOptimizationPage
    : SoftwareOptimizationPageT<SoftwareOptimizationPage> {
  SoftwareOptimizationPage();
  void bind(azzs::application::software_optimization_discovery::
                SoftwareOptimizationDiscoveryService& service,
            bool advanced_view);
  void OnRefresh(winrt::Windows::Foundation::IInspectable const& sender,
                 Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnPrepareSelected(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  winrt::fire_and_forget OnOptionSelectionChanged(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  void project(azzs::application::software_optimization_discovery::
                   SoftwareOptimizationDiscoverySnapshot const& snapshot);
  void set_status(winrt::hstring const& message,
                  Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);

  azzs::application::software_optimization_discovery::
      SoftwareOptimizationDiscoveryService* service_{};
  bool advanced_view_{false};
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareOptimizationPage
    : SoftwareOptimizationPageT<SoftwareOptimizationPage,
                                implementation::SoftwareOptimizationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
