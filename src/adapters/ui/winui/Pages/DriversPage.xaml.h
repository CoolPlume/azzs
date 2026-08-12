#pragma once

#include <functional>
#include <memory>

#include "Pages/DriversPage.g.h"

namespace azzs::application {
class HardwareOverviewSnapshot;
class Workbench;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct DriversPage : DriversPageT<DriversPage> {
  using RefreshHandler = std::function<void()>;

  DriversPage();
  void bind(azzs::application::HardwareOverviewSnapshot const& snapshot,
            RefreshHandler refresh_handler);
  void project(azzs::application::HardwareOverviewSnapshot const& snapshot);
  void OnRefreshClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  RefreshHandler refresh_handler_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct DriversPage : DriversPageT<DriversPage, implementation::DriversPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
