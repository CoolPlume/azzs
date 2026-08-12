#include "pch.h"

#include "composition_root.hpp"

#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../../adapters/ui/winui/DesignSystem/motion_preferences.hpp"
#include "../../adapters/ui/winui/MainWindow.xaml.h"
#include "azzs/adapters/infrastructure/local_file_log_storage.hpp"
#include "azzs/adapters/infrastructure/state_application_update_health_storage.hpp"
#include "azzs/adapters/infrastructure/state_operation_occupancy_storage.hpp"
#include "azzs/adapters/infrastructure/structured_execution_log.hpp"
#include "azzs/adapters/infrastructure/system_clock.hpp"
#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/adapters/windows/windows_application_update_platform.hpp"
#include "azzs/adapters/windows/windows_emergency_withdrawal_notice_source.hpp"
#include "azzs/adapters/windows/windows_external_address_launcher.hpp"
#include "azzs/adapters/windows/windows_hardware_observer.hpp"
#include "azzs/adapters/windows/windows_lease_token_source.hpp"
#include "azzs/adapters/windows/windows_platform_info.hpp"
#include "azzs/adapters/windows/windows_state_file_system.hpp"
#include "azzs/adapters/windows/windows_sogou_optimization_adapter.hpp"
#include "azzs/application/clock.hpp"
#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/application_update.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/emergency_withdrawal_service.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/sogou_optimization.hpp"
#include "azzs/application/workbench.hpp"
#include "azzs/application/workbench_services.hpp"

namespace azzs::composition::windows {
namespace {

class UnavailableControlledSourceResolver final
    : public application::software_selection::ControlledSourceResolver {
 public:
  [[nodiscard]] application::software_selection::SourceResolutionResult resolve(
      std::string_view,
      domain::software_catalog::CatalogSource const&) override {
    return {.error = "controlled source resolution is not configured"};
  }
};

class OfflineNetworkObserver final
    : public application::software_selection::NetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override { return false; }
};

class UnavailableSoftwarePresenceDetector final
    : public application::software_selection::SoftwarePresenceDetector {
 public:
  [[nodiscard]] application::software_selection::PresenceDetection detect(
      std::string_view) override {
    return {.detail = "software presence detection is not configured"};
  }
};

class WindowsWorkbenchServices final
    : public application::WorkbenchServices,
      public std::enable_shared_from_this<WindowsWorkbenchServices> {
 public:
  explicit WindowsWorkbenchServices(
      adapters::windows::DeviceDataEnvironment environment)
      : state_subject_{environment.subject_id},
        clock_{},
        hardware_observer_{},
        hardware_overview_(hardware_observer_, clock_),
        state_files_(environment),
        states_(state_files_, clock_),
        log_storage_(environment.root_utf8, environment.subject_id),
        log_(log_storage_, clock_),
        emergency_notice_source_(),
        emergency_withdrawals_(
            states_, clock_, emergency_notice_source_,
            {.log = &log_, .correlation = log_.begin_correlation()}),
        occupancy_storage_(states_),
        occupancy_(occupancy_storage_, lease_tokens_),
        sogou_optimization_adapter_{},
        sogou_optimizations_(sogou_optimization_adapter_,
                             sogou_optimization_adapter_),
        platform_info_{},
        operation_activity_(occupancy_),
        application_update_health_storage_(states_),
        application_update_platform_(platform_info_,
                                     application_update_health_storage_),
        application_updates_(application_update_platform_, operation_activity_,
                             log_, clock_),
        architecture_selection_(platform_info_, log_,
                                application::architecture_selection::
                                    selection_domain::ArchitecturePreference::
                                        prefer_arm64_prompt_fallback),
        software_selection_(states_, clock_, log_, architecture_selection_,
                            source_resolver_, network_, presence_detector_,
                            external_launcher_, state_subject_) {
    static_cast<void>(architecture_selection_.start());
    static_cast<void>(software_selection_.restore());
  }

  void start_emergency_preflight() {
    std::call_once(emergency_preflight_started_, [this] {
      auto self = shared_from_this();
      std::thread([self = std::move(self)] {
        try {
          (void)self->emergency_withdrawals_.preflight_check();
        } catch (...) {
          // The service owns the typed failure path; the adapter boundary must
          // never terminate the workbench because a startup check threw.
        }
      }).detach();
    });
  }

  [[nodiscard]] domain::StateSubject const& state_subject()
      const noexcept override {
    return state_subject_;
  }

  [[nodiscard]] application::DeviceStateStore& device_states()
      noexcept override {
    return states_;
  }

  [[nodiscard]] application::EmergencyWithdrawalService&
  emergency_withdrawals() noexcept override {
    return emergency_withdrawals_;
  }

  [[nodiscard]] application::ExecutionLog& execution_log()
      noexcept override {
    return log_;
  }

  [[nodiscard]] application::SharedOperationOccupancy& operation_occupancy()
      noexcept override {
    return occupancy_;
  }

  [[nodiscard]] application::sogou_optimization::SogouOptimizationService&
  sogou_optimizations() noexcept override {
    return sogou_optimizations_;
  }

  [[nodiscard]] application::ApplicationUpdateLifecycle& application_updates()
      noexcept override {
    return application_updates_;
  }

  [[nodiscard]] application::architecture_selection::
      ArchitectureSelectionLifecycle& architecture_selection()
      noexcept override {
    return architecture_selection_;
  }

  [[nodiscard]] application::software_selection::SoftwareSelectionLifecycle&
  software_selection() noexcept override {
    return software_selection_;
  }

  [[nodiscard]] application::HardwareOverviewService& hardware_overview()
      noexcept override {
    return hardware_overview_;
  }

  [[nodiscard]] adapters::windows::WindowsPlatformInfo const& platform_info()
      const noexcept {
    return platform_info_;
  }

 private:
  domain::StateSubject state_subject_;
  adapters::infrastructure::SystemClock clock_;
  adapters::windows::WindowsHardwareObserver hardware_observer_;
  application::HardwareOverviewService hardware_overview_;
  adapters::windows::WindowsStateFileSystem state_files_;
  application::DeviceStateStore states_;
  adapters::infrastructure::LocalFileLogStorage log_storage_;
  adapters::infrastructure::StructuredExecutionLog log_;
  std::once_flag emergency_preflight_started_;
  adapters::windows::WindowsEmergencyWithdrawalNoticeSource
      emergency_notice_source_;
  application::EmergencyWithdrawalService emergency_withdrawals_;
  adapters::infrastructure::StateOperationOccupancyStorage occupancy_storage_;
  adapters::windows::WindowsLeaseTokenSource lease_tokens_;
  application::SharedOperationOccupancy occupancy_;
  adapters::windows::WindowsSogouOptimizationAdapter
      sogou_optimization_adapter_;
  application::sogou_optimization::SogouOptimizationService
      sogou_optimizations_;
  adapters::windows::WindowsPlatformInfo platform_info_;
  class OperationActivity final
      : public application::InitializationOperationActivity {
   public:
    explicit OperationActivity(application::SharedOperationOccupancy& occupancy)
        : occupancy_(occupancy) {}

    [[nodiscard]] application::InitializationOperationActivitySnapshot observe()
        override {
      auto occupied = occupancy_.inspect();
      if (occupied.code == application::OccupancyResultCode::observed) {
        if (!occupied.current.has_value()) {
          return {};
        }
        return {.phase = application::InitializationOperationPhase::active,
                .operation_id = occupied.current->identity.operation_id,
                .detail = "another workbench initialization operation is active"};
      }
      return {.phase = application::InitializationOperationPhase::
                         observation_unavailable,
              .detail = occupied.detail.empty()
                            ? "initialization operation occupancy is unavailable"
                            : std::move(occupied.detail)};
    }

   private:
    application::SharedOperationOccupancy& occupancy_;
  } operation_activity_;
  adapters::infrastructure::StateApplicationUpdateHealthStorage
      application_update_health_storage_;
  adapters::windows::WindowsApplicationUpdatePlatform application_update_platform_;
  application::ApplicationUpdateLifecycle application_updates_;
  application::architecture_selection::ArchitectureSelectionLifecycle
      architecture_selection_;
  UnavailableControlledSourceResolver source_resolver_;
  OfflineNetworkObserver network_;
  UnavailableSoftwarePresenceDetector presence_detector_;
  adapters::windows::WindowsExternalAddressLauncher external_launcher_;
  application::software_selection::SoftwareSelectionLifecycle
      software_selection_;
};

}  // namespace

winrt::Microsoft::UI::Xaml::Window create_main_window() {
  auto environment =
      adapters::windows::WindowsDeviceDataEnvironment::prepare();
  if (!environment) {
    throw std::runtime_error(
        "device data environment preparation failed (raw=" +
        std::to_string(environment.raw_error) + "): " + environment.detail);
  }
  auto services = std::make_shared<WindowsWorkbenchServices>(
      std::move(*environment.environment));
  auto workbench = std::make_shared<application::Workbench>(
      services->platform_info(), services);
  services->start_emergency_preflight();
  auto motion_preferences = ui::winui::MotionPreferences::create();
  auto window = winrt::make_self<winrt::Azzs::Ui::implementation::MainWindow>();
  window->bind(std::move(workbench), std::move(motion_preferences));
  return window.as<winrt::Microsoft::UI::Xaml::Window>();
}

}  // namespace azzs::composition::windows
