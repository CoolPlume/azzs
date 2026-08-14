#include "azzs/application/history_and_logs.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "azzs/application/application_update.hpp"
#include "azzs/application/clock.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_batch.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/domain/installation_batch.hpp"
#include "azzs/domain/software_optimization_batch.hpp"
#include "azzs/domain/software_selection.hpp"

namespace azzs::application {
namespace {

[[nodiscard]] char const* recovery_status_name(
    RecoveryRecordStatus status) noexcept {
  switch (status) {
    case RecoveryRecordStatus::pending:
      return "pending";
    case RecoveryRecordStatus::applied:
      return "applied";
    case RecoveryRecordStatus::restored:
      return "restored";
    case RecoveryRecordStatus::waiting_explorer_restart:
      return "waiting-explorer-restart";
    case RecoveryRecordStatus::restoring:
      return "restoring";
    case RecoveryRecordStatus::restore_failed:
      return "restore-failed";
  }
  return "unknown";
}

[[nodiscard]] char const* architecture_name(
    domain::SystemArchitecture architecture) noexcept {
  switch (architecture) {
    case domain::SystemArchitecture::x64:
      return "x64";
    case domain::SystemArchitecture::arm64:
      return "arm64";
    case domain::SystemArchitecture::unknown:
      return "unknown";
  }
  return "unknown";
}

[[nodiscard]] char const* release_form_name(
    ApplicationReleaseForm form) noexcept {
  switch (form) {
    case ApplicationReleaseForm::portable:
      return "portable";
    case ApplicationReleaseForm::installed:
      return "installed";
  }
  return "unknown";
}

[[nodiscard]] char const* catalog_identity_name(
    software_catalog::EffectiveCatalogIdentity identity) noexcept {
  switch (identity) {
    case software_catalog::EffectiveCatalogIdentity::released:
      return "released";
    case software_catalog::EffectiveCatalogIdentity::local_trial:
      return "local-trial";
  }
  return "unknown";
}

[[nodiscard]] char const* catalog_release_state_name(
    domain::software_catalog::ReleaseState state) noexcept {
  switch (state) {
    case domain::software_catalog::ReleaseState::draft:
      return "draft";
    case domain::software_catalog::ReleaseState::release:
      return "release";
  }
  return "unknown";
}

void append_missing(DiagnosticContext& context, std::string fact,
                    std::string reason) {
  context.missing_facts.push_back(
      {.fact = std::move(fact), .reason = std::move(reason)});
}

void append_retained(DiagnosticContext& context, std::string key,
                     std::string value) {
  context.fields.push_back({.key = std::move(key),
                            .value = std::move(value),
                            .disposition = DiagnosticValueDisposition::retain});
}

void append_installation_history(
    std::vector<HistoryEntryProjection>& target,
    installation_batch::InstallationBatchService& service) {
  auto const snapshot = service.snapshot();
  for (auto const& record : snapshot.history) {
    target.push_back({
        .kind = HistoryEntryKind::installation_batch,
        .stable_id = record.plan.batch_id,
        .state = domain::installation_batch::to_string(record.final_state),
        .detail = record.reason,
        .retry = record.plan.retry_of_batch_id.has_value(),
    });
  }
  if (snapshot.active.has_value()) {
    auto const& active = *snapshot.active;
    target.push_back({
        .kind = HistoryEntryKind::installation_batch,
        .stable_id = active.plan.batch_id,
        .state = domain::installation_batch::to_string(active.state),
        .detail = active.last_transition.item_id,
        .retry = active.plan.retry_of_batch_id.has_value(),
    });
  }
}

void append_optimization_history(
    std::vector<HistoryEntryProjection>& target,
    software_optimization_batch::SoftwareOptimizationBatchService& service) {
  auto const snapshot = service.snapshot();
  for (auto const& record : snapshot.history) {
    target.push_back({
        .kind = HistoryEntryKind::software_optimization_batch,
        .stable_id = record.plan.batch_id,
        .state =
            domain::software_optimization_batch::to_string(record.final_state),
        .detail = record.reason,
        .retry = record.plan.retry_of_batch_id.has_value(),
    });
  }
  if (snapshot.active.has_value()) {
    auto const& active = *snapshot.active;
    target.push_back({
        .kind = HistoryEntryKind::software_optimization_batch,
        .stable_id = active.plan.batch_id,
        .state = domain::software_optimization_batch::to_string(active.state),
        .detail = active.last_transition.option_id,
        .retry = active.plan.retry_of_batch_id.has_value(),
    });
  }
}

}  // namespace

HistoryAndLogsService::HistoryAndLogsService(
    Clock const& clock, ApplicationUpdateLifecycle& application_updates,
    PlatformInfo const& platform_info, HardwareOverviewService& hardware_overview,
    software_catalog::SoftwareCatalogLifecycle& software_catalog,
    ExecutionLog& log,
    installation_batch::InstallationBatchService& installation_batches,
    software_optimization_batch::SoftwareOptimizationBatchService&
        software_optimization_batches,
    SystemSettingsApplyService& system_settings,
    software_selection::SoftwareSelectionLifecycle& software_selection)
    : clock_(clock),
      application_updates_(application_updates),
      platform_info_(platform_info),
      hardware_overview_(hardware_overview),
      software_catalog_(software_catalog),
      log_(log),
      installation_batches_(installation_batches),
      software_optimization_batches_(software_optimization_batches),
      system_settings_(system_settings),
      software_selection_(software_selection) {}

HistoryAndLogsSnapshot HistoryAndLogsService::refresh() {
  HistoryAndLogsSnapshot result;
  append_installation_history(result.history, installation_batches_);
  append_optimization_history(result.history, software_optimization_batches_);

  for (auto const& record : system_settings_.recovery_records()) {
    result.history.push_back({
        .kind = HistoryEntryKind::system_setting_recovery,
        .stable_id = record.setting_id.value,
        .state = recovery_status_name(record.status),
        .detail = record.display_name,
    });
  }

  auto const selection = software_selection_.snapshot();
  for (auto const& handoff : selection.handoffs) {
    result.history.push_back({
        .kind = HistoryEntryKind::external_install_handoff,
        .stable_id = handoff.software_id,
        .state = domain::software_selection::to_string(handoff.status),
        .detail = handoff.detail,
    });
  }

  result.log = log_.snapshot();
  if (!result.log.available && !result.log.error.empty()) {
    result.detail = "execution-log-unavailable";
  }
  return result;
}

DiagnosticContext HistoryAndLogsService::diagnostic_context(
    HistoryAndLogsSnapshot const& snapshot) const {
  DiagnosticContext context;
  append_retained(context, "history_record_count",
                  std::to_string(snapshot.history.size()));
  append_retained(
      context, "export_requested_at_utc_ms",
      std::to_string(clock_.now().time_since_epoch().count()));
  append_retained(context, "redaction_status", "centralized-before-storage");

  auto const update = application_updates_.snapshot();
  if (update.current.valid()) {
    context.workbench_build = update.current.version;
    context.release_form = release_form_name(update.current.form);
    auto const architecture = architecture_name(update.current.architecture);
    context.process_architecture = architecture;
    context.package_architecture = architecture;
  } else {
    constexpr std::string_view reason{
        "application update owner has no valid current build identity"};
    append_missing(context, "workbench_build", std::string{reason});
    append_missing(context, "release_form", std::string{reason});
    append_missing(context, "process_architecture", std::string{reason});
    append_missing(context, "package_architecture", std::string{reason});
  }

  if (auto const version = platform_info_.windows_version()) {
    context.windows_version = std::to_string(version->major) + "." +
                              std::to_string(version->minor) + "." +
                              std::to_string(version->build);
  } else {
    append_missing(context, "windows_version",
                   "platform owner could not read the Windows version");
  }
  append_missing(context, "language",
                 "no language projection was provided by the host");
  append_missing(context, "timezone",
                 "no timezone projection was provided by the host");

  auto const hardware = hardware_overview_.snapshot();
  if (hardware.observation.has_value()) {
    auto const& observed = *hardware.observation;
    if (!observed.oem_model.empty()) {
      append_retained(context, "hardware_model", observed.oem_model);
    } else {
      append_missing(context, "hardware_model",
                     "hardware owner did not provide a non-unique model");
    }
    if (!observed.cpu.empty()) {
      append_retained(context, "hardware_cpu", observed.cpu);
    }
    if (!observed.gpu.empty()) {
      append_retained(context, "hardware_gpu", observed.gpu);
    }
    if (!observed.network_adapter.empty()) {
      append_retained(context, "network_adapter_category",
                      observed.network_adapter);
    }
  } else {
    append_missing(context, "hardware_model",
                   hardware.error.empty()
                       ? "hardware owner has no observed snapshot"
                       : hardware.error);
  }
  append_missing(context, "network_state",
                 "no network state projection was provided by the host");
  append_missing(context, "module_stack",
                 "platform did not provide a sanitized module projection");

  auto const catalog = software_catalog_.snapshot();
  if (catalog.current.has_value()) {
    auto const& current = *catalog.current;
    append_retained(context, "catalog_revision",
                    std::to_string(current.revision));
    append_retained(context, "catalog_identity",
                    catalog_identity_name(current.identity));
    if (!current.content_identity.empty()) {
      append_retained(context, "catalog_content_identity",
                      current.content_identity);
    }
    if (!current.application_id.empty()) {
      append_retained(context, "catalog_application_id", current.application_id);
    } else {
      append_missing(context, "catalog_application_id",
                     "active catalog has no application association identifier");
    }
    append_retained(context, "catalog_release_issue_count",
                    std::to_string(current.release_issues.size()));
    append_retained(context, "catalog_release_gate",
                    current.release_issues.empty() ? "passed" : "failed");
  } else {
    auto const reason = catalog.error.empty()
                            ? std::string{"catalog owner has no active catalog"}
                            : catalog.error;
    append_missing(context, "catalog_revision", reason);
    append_missing(context, "catalog_identity", reason);
    append_missing(context, "catalog_application_id", reason);
    append_missing(context, "catalog_release_gate", reason);
  }
  if (catalog.current_document.has_value() &&
      catalog.current_document->release_state.has_value()) {
    append_retained(context, "catalog_release_state",
                    catalog_release_state_name(
                        *catalog.current_document->release_state));
    append_retained(context, "catalog_schema",
                    std::to_string(catalog.current_document->schema_version));
  } else {
    append_missing(context, "catalog_release_state",
                   "catalog owner did not provide an active document release state");
    append_missing(context, "catalog_schema",
                   "catalog owner did not provide an active document schema");
  }
  if (catalog.current_catalog.has_value()) {
    append_retained(context, "catalog_runtime_load", "accepted");
  } else {
    append_missing(context, "catalog_runtime_load",
                   "catalog owner has no runtime load projection");
  }

  if (!snapshot.log.available) {
    append_missing(context, "execution_log",
                   snapshot.log.error.empty()
                       ? "execution log projection is unavailable"
                       : snapshot.log.error);
  } else {
    append_retained(context, "execution_log_bytes",
                    std::to_string(snapshot.log.durable_bytes));
    append_retained(context, "execution_log_coverage_gap_count",
                    std::to_string(snapshot.log.coverage_gap_count));
    if (snapshot.log.pending_clear.has_value()) {
      append_retained(
          context, "clear_cutoff_segment",
          std::to_string(snapshot.log.pending_clear->cutoff_segment));
      append_retained(
          context, "clear_cutoff_sequence",
          std::to_string(snapshot.log.pending_clear->cutoff_sequence));
      append_missing(
          context, "active_log_segment",
          "clear was committed but new segment completion is unconfirmed");
      append_missing(
          context, "last_log_sequence",
          "clear was committed but new segment completion is unconfirmed");
    } else {
      append_retained(context, "active_log_segment",
                      std::to_string(snapshot.log.active_segment));
      append_retained(context, "last_log_sequence",
                      std::to_string(snapshot.log.last_sequence));
    }
    if (snapshot.log.coverage_gap_count != 0) {
      append_missing(
          context, "execution_log_coverage",
          std::to_string(snapshot.log.coverage_gap_count) +
              " recorded coverage gap events are present");
    }
  }
  if (snapshot.log.coverage_started_at.has_value()) {
    context.coverage_started_at = snapshot.log.coverage_started_at;
  } else {
    append_missing(context, "coverage_start",
                   "current log projection has no recorded coverage start");
  }
  if (snapshot.log.coverage_ended_at.has_value()) {
    context.coverage_ended_at = snapshot.log.coverage_ended_at;
  } else {
    append_missing(context, "coverage_end",
                   "current log projection has no recorded coverage end");
  }
  return context;
}

HistoryAndLogsActionResult HistoryAndLogsService::clear_logs() {
  HistoryAndLogsActionResult result;
  result.clear_receipt = log_.clear();
  result.snapshot = refresh();
  result.code = result.clear_receipt.cleared
                    ? HistoryAndLogsActionCode::succeeded
                    : HistoryAndLogsActionCode::failed;
  result.message = result.clear_receipt.cleared ? "execution-log-cleared"
                                                 : "execution-log-clear-failed";
  return result;
}

HistoryAndLogsActionResult HistoryAndLogsService::export_diagnostic() {
  HistoryAndLogsActionResult result;
  auto const before_export = refresh();
  result.export_receipt = log_.export_diagnostic(
      diagnostic_context(before_export));
  result.snapshot = refresh();
  result.code = result.export_receipt.produced
                    ? HistoryAndLogsActionCode::succeeded
                    : HistoryAndLogsActionCode::failed;
  result.message = result.export_receipt.produced
                       ? "diagnostic-exported"
                       : "diagnostic-export-failed";
  return result;
}

char const* to_string(HistoryEntryKind value) noexcept {
  switch (value) {
    case HistoryEntryKind::installation_batch:
      return "installation-batch";
    case HistoryEntryKind::software_optimization_batch:
      return "software-optimization-batch";
    case HistoryEntryKind::system_setting_recovery:
      return "system-setting-recovery";
    case HistoryEntryKind::external_install_handoff:
      return "external-install-handoff";
  }
  return "unknown";
}

}  // namespace azzs::application
