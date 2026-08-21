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

namespace software_catalog {
class SoftwareCatalogLifecycle;
}

namespace offline_package_cache {
class OfflinePackageCacheService;
}

namespace installation_batch {
class InstallationBatchService;
class InstallationBatchCreationService;
}

namespace software_optimization_batch {
class SoftwareOptimizationBatchService;
}

namespace sogou_optimization {
class SogouOptimizationService;
}

namespace software_optimization_discovery {
class SoftwareOptimizationDiscoveryService;
}

namespace driver_acquisition {
class DriverAcquisitionService;
}

class HardwareOverviewService;
class HistoryAndLogsService;
class ApplicationSettingsService;
class DebugModeCatalogEditor;

class DeviceStateStore;
class EmergencyWithdrawalService;
class ExecutionLog;
class SharedOperationOccupancy;
class SystemSettingsApplyService;
class ApplicationUpdateLifecycle;
namespace restart_resume {
class RestartResumeService;
}

namespace guided_initialization {
class GuidedInitializationService;
}

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
  [[nodiscard]] virtual HistoryAndLogsService& history_and_logs() noexcept = 0;
  [[nodiscard]] virtual ApplicationSettingsService& application_settings()
      noexcept = 0;
  [[nodiscard]] virtual DebugModeCatalogEditor& debug_mode_catalog_editor()
      noexcept = 0;
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
  [[nodiscard]] virtual software_catalog::SoftwareCatalogLifecycle&
  software_catalog() noexcept = 0;
  [[nodiscard]] virtual software_selection::SoftwareSelectionLifecycle&
  software_selection() noexcept = 0;
  [[nodiscard]] virtual HardwareOverviewService& hardware_overview()
      noexcept = 0;
  [[nodiscard]] virtual driver_acquisition::DriverAcquisitionService&
  driver_acquisition() noexcept = 0;
  [[nodiscard]] virtual offline_package_cache::OfflinePackageCacheService&
  offline_package_cache() noexcept = 0;
  // Batch state remains application-owned. Hosts and WinUI can only observe
  // its snapshot or submit typed commands through this service.
  [[nodiscard]] virtual installation_batch::InstallationBatchService&
  installation_batches() noexcept = 0;
  [[nodiscard]] virtual installation_batch::InstallationBatchCreationService&
  installation_batch_creation() noexcept = 0;
  [[nodiscard]] virtual software_optimization_batch::SoftwareOptimizationBatchService&
  software_optimization_batches() noexcept = 0;
  [[nodiscard]] virtual restart_resume::RestartResumeService&
  restart_resume() noexcept = 0;
  [[nodiscard]] virtual guided_initialization::GuidedInitializationService&
  guided_initialization() noexcept = 0;
};

}  // namespace azzs::application
