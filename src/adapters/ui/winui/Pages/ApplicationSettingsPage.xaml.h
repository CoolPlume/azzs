#pragma once

#include <functional>
#include <memory>

#include "Pages/ApplicationSettingsPage.g.h"
#include "azzs/application/workbench.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage> {
  using AdvancedViewChangedHandler = std::function<bool(bool)>;

  ApplicationSettingsPage();

  void bind(std::shared_ptr<azzs::application::Workbench> workbench,
            bool advanced_view,
            AdvancedViewChangedHandler advanced_view_changed);
  void OnAdvancedViewToggled(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateCommandClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateRetryClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateRestoreClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateManualClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateDiagnosticClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void project(azzs::application::UpdateSnapshot const& snapshot);

  std::shared_ptr<azzs::application::Workbench> workbench_;
  AdvancedViewChangedHandler advanced_view_changed_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage,
                               implementation::ApplicationSettingsPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
