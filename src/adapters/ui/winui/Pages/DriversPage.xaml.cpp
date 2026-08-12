#include "pch.h"

#include "DriversPage.xaml.h"

#include <string_view>
#include <utility>

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

}  // namespace

DriversPage::DriversPage() {
  InitializeComponent();
}

void DriversPage::bind(
    azzs::application::HardwareOverviewSnapshot const& snapshot,
    RefreshHandler refresh_handler) {
  refresh_handler_ = std::move(refresh_handler);
  project(snapshot);
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
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

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
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
