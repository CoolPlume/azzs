#pragma once

#include <optional>

#include <winrt/Microsoft.UI.Xaml.h>

#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/application/startup_assembly_status.hpp"

namespace azzs::composition::windows {

namespace startup = ::azzs::application::startup;

struct StartupAssemblyResult final {
  winrt::Microsoft::UI::Xaml::Window window{nullptr};
  startup::StartupAssemblyStatus status;
  // Keep the complete adapter result for the caller without putting any of it
  // into the public failure window.
  std::optional<adapters::windows::DeviceDataEnvironmentResult>
      device_data_environment_failure;
};

[[nodiscard]] StartupAssemblyResult assemble_startup();

}  // namespace azzs::composition::windows
