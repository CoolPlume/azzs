#pragma once

#include <optional>

#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/application/startup_assembly_status.hpp"

#include "App.xaml.g.h"

namespace winrt::Azzs::Ui::implementation {

struct App : AppT<App> {
  App();

  void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

  [[nodiscard]] ::azzs::application::startup::StartupAssemblyStatus const*
  startup_status() const noexcept {
    return startup_status_ ? &*startup_status_ : nullptr;
  }

 private:
  Microsoft::UI::Xaml::Window window_{nullptr};
  std::optional<::azzs::adapters::windows::DeviceDataEnvironmentResult>
      device_data_environment_failure_;
  std::optional<::azzs::application::startup::StartupAssemblyStatus>
      startup_status_;
  bool bootstrap_failed_{false};
};

}  // namespace winrt::Azzs::Ui::implementation
