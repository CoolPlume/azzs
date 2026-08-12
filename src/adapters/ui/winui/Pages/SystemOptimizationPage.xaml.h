#pragma once

#include <memory>

#include "Pages/SystemOptimizationPage.g.h"

namespace azzs::application {
class SystemSettingsApplyService;
struct SystemSettingsApplySnapshot;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct SystemOptimizationPage : SystemOptimizationPageT<SystemOptimizationPage> {
  SystemOptimizationPage();
  void bind(
      std::shared_ptr<azzs::application::SystemSettingsApplyService> service);
  void OnSelectRecommended(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplySelected(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnRestartExplorerNow(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void OnSettingSelectionChanged(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void project(azzs::application::SystemSettingsApplySnapshot const& snapshot);

  std::shared_ptr<azzs::application::SystemSettingsApplyService> service_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SystemOptimizationPage
    : SystemOptimizationPageT<SystemOptimizationPage,
                              implementation::SystemOptimizationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
