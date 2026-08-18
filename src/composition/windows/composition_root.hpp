#pragma once

#include <winrt/Microsoft.UI.Xaml.h>

#include "azzs/application/startup_assembly_status.hpp"

namespace azzs::composition::windows {

namespace startup = ::azzs::application::startup;

struct StartupAssemblyResult final {
  winrt::Microsoft::UI::Xaml::Window window{nullptr};
  startup::StartupAssemblyStatus status;
};

[[nodiscard]] StartupAssemblyResult assemble_startup();

}  // namespace azzs::composition::windows
