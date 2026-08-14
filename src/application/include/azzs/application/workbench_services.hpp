#pragma once

namespace azzs::domain {
struct StateSubject;
}

namespace azzs::application {

namespace architecture_selection {
class ArchitectureSelectionLifecycle;
}

namespace software_selection {
class SoftwareSelectionLifecycle;
}

namespace offline_package_cache {
class OfflinePackageCacheService;
}

namespace installation_batch {
class InstallationBatchService;
}

namespace sogou_optimization {
class SogouOptimizationService;
}

namespace software_optimization_discovery {
class SoftwareOptimizationDiscoveryService;
}

class HardwareOverviewService;

class DeviceStateStore;
class EmergencyWithdrawalService;
class ExecutionLog;
class SharedOperationOccupancy;
class SystemSettingsApplyService;
class ApplicationUpdateLifecycle;

// The application-facing service bundle assembled exactly once by the host.
// Concrete filesystem, lock and Win32 types remain behind these typed seams.
class WorkbenchServices {
 public:
  virtual ~WorkbenchServices() = default;

  [[nodiscard]] virtual domain::StateSubject const& state_subject()
      const noexcept = 0;
  [[nodiscard]] virtual DeviceStateStore& device_states() noexcept = 0;
  [[nodiscard]] virtual EmergencyWithdrawalService& emergency_withdrawals()
      noexcept = 0;
  [[nodiscard]] virtual ExecutionLog& execution_log() noexcept = 0;
  [[nodiscard]] virtual SharedOperationOccupancy& operation_occupancy()
      noexcept = 0;
  [[nodiscard]] virtual sogou_optimization::SogouOptimizationService&
  sogou_optimizations() noexcept = 0;
  [[nodiscard]] virtual software_optimization_discovery::
      SoftwareOptimizationDiscoveryService&
  software_optimization_discovery() noexcept = 0;
  [[nodiscard]] virtual SystemSettingsApplyService& system_settings_apply()
      noexcept = 0;
  [[nodiscard]] virtual ApplicationUpdateLifecycle& application_updates()
      noexcept = 0;
  [[nodiscard]] virtual architecture_selection::ArchitectureSelectionLifecycle&
  architecture_selection() noexcept = 0;
  [[nodiscard]] virtual software_selection::SoftwareSelectionLifecycle&
  software_selection() noexcept = 0;
  [[nodiscard]] virtual HardwareOverviewService& hardware_overview()
      noexcept = 0;
  [[nodiscard]] virtual offline_package_cache::OfflinePackageCacheService&
  offline_package_cache() noexcept = 0;
  // Batch state remains application-owned. Hosts and WinUI can only observe
  // its snapshot or submit typed commands through this service.
  [[nodiscard]] virtual installation_batch::InstallationBatchService&
  installation_batches() noexcept = 0;
};

}  // namespace azzs::application
