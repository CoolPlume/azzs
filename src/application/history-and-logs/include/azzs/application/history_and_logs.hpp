#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/debug_log_policy/debug_log_policy.hpp"
#include "azzs/application/execution_log.hpp"

namespace azzs::application {
class ApplicationUpdateLifecycle;
class Clock;
class HardwareOverviewService;
class PlatformInfo;
class SystemSettingsApplyService;

namespace installation_batch {
class InstallationBatchService;
}

namespace software_optimization_batch {
class SoftwareOptimizationBatchService;
}

namespace software_selection {
class SoftwareSelectionLifecycle;
}

namespace restart_resume {
class RestartResumeService;
}

namespace software_catalog {
class SoftwareCatalogLifecycle;
}

enum class HistoryEntryKind {
  installation_batch,
  software_optimization_batch,
  system_setting_apply,
  system_setting_recovery,
  external_install_handoff,
  restart_resume,
  application_update,
};

// A history fact is either a retained, already-observed value or an explicit
// statement that the owning subsystem could not provide it. Consumers must
// never replace a missing fact with a guessed value.
enum class HistoryFactDisposition {
  obtained,
  not_obtained,
};

struct HistoryFactProjection final {
  std::string key;
  std::string value;
  HistoryFactDisposition disposition{HistoryFactDisposition::not_obtained};
  std::string reason;
};

enum class HistoryTimelineKind {
  snapshot,
  user_command,
  state_transition,
  adapter_result,
  recovery,
  external_handoff,
  coverage_gap,
};

struct HistoryTimelineProjection final {
  HistoryTimelineKind kind{HistoryTimelineKind::snapshot};
  std::optional<std::int64_t> recorded_at_milliseconds;
  std::string state;
  std::string detail;
  std::vector<HistoryFactProjection> facts;
};

struct HistoryEntryProjection final {
  HistoryEntryKind kind{HistoryEntryKind::installation_batch};
  std::string stable_id;
  std::string state;
  std::string detail;
  bool retry{false};
  std::vector<HistoryFactProjection> facts;
  std::vector<HistoryTimelineProjection> timeline;
};

struct HistoryAndLogsFilter final {
  std::string query;
  std::optional<HistoryEntryKind> history_kind;
  std::optional<ExecutionEventKind> event_kind;
  std::optional<ExecutionResult> event_result;
  std::string correlation_id;
};

struct HistoryAndLogsSnapshot final {
  std::vector<HistoryEntryProjection> history;
  ExecutionLogSnapshot log;
  DebugLogPolicySnapshot debug;
  HistoryAndLogsFilter applied_filter;
  std::string detail;
};

enum class HistoryAndLogsActionCode {
  succeeded,
  failed,
};

struct HistoryAndLogsActionResult final {
  HistoryAndLogsActionCode code{HistoryAndLogsActionCode::failed};
  HistoryAndLogsSnapshot snapshot;
  ExecutionLogClearReceipt clear_receipt;
  DiagnosticExportReceipt export_receipt;
  std::string message;
};

// This is the single application-facing owner of the history-and-log page
// projection and commands. It reads typed snapshots from existing owners and
// never exposes the durable execution-log byte format to a UI adapter.
class HistoryAndLogsService final {
 public:
  HistoryAndLogsService(
      Clock const& clock, ApplicationUpdateLifecycle& application_updates,
      PlatformInfo const& platform_info,
      HardwareOverviewService& hardware_overview,
      software_catalog::SoftwareCatalogLifecycle& software_catalog,
      ExecutionLog& log,
      installation_batch::InstallationBatchService& installation_batches,
      software_optimization_batch::SoftwareOptimizationBatchService&
          software_optimization_batches,
      SystemSettingsApplyService& system_settings,
      software_selection::SoftwareSelectionLifecycle& software_selection,
      DebugLogPolicySnapshotSource const* debug_log_policy = nullptr,
      restart_resume::RestartResumeService const* restart_resume = nullptr);

  [[nodiscard]] HistoryAndLogsSnapshot refresh();
  [[nodiscard]] HistoryAndLogsSnapshot refresh(
      HistoryAndLogsFilter const& filter);
  [[nodiscard]] HistoryAndLogsSnapshot locate(std::string_view stable_id);
  [[nodiscard]] HistoryAndLogsActionResult clear_logs();
  [[nodiscard]] HistoryAndLogsActionResult export_diagnostic();

 private:
  [[nodiscard]] DiagnosticContext diagnostic_context(
      HistoryAndLogsSnapshot const& snapshot) const;

  Clock const& clock_;
  ApplicationUpdateLifecycle& application_updates_;
  PlatformInfo const& platform_info_;
  HardwareOverviewService& hardware_overview_;
  software_catalog::SoftwareCatalogLifecycle& software_catalog_;
  ExecutionLog& log_;
  installation_batch::InstallationBatchService& installation_batches_;
  software_optimization_batch::SoftwareOptimizationBatchService&
      software_optimization_batches_;
  SystemSettingsApplyService& system_settings_;
  software_selection::SoftwareSelectionLifecycle& software_selection_;
  DebugLogPolicySnapshotSource const* debug_log_policy_{};
  restart_resume::RestartResumeService const* restart_resume_{};
};

[[nodiscard]] char const* to_string(HistoryEntryKind value) noexcept;
[[nodiscard]] char const* to_string(HistoryFactDisposition value) noexcept;
[[nodiscard]] char const* to_string(HistoryTimelineKind value) noexcept;

}  // namespace azzs::application
