#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "Pages/HardwareInformationPage.g.h"
#include "azzs/application/hardware_overview.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct HardwareInformationPage : HardwareInformationPageT<HardwareInformationPage> {
  using RefreshHandler = std::function<void()>;
  using CopyRow = std::pair<winrt::hstring, winrt::hstring>;

  HardwareInformationPage();
  void bind(azzs::application::HardwareOverviewSnapshot const& snapshot,
            RefreshHandler refresh_handler);
  void project(azzs::application::HardwareOverviewSnapshot const& snapshot);
  void OnRefreshClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCopyHardwareClicked(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  RefreshHandler refresh_handler_;
  std::vector<CopyRow> copy_rows_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct HardwareInformationPage
    : HardwareInformationPageT<HardwareInformationPage,
                               implementation::HardwareInformationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
