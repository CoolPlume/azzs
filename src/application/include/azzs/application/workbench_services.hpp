#pragma once

namespace azzs::domain {
struct StateSubject;
}

namespace azzs::application {

namespace architecture_selection {
class ArchitectureSelectionLifecycle;
}

class DeviceStateStore;
class ExecutionLog;
class SharedOperationOccupancy;

// The application-facing service bundle assembled exactly once by the host.
// Concrete filesystem, lock and Win32 types remain behind these typed seams.
class WorkbenchServices {
 public:
  virtual ~WorkbenchServices() = default;

  [[nodiscard]] virtual domain::StateSubject const& state_subject()
      const noexcept = 0;
  [[nodiscard]] virtual DeviceStateStore& device_states() noexcept = 0;
  [[nodiscard]] virtual ExecutionLog& execution_log() noexcept = 0;
  [[nodiscard]] virtual SharedOperationOccupancy& operation_occupancy()
      noexcept = 0;
  [[nodiscard]] virtual architecture_selection::ArchitectureSelectionLifecycle&
  architecture_selection() noexcept = 0;
};

}  // namespace azzs::application
