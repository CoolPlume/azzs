#include "pch.h"

#include "composition_root.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "../../adapters/ui/winui/MainWindow.xaml.h"
#include "azzs/adapters/infrastructure/local_file_log_storage.hpp"
#include "azzs/adapters/infrastructure/state_operation_occupancy_storage.hpp"
#include "azzs/adapters/infrastructure/structured_execution_log.hpp"
#include "azzs/adapters/infrastructure/system_clock.hpp"
#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/adapters/windows/windows_lease_token_source.hpp"
#include "azzs/adapters/windows/windows_platform_info.hpp"
#include "azzs/adapters/windows/windows_state_file_system.hpp"
#include "azzs/application/clock.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/workbench.hpp"
#include "azzs/application/workbench_services.hpp"

namespace azzs::composition::windows {
namespace {

class WindowsWorkbenchServices final : public application::WorkbenchServices {
 public:
  explicit WindowsWorkbenchServices(
      adapters::windows::DeviceDataEnvironment environment)
      : state_subject_{environment.subject_id},
        state_files_(environment),
        states_(state_files_, clock_),
        log_storage_(environment.root_utf8, environment.subject_id),
        log_(log_storage_, clock_),
        occupancy_storage_(states_),
        occupancy_(occupancy_storage_, lease_tokens_) {}

  [[nodiscard]] domain::StateSubject const& state_subject()
      const noexcept override {
    return state_subject_;
  }

  [[nodiscard]] application::DeviceStateStore& device_states()
      noexcept override {
    return states_;
  }

  [[nodiscard]] application::ExecutionLog& execution_log()
      noexcept override {
    return log_;
  }

  [[nodiscard]] application::SharedOperationOccupancy& operation_occupancy()
      noexcept override {
    return occupancy_;
  }

 private:
  domain::StateSubject state_subject_;
  adapters::infrastructure::SystemClock clock_;
  adapters::windows::WindowsStateFileSystem state_files_;
  application::DeviceStateStore states_;
  adapters::infrastructure::LocalFileLogStorage log_storage_;
  adapters::infrastructure::StructuredExecutionLog log_;
  adapters::infrastructure::StateOperationOccupancyStorage occupancy_storage_;
  adapters::windows::WindowsLeaseTokenSource lease_tokens_;
  application::SharedOperationOccupancy occupancy_;
};

}  // namespace

winrt::Microsoft::UI::Xaml::Window create_main_window() {
  adapters::windows::WindowsPlatformInfo const platform_info;
  auto environment =
      adapters::windows::WindowsDeviceDataEnvironment::prepare();
  if (!environment) {
    throw std::runtime_error(
        "device data environment preparation failed (raw=" +
        std::to_string(environment.raw_error) + "): " + environment.detail);
  }
  auto services = std::make_shared<WindowsWorkbenchServices>(
      std::move(*environment.environment));
  auto workbench =
      std::make_shared<application::Workbench>(platform_info, services);
  auto window = winrt::make_self<winrt::Azzs::Ui::implementation::MainWindow>();
  window->bind(std::move(workbench));
  return window.as<winrt::Microsoft::UI::Xaml::Window>();
}

}  // namespace azzs::composition::windows
