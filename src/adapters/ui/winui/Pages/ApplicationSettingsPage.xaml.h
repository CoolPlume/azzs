#pragma once

#include <memory>

#include "Pages/ApplicationSettingsPage.g.h"
#include "azzs/application/workbench.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage> {
  ApplicationSettingsPage();

  void bind(std::shared_ptr<azzs::application::Workbench> workbench);
  void OnApplicationUpdateCommandClick(
      Microsoft::UI::Xaml::Controls::Button const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateRetryClick(
      Microsoft::UI::Xaml::Controls::Button const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateRestoreClick(
      Microsoft::UI::Xaml::Controls::Button const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateManualClick(
      Microsoft::UI::Xaml::Controls::Button const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateDiagnosticClick(
      Microsoft::UI::Xaml::Controls::Button const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void project(azzs::application::UpdateSnapshot const& snapshot);

  std::shared_ptr<azzs::application::Workbench> workbench_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage,
                               implementation::ApplicationSettingsPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
