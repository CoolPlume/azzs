#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "azzs/application/hardware_overview.hpp"

namespace azzs::application {
class DeviceStateStore;
class ExecutionLog;
}

namespace azzs::application::restart_resume {
class RestartResumeService;
}

namespace azzs::application::driver_acquisition {

// These are fixed product-owned handoff targets. No UI command can supply an
// address, executable path, package identifier, or driver match expression.
enum class DriverEntrypoint {
  amd_software,
  intel_driver_assistant,
  nvidia_drivers,
  dell_support,
  hp_support,
  lenovo_support,
  asus_support,
};

enum class DriverAssistantAction {
  open_page,
  install,
  launch,
};

enum class DriverAcquisitionState {
  not_restored,
  ready,
  handoff_in_progress,
  awaiting_user_decision,
  waiting_for_restart,
  read_only,
};

enum class DriverHandoffDecision {
  completed_externally,
  restart_required,
  skip_for_now,
};

enum class DriverActionCode {
  succeeded,
  not_restored,
  read_only,
  rejected,
  persistence_failed,
  launcher_failed,
  restart_barrier_failed,
};

struct DriverPostHandoffObservation final {
  HardwareOverviewState hardware_state{HardwareOverviewState::not_observed};
  bool network_available{false};
};

struct DriverAcquisitionSnapshot final {
  DriverAcquisitionState state{DriverAcquisitionState::not_restored};
  bool writable{false};
  bool assistant_installed{false};
  std::optional<DriverEntrypoint> active_entrypoint;
  std::vector<DriverEntrypoint> recommended_entrypoints;
  std::optional<DriverPostHandoffObservation> last_observation;
  std::string detail;
};

struct DriverActionResult final {
  DriverActionCode code{DriverActionCode::rejected};
  DriverAcquisitionSnapshot snapshot;
  std::optional<HardwareOverviewSnapshot> refreshed_hardware;
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == DriverActionCode::succeeded;
  }
};

// The Windows adapter can only report one deliberately selected assistant and
// open the fixed entrypoint matching this enum. It never receives a package,
// an INF, an arbitrary URL, or a command line from the UI.
class DriverHandoffPlatform {
 public:
  virtual ~DriverHandoffPlatform() = default;
  [[nodiscard]] virtual bool assistant_installed() const noexcept = 0;
  [[nodiscard]] virtual bool open(DriverEntrypoint entrypoint,
                                  DriverAssistantAction action,
                                  std::string& error) = 0;
};

// This is an observation seam only. It must not initiate a connection or
// change any adapter state while recording the external-return boundary.
class DriverNetworkObserver {
 public:
  virtual ~DriverNetworkObserver() = default;
  [[nodiscard]] virtual bool available() const noexcept = 0;
};

// Owns the durable driver handoff state. It deliberately records only that a
// user completed an external flow and the resulting observations; it never
// declares a driver installed or verifies an external driver's success.
class DriverAcquisitionService final {
 public:
  DriverAcquisitionService(
      DeviceStateStore& states, HardwareOverviewService& hardware,
      DriverHandoffPlatform& platform, DriverNetworkObserver const& network,
      ExecutionLog& log, restart_resume::RestartResumeService& restart_resume);
  ~DriverAcquisitionService();

  [[nodiscard]] DriverActionResult restore();
  [[nodiscard]] DriverAcquisitionSnapshot snapshot() const;
  [[nodiscard]] DriverActionResult begin_external_handoff(
      DriverEntrypoint entrypoint);
  [[nodiscard]] DriverActionResult external_flow_returned();
  [[nodiscard]] DriverActionResult decide(DriverHandoffDecision decision);
  // Called only during the existing restart-resume service's read-only
  // recovery. It changes no driver state outside the persisted handoff record.
  [[nodiscard]] DriverActionResult recover_after_restart();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] char const* to_string(DriverEntrypoint value) noexcept;
[[nodiscard]] char const* to_string(DriverAcquisitionState value) noexcept;
[[nodiscard]] char const* to_string(DriverActionCode value) noexcept;

}  // namespace azzs::application::driver_acquisition
