#include "azzs/application/history_and_logs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "azzs/application/application_update.hpp"
#include "azzs/application/clock.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/application/restart_resume.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_batch.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/domain/installation_batch.hpp"
#include "azzs/domain/software_optimization_batch.hpp"
#include "azzs/domain/software_selection.hpp"

namespace azzs::application {
namespace {

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

[[nodiscard]] char const* catalog_origin_name(
    software_catalog::CatalogCandidateOrigin origin) noexcept {
  switch (origin) {
    case software_catalog::CatalogCandidateOrigin::built_in:
      return "built-in";
    case software_catalog::CatalogCandidateOrigin::update:
      return "update";
    case software_catalog::CatalogCandidateOrigin::manual_import:
      return "manual-import";
    case software_catalog::CatalogCandidateOrigin::saved_draft:
      return "saved-draft";
    case software_catalog::CatalogCandidateOrigin::rollback:
      return "rollback";
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

void append_fact(std::vector<HistoryFactProjection>& target, std::string key,
                 std::string value) {
  target.push_back({.key = std::move(key),
                    .value = std::move(value),
                    .disposition = HistoryFactDisposition::obtained});
}

void append_unavailable_fact(std::vector<HistoryFactProjection>& target,
                             std::string key, std::string reason) {
  target.push_back({.key = std::move(key),
                    .disposition = HistoryFactDisposition::not_obtained,
                    .reason = std::move(reason)});
}

[[nodiscard]] std::string normalize_history_state_token(
    std::string_view token) {
  std::string normalized;
  normalized.reserve(token.size());
  for (auto const character : token) {
    normalized.push_back(
        character == '-'
            ? '_'
            : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return normalized;
}

[[nodiscard]] std::string canonical_history_display_state(
    HistoryEntryKind kind, std::string_view raw_token) {
  auto const token = normalize_history_state_token(raw_token);
  switch (kind) {
    case HistoryEntryKind::installation_batch:
      if (token == "skipped_installed") {
        return "已安装";
      }
      if (token == "result_confirmation_pending") {
        return "安装结果待确认";
      }
      if (token == "installer_interaction_pending") {
        return "安装器交互待处理";
      }
      if (token == "waiting_network") {
        return "等待联网";
      }
      if (token == "waiting_restart") {
        return "等待重启";
      }
      if (token == "source_invalid") {
        return "来源失效";
      }
      if (token == "failed") {
        return "安装失败";
      }
      if (token == "pending" || token == "dependency_blocked" ||
          token == "stop_pending") {
        return "未执行";
      }
      break;
    case HistoryEntryKind::software_optimization_batch:
      if (token == "optimized") {
        return "已优化";
      }
      if (token == "result_confirmation_pending") {
        return "优化结果待确认";
      }
      if (token == "waiting_restart") {
        return "等待重启";
      }
      if (token == "failed") {
        return "优化失败";
      }
      if (token == "blocked_by_withdrawal") {
        return "紧急撤回优化方案";
      }
      if (token == "not_executed" || token == "pending" ||
          token == "awaiting_target_exit" ||
          token == "force_close_confirmation_pending") {
        return "未执行";
      }
      break;
    case HistoryEntryKind::system_setting_apply:
      if (token == "already_effective" || token == "applied") {
        return "已生效";
      }
      if (token == "waiting_explorer_restart") {
        return "等待资源管理器重启";
      }
      if (token == "not_applicable" ||
          token == "force_confirmation_required") {
        return "可能不适用";
      }
      if (token == "not_selected" || token == "blocked_by_dependency") {
        return "未执行";
      }
      break;
    case HistoryEntryKind::system_setting_recovery:
      if (token == "restored") {
        return "已恢复";
      }
      break;
    case HistoryEntryKind::external_install_handoff:
      if (token == "externally_recognized") {
        return "外部安装已识别";
      }
      if (token == "skipped") {
        return "暂时跳过";
      }
      break;
    case HistoryEntryKind::restart_resume:
    case HistoryEntryKind::application_update:
      break;
  }
  return std::string{raw_token};
}

void canonicalize_history_state(HistoryEntryKind kind, std::string& state,
                                std::string& detail,
                                std::vector<HistoryFactProjection>& facts) {
  auto const raw_token = state;
  auto display_state = canonical_history_display_state(kind, raw_token);
  if (display_state == raw_token) {
    return;
  }
  append_fact(facts, "state.raw_token", raw_token);
  if (!detail.empty()) {
    append_fact(facts, "state.raw_detail", detail);
    detail = raw_token + ": " + detail;
  } else {
    detail = raw_token;
  }
  state = std::move(display_state);
}

void canonicalize_history_entry(HistoryEntryProjection& entry) {
  canonicalize_history_state(entry.kind, entry.state, entry.detail, entry.facts);
  for (auto& timeline : entry.timeline) {
    canonicalize_history_state(entry.kind, timeline.state, timeline.detail,
                               timeline.facts);
  }
}

[[nodiscard]] char const* execution_result_name(ExecutionResult result) {
  switch (result) {
    case ExecutionResult::started:
      return "started";
    case ExecutionResult::succeeded:
      return "succeeded";
    case ExecutionResult::failed:
      return "failed";
    case ExecutionResult::cancelled:
      return "cancelled";
    case ExecutionResult::unknown:
      return "unknown";
  }
  return "unknown";
}

[[nodiscard]] HistoryTimelineKind timeline_kind(ExecutionEventKind kind) {
  switch (kind) {
    case ExecutionEventKind::user_command:
      return HistoryTimelineKind::user_command;
    case ExecutionEventKind::state_transition:
      return HistoryTimelineKind::state_transition;
    case ExecutionEventKind::adapter_result:
      return HistoryTimelineKind::adapter_result;
    case ExecutionEventKind::coverage_gap:
      return HistoryTimelineKind::coverage_gap;
  }
  return HistoryTimelineKind::snapshot;
}

[[nodiscard]] HistoryTimelineProjection timeline_from_event(
    ExecutionLogEventProjection const& event) {
  HistoryTimelineProjection result{
      .kind = timeline_kind(event.kind),
      .recorded_at_milliseconds = event.recorded_at_milliseconds,
      .state = execution_result_name(event.result),
      .detail = event.stage,
  };
  append_fact(result.facts, "event.segment", std::to_string(event.segment));
  append_fact(result.facts, "event.sequence", std::to_string(event.sequence));
  append_fact(result.facts, "event.correlation", event.correlation.value);
  append_fact(result.facts, "event.component", event.component);
  append_fact(result.facts, "event.stage", event.stage);
  for (auto const& field : event.fields) {
    append_fact(result.facts, "event." + field.key, field.value);
  }
  if (event.error.has_value()) {
    append_fact(result.facts, "event.error_source", event.error->source);
    append_fact(result.facts, "event.error_code",
                std::to_string(event.error->code));
    append_fact(result.facts, "event.error_message", event.error->message);
  }
  if (event.last_trusted_state.has_value()) {
    append_fact(result.facts, "event.last_trusted_generation",
                std::to_string(event.last_trusted_state->generation));
    append_fact(result.facts, "event.last_trusted_summary",
                event.last_trusted_state->summary);
  }
  if (event.coverage_gap.has_value()) {
    append_fact(result.facts, "event.coverage_gap_reason",
                event.coverage_gap->reason);
    if (event.coverage_gap->first_missing_sequence.has_value()) {
      append_fact(result.facts, "event.coverage_gap_first_sequence",
                  std::to_string(
                      *event.coverage_gap->first_missing_sequence));
    }
    if (event.coverage_gap->last_missing_sequence.has_value()) {
      append_fact(result.facts, "event.coverage_gap_last_sequence",
                  std::to_string(*event.coverage_gap->last_missing_sequence));
    }
  }
  return result;
}

void append_correlation_timeline(HistoryEntryProjection& target,
                                 ExecutionLogSnapshot const& log,
                                 std::string_view correlation) {
  if (!log.available) {
    append_unavailable_fact(
        target.facts, "execution_timeline",
        log.error.empty() ? "execution log projection is unavailable"
                          : log.error);
    return;
  }
  if (correlation.empty()) {
    append_unavailable_fact(target.facts, "execution_timeline",
                            "the durable record has no correlation identifier");
    return;
  }
  bool found = false;
  for (auto const& event : log.events) {
    if (event.correlation.value != correlation) {
      continue;
    }
    target.timeline.push_back(timeline_from_event(event));
    found = true;
  }
  if (!found) {
    append_unavailable_fact(
        target.facts, "execution_timeline",
        "no retained execution events matched the durable correlation identifier");
  }
}

[[nodiscard]] bool event_mentions_id(ExecutionLogEventProjection const& event,
                                      std::string_view stable_id) {
  if (stable_id.empty()) {
    return false;
  }
  return std::ranges::any_of(event.fields, [&](auto const& field) {
    return (field.key == "software_id" || field.key == "item_id" ||
            field.key == "setting_id" || field.key == "operation_id") &&
           field.value == stable_id;
  });
}

void append_id_timeline(HistoryEntryProjection& target,
                        ExecutionLogSnapshot const& log,
                        std::string_view stable_id) {
  if (!log.available) {
    append_unavailable_fact(
        target.facts, "execution_timeline",
        log.error.empty() ? "execution log projection is unavailable"
                          : log.error);
    return;
  }
  bool found = false;
  for (auto const& event : log.events) {
    if (!event_mentions_id(event, stable_id)) {
      continue;
    }
    target.timeline.push_back(timeline_from_event(event));
    found = true;
  }
  if (!found) {
    append_unavailable_fact(target.facts, "execution_timeline",
                            "no retained execution events named this stable identifier");
  }
}

[[nodiscard]] char const* installation_durable_outcome_name(
    domain::installation_batch::DurableTransitionOutcome outcome) noexcept {
  switch (outcome) {
    case domain::installation_batch::DurableTransitionOutcome::committed:
      return "committed";
    case domain::installation_batch::DurableTransitionOutcome::outcome_unknown:
      return "outcome-unknown";
    case domain::installation_batch::DurableTransitionOutcome::failed_closed:
      return "failed-closed";
  }
  return "unknown";
}

[[nodiscard]] char const* optimization_durable_outcome_name(
    domain::software_optimization_batch::DurableTransitionOutcome outcome)
    noexcept {
  switch (outcome) {
    case domain::software_optimization_batch::DurableTransitionOutcome::committed:
      return "committed";
    case domain::software_optimization_batch::DurableTransitionOutcome::
        outcome_unknown:
      return "outcome-unknown";
    case domain::software_optimization_batch::DurableTransitionOutcome::
        failed_closed:
      return "failed-closed";
  }
  return "unknown";
}

[[nodiscard]] char const* optimization_risk_name(
    domain::software_optimization_catalog::RiskLevel risk) noexcept {
  switch (risk) {
    case domain::software_optimization_catalog::RiskLevel::low:
      return "low";
    case domain::software_optimization_catalog::RiskLevel::medium:
      return "medium";
    case domain::software_optimization_catalog::RiskLevel::high:
      return "high";
  }
  return "unknown";
}

[[nodiscard]] char const* optimization_automation_name(
    domain::software_optimization_catalog::AutomationSupport automation)
    noexcept {
  switch (automation) {
    case domain::software_optimization_catalog::AutomationSupport::controlled:
      return "controlled";
    case domain::software_optimization_catalog::AutomationSupport::manual_only:
      return "manual-only";
  }
  return "unknown";
}

[[nodiscard]] char const* optimization_exit_requirement_name(
    domain::software_optimization_catalog::ExitRequirement requirement)
    noexcept {
  switch (requirement) {
    case domain::software_optimization_catalog::ExitRequirement::none:
      return "none";
    case domain::software_optimization_catalog::ExitRequirement::graceful_exit:
      return "graceful-exit";
  }
  return "unknown";
}

[[nodiscard]] char const* optimization_restart_requirement_name(
    domain::software_optimization_catalog::RestartRequirement requirement)
    noexcept {
  switch (requirement) {
    case domain::software_optimization_catalog::RestartRequirement::none:
      return "none";
    case domain::software_optimization_catalog::RestartRequirement::explorer:
      return "explorer";
    case domain::software_optimization_catalog::RestartRequirement::windows:
      return "windows";
  }
  return "unknown";
}

void append_installation_plan_facts(
    std::vector<HistoryFactProjection>& target,
    domain::installation_batch::FrozenBatchPlan const& plan) {
  append_fact(target, "batch.correlation_id", plan.correlation_id);
  append_fact(target, "batch.frozen_at_utc_ms",
              std::to_string(plan.frozen_at_milliseconds));
  append_fact(target, "catalog.revision", std::to_string(plan.catalog.revision));
  append_fact(target, "catalog.schema_version",
              std::to_string(plan.catalog.schema_version));
  append_fact(target, "catalog.release_state",
              catalog_release_state_name(plan.catalog.release_state));
  append_fact(target, "catalog.identity",
              plan.catalog.local_trial ? "local-trial" : "released");
  if (plan.catalog.content_identity.empty()) {
    append_unavailable_fact(target, "catalog.content_identity",
                            "the frozen catalog did not retain a content identity");
  } else {
    append_fact(target, "catalog.content_identity", plan.catalog.content_identity);
  }
  if (plan.catalog.application_id.empty()) {
    append_unavailable_fact(target, "catalog.application_id",
                            "the frozen catalog did not retain an application association");
  } else {
    append_fact(target, "catalog.application_id", plan.catalog.application_id);
  }
  if (plan.retry_of_batch_id.has_value()) {
    append_fact(target, "batch.retry_of", *plan.retry_of_batch_id);
  }
}

void append_installation_item_timeline(
    HistoryEntryProjection& target,
    domain::installation_batch::FrozenBatchPlan const& plan,
    domain::installation_batch::InstallationItemProgress const& progress) {
  HistoryTimelineProjection item{
      .kind = HistoryTimelineKind::snapshot,
      .state = domain::installation_batch::to_string(progress.state),
      .detail = progress.detail,
  };
  append_fact(item.facts, "item.id", progress.item_id);
  append_fact(item.facts, "item.attempt", std::to_string(progress.attempt));
  append_fact(item.facts, "item.launch_requested",
              progress.launch_requested ? "true" : "false");
  append_fact(item.facts, "item.installer_started",
              progress.installer_started ? "true" : "false");
  append_fact(item.facts, "item.force_termination_confirmation_requested",
              progress.force_termination_confirmation_requested ? "true" : "false");
  append_fact(item.facts, "item.force_termination_completed",
              progress.force_termination_completed ? "true" : "false");
  append_fact(item.facts, "item.post_install_completed",
              progress.post_install_completed ? "true" : "false");
  auto const found = std::ranges::find_if(
      plan.items, [&](auto const& candidate) {
        return candidate.item_id == progress.item_id;
      });
  if (found == plan.items.end()) {
    append_unavailable_fact(item.facts, "item.frozen_definition",
                            "the durable item progress has no matching frozen item");
  } else {
    append_fact(item.facts, "item.resource_kind",
                domain::installation_batch::to_string(found->resource_kind));
    append_fact(item.facts, "source.software_id", found->source.software_id);
    append_fact(item.facts, "source.network_required",
                found->source.network_required ? "true" : "false");
    append_fact(item.facts, "source.resolved_at_utc_ms",
                std::to_string(found->source.resolved_at_milliseconds));
    if (found->source.version.empty()) {
      append_unavailable_fact(item.facts, "source.version",
                              "the frozen source snapshot had no version");
    } else {
      append_fact(item.facts, "source.version", found->source.version);
    }
    append_fact(item.facts, "package.kind",
                domain::software_selection::to_string(
                    found->selected_package.package_type));
    append_fact(item.facts, "package.complete",
                found->selected_package.complete_package ? "true" : "false");
    append_fact(item.facts, "package.network_required",
                found->selected_package.network_required ? "true" : "false");
  }
  target.timeline.push_back(std::move(item));
}

void append_installation_history(
    std::vector<HistoryEntryProjection>& target,
    installation_batch::InstallationBatchService& service,
    ExecutionLogSnapshot const& log) {
  auto const snapshot = service.snapshot();
  for (auto const& record : snapshot.history) {
    HistoryEntryProjection entry{
        .kind = HistoryEntryKind::installation_batch,
        .stable_id = record.plan.batch_id,
        .state = domain::installation_batch::to_string(record.final_state),
        .detail = record.reason,
        .retry = record.plan.retry_of_batch_id.has_value(),
    };
    append_installation_plan_facts(entry.facts, record.plan);
    for (auto const& progress : record.items) {
      append_installation_item_timeline(entry, record.plan, progress);
    }
    append_correlation_timeline(entry, log, record.plan.correlation_id);
    canonicalize_history_entry(entry);
    target.push_back(std::move(entry));
  }
  if (snapshot.active.has_value()) {
    auto const& active = *snapshot.active;
    HistoryEntryProjection entry{
        .kind = HistoryEntryKind::installation_batch,
        .stable_id = active.plan.batch_id,
        .state = domain::installation_batch::to_string(active.state),
        .detail = active.last_transition.item_id,
        .retry = active.plan.retry_of_batch_id.has_value(),
    };
    append_installation_plan_facts(entry.facts, active.plan);
    append_fact(entry.facts, "batch.generation",
                std::to_string(active.generation));
    append_fact(entry.facts, "batch.close_requested",
                active.close_requested ? "true" : "false");
    append_fact(entry.facts, "batch.stop_requested",
                active.stop_requested ? "true" : "false");
    if (active.last_transition.valid()) {
      append_fact(entry.facts, "last_durable_transition.generation",
                  std::to_string(active.last_transition.generation));
      append_fact(entry.facts, "last_durable_transition.item_id",
                  active.last_transition.item_id);
      append_fact(entry.facts, "last_durable_transition.item_state",
                  domain::installation_batch::to_string(
                      active.last_transition.item_state));
      append_fact(entry.facts, "last_durable_transition.outcome",
                  installation_durable_outcome_name(
                      active.last_transition.outcome));
      append_fact(entry.facts, "last_durable_transition.coverage_gap",
                  active.last_transition.coverage_gap ? "true" : "false");
    } else {
      constexpr std::string_view reason{
          "the active installation record has no valid durable transition"};
      append_unavailable_fact(entry.facts,
                              "last_durable_transition.generation",
                              std::string{reason});
      append_unavailable_fact(entry.facts, "last_durable_transition.item_id",
                              std::string{reason});
      append_unavailable_fact(entry.facts,
                              "last_durable_transition.item_state",
                              std::string{reason});
      append_unavailable_fact(entry.facts, "last_durable_transition.outcome",
                              std::string{reason});
      append_unavailable_fact(entry.facts,
                              "last_durable_transition.coverage_gap",
                              std::string{reason});
    }
    for (auto const& progress : active.items) {
      append_installation_item_timeline(entry, active.plan, progress);
    }
    append_correlation_timeline(entry, log, active.plan.correlation_id);
    canonicalize_history_entry(entry);
    target.push_back(std::move(entry));
  }
}

void append_optimization_plan_facts(
    std::vector<HistoryFactProjection>& target,
    domain::software_optimization_batch::FrozenOptimizationBatchPlan const&
        plan) {
  append_fact(target, "batch.correlation_id", plan.correlation_id);
  append_fact(target, "batch.frozen_at_utc_ms",
              std::to_string(plan.frozen_at_milliseconds));
  append_fact(target, "catalog.revision", std::to_string(plan.catalog_revision));
  append_fact(target, "emergency_notice.revision",
              std::to_string(plan.emergency_notice_revision));
  if (plan.retry_of_batch_id.has_value()) {
    append_fact(target, "batch.retry_of", *plan.retry_of_batch_id);
  }
  for (auto const& frozen_scheme : plan.schemes) {
    auto const prefix = "scheme." + frozen_scheme.scheme.id.value + ".";
    append_fact(target, prefix + "target_id", frozen_scheme.target.id.value);
    append_fact(target, prefix + "target_identity_anchor",
                frozen_scheme.target.identity_anchor.value);
    append_fact(target, prefix + "detected_version",
                frozen_scheme.detected_version);
    append_fact(target, prefix + "risk",
                optimization_risk_name(frozen_scheme.scheme.risk));
    append_fact(target, prefix + "risk_confirmation_id",
                frozen_scheme.risk_confirmation_id);
    append_fact(target, prefix + "automation",
                optimization_automation_name(frozen_scheme.scheme.automation));
    append_fact(target, prefix + "exit_requirement",
                optimization_exit_requirement_name(
                    frozen_scheme.scheme.exit_requirement));
    append_fact(target, prefix + "restart_requirement",
                optimization_restart_requirement_name(
                    frozen_scheme.scheme.restart_requirement));
    append_fact(target, prefix + "forced_version_execution",
                frozen_scheme.forced_version_execution ? "true" : "false");
    append_fact(target, prefix + "force_version_confirmed",
                frozen_scheme.force_version_confirmed ? "true" : "false");
    if (frozen_scheme.force_risk_version.empty()) {
      append_unavailable_fact(
          target, prefix + "force_risk_version",
          "the frozen scheme did not require a forced-version confirmation");
    } else {
      append_fact(target, prefix + "force_risk_version",
                  frozen_scheme.force_risk_version);
    }
    if (frozen_scheme.force_version_confirmation_id.has_value()) {
      append_fact(target, prefix + "force_version_confirmation_id",
                  *frozen_scheme.force_version_confirmation_id);
    } else {
      append_unavailable_fact(
          target, prefix + "force_version_confirmation_id",
          "the frozen scheme did not require a forced-version confirmation");
    }
    append_fact(target, prefix + "withdrawal_notice_revision",
                std::to_string(plan.emergency_notice_revision));
    if (frozen_scheme.scheme.manual_emergency_explanation.empty()) {
      append_unavailable_fact(
          target, prefix + "withdrawal_explanation",
          "the frozen scheme did not retain a manual withdrawal explanation");
    } else {
      append_fact(target, prefix + "withdrawal_explanation",
                  frozen_scheme.scheme.manual_emergency_explanation);
    }
    for (auto const& frozen_option : frozen_scheme.selected_options) {
      auto const option_prefix =
          prefix + "option." + frozen_option.option.id.value + ".";
      append_fact(target, option_prefix + "selected", "true");
      append_fact(target, option_prefix + "automation",
                  optimization_automation_name(frozen_option.option.automation));
      append_fact(target, option_prefix + "impact", frozen_option.option.impact);
      if (frozen_option.selected_value.has_value()) {
        append_fact(target, option_prefix + "selected_value_kind",
                    "explicit-value");
        append_fact(target, option_prefix + "selected_value",
                    *frozen_option.selected_value);
      } else {
        append_fact(target, option_prefix + "selected_value_kind",
                    "no-value-parameter");
      }
    }
  }
}

void append_optimization_step_timeline(
    HistoryEntryProjection& target,
    domain::software_optimization_batch::OptimizationStepProgress const& step) {
  HistoryTimelineProjection timeline{
      .kind = HistoryTimelineKind::snapshot,
      .state = domain::software_optimization_batch::to_string(step.state),
      .detail = step.detail,
  };
  append_fact(timeline.facts, "scheme.id", step.scheme_id);
  append_fact(timeline.facts, "option.id", step.option_id);
  append_fact(timeline.facts, "step.attempt", std::to_string(step.attempt));
  append_fact(timeline.facts, "step.execution_started",
              step.execution_started ? "true" : "false");
  append_fact(timeline.facts, "step.target_exit_confirmed",
              step.target_exit_confirmed ? "true" : "false");
  append_fact(timeline.facts, "step.force_close_confirmation_requested",
              step.force_close_confirmation_requested ? "true" : "false");
  append_fact(timeline.facts, "step.force_close_completed",
              step.force_close_completed ? "true" : "false");
  append_fact(timeline.facts, "step.emergency_notice_revision",
              std::to_string(step.emergency_notice_revision));
  target.timeline.push_back(std::move(timeline));
}

void append_optimization_history(
    std::vector<HistoryEntryProjection>& target,
    software_optimization_batch::SoftwareOptimizationBatchService& service,
    ExecutionLogSnapshot const& log) {
  auto const snapshot = service.snapshot();
  for (auto const& record : snapshot.history) {
    HistoryEntryProjection entry{
        .kind = HistoryEntryKind::software_optimization_batch,
        .stable_id = record.plan.batch_id,
        .state =
            domain::software_optimization_batch::to_string(record.final_state),
        .detail = record.reason,
        .retry = record.plan.retry_of_batch_id.has_value(),
    };
    append_optimization_plan_facts(entry.facts, record.plan);
    for (auto const& step : record.steps) {
      append_optimization_step_timeline(entry, step);
    }
    append_correlation_timeline(entry, log, record.plan.correlation_id);
    canonicalize_history_entry(entry);
    target.push_back(std::move(entry));
  }
  if (snapshot.active.has_value()) {
    auto const& active = *snapshot.active;
    HistoryEntryProjection entry{
        .kind = HistoryEntryKind::software_optimization_batch,
        .stable_id = active.plan.batch_id,
        .state = domain::software_optimization_batch::to_string(active.state),
        .detail = active.last_transition.option_id,
        .retry = active.plan.retry_of_batch_id.has_value(),
    };
    append_optimization_plan_facts(entry.facts, active.plan);
    append_fact(entry.facts, "batch.generation",
                std::to_string(active.generation));
    append_fact(entry.facts, "batch.close_requested",
                active.close_requested ? "true" : "false");
    append_fact(entry.facts, "batch.stop_requested",
                active.stop_requested ? "true" : "false");
    if (active.last_transition.valid()) {
      append_fact(entry.facts, "last_durable_transition.generation",
                  std::to_string(active.last_transition.generation));
      append_fact(entry.facts, "last_durable_transition.scheme_id",
                  active.last_transition.scheme_id);
      append_fact(entry.facts, "last_durable_transition.option_id",
                  active.last_transition.option_id);
      append_fact(entry.facts, "last_durable_transition.step_state",
                  domain::software_optimization_batch::to_string(
                      active.last_transition.step_state));
      append_fact(entry.facts, "last_durable_transition.outcome",
                  optimization_durable_outcome_name(
                      active.last_transition.outcome));
      append_fact(entry.facts, "last_durable_transition.coverage_gap",
                  active.last_transition.coverage_gap ? "true" : "false");
    } else {
      constexpr std::string_view reason{
          "the active optimization record has no valid durable transition"};
      append_unavailable_fact(entry.facts, "last_durable_transition.generation",
                              std::string{reason});
      append_unavailable_fact(entry.facts, "last_durable_transition.scheme_id",
                              std::string{reason});
      append_unavailable_fact(entry.facts, "last_durable_transition.option_id",
                              std::string{reason});
      append_unavailable_fact(entry.facts, "last_durable_transition.step_state",
                              std::string{reason});
      append_unavailable_fact(entry.facts, "last_durable_transition.outcome",
                              std::string{reason});
      append_unavailable_fact(entry.facts,
                              "last_durable_transition.coverage_gap",
                              std::string{reason});
    }
    for (auto const& step : active.steps) {
      append_optimization_step_timeline(entry, step);
    }
    append_correlation_timeline(entry, log, active.plan.correlation_id);
    canonicalize_history_entry(entry);
    target.push_back(std::move(entry));
  }
}

void append_sanitized_diagnostic_text(std::string& target,
                                      std::string_view value) {
  for (auto const character : value) {
    target.push_back(
        std::iscntrl(static_cast<unsigned char>(character)) ? ' ' : character);
  }
}

[[nodiscard]] std::string diagnostic_batch_plan_value(
    std::string_view kind, std::string_view batch_id,
    std::vector<HistoryFactProjection> const& facts) {
  std::string value{"kind="};
  append_sanitized_diagnostic_text(value, kind);
  value += ";batch.id=";
  append_sanitized_diagnostic_text(value, batch_id);
  for (auto const& fact : facts) {
    if (fact.disposition != HistoryFactDisposition::obtained) {
      continue;
    }
    value += ';';
    append_sanitized_diagnostic_text(value, fact.key);
    value += '=';
    append_sanitized_diagnostic_text(value, fact.value);
  }
  return value;
}

void append_diagnostic_batch_plan(
    DiagnosticContext& context,
    installation_batch::InstallationBatchService& installation_batches,
    software_optimization_batch::SoftwareOptimizationBatchService&
        optimization_batches) {
  struct Candidate final {
    std::int64_t frozen_at_milliseconds{};
    std::uint8_t priority{};
    std::string value;
  };
  std::optional<Candidate> selected;
  auto consider = [&](std::string_view kind, std::string_view batch_id,
                      std::int64_t frozen_at_milliseconds,
                      std::uint8_t priority,
                      std::vector<HistoryFactProjection> facts) {
    auto value = diagnostic_batch_plan_value(kind, batch_id, facts);
    if (!selected.has_value() ||
        frozen_at_milliseconds > selected->frozen_at_milliseconds ||
        (frozen_at_milliseconds == selected->frozen_at_milliseconds &&
         priority > selected->priority)) {
      selected = Candidate{.frozen_at_milliseconds = frozen_at_milliseconds,
                           .priority = priority,
                           .value = std::move(value)};
    }
  };
  auto consider_installation = [&](auto const& plan, std::uint8_t priority) {
    std::vector<HistoryFactProjection> facts;
    append_installation_plan_facts(facts, plan);
    consider("installation", plan.batch_id, plan.frozen_at_milliseconds,
             priority, std::move(facts));
  };
  auto consider_optimization = [&](auto const& plan, std::uint8_t priority) {
    std::vector<HistoryFactProjection> facts;
    append_optimization_plan_facts(facts, plan);
    consider("software-optimization", plan.batch_id,
             plan.frozen_at_milliseconds, priority, std::move(facts));
  };

  auto const installation_snapshot = installation_batches.snapshot();
  for (auto const& record : installation_snapshot.history) {
    consider_installation(record.plan, 0);
  }
  if (installation_snapshot.active.has_value()) {
    consider_installation(installation_snapshot.active->plan, 1);
  }

  auto const optimization_snapshot = optimization_batches.snapshot();
  for (auto const& record : optimization_snapshot.history) {
    consider_optimization(record.plan, 0);
  }
  if (optimization_snapshot.active.has_value()) {
    consider_optimization(optimization_snapshot.active->plan, 1);
  }

  if (!selected.has_value()) {
    context.batch_plan.unavailable_reason =
        "no active or retained frozen batch plan is available";
    return;
  }
  context.batch_plan = {.value = std::move(selected->value),
                        .disposition = DiagnosticValueDisposition::retain};
}

[[nodiscard]] char const* system_settings_operation_name(
    SystemSettingsOperationKind operation) noexcept {
  switch (operation) {
    case SystemSettingsOperationKind::apply:
      return "apply";
    case SystemSettingsOperationKind::restore:
      return "restore";
    case SystemSettingsOperationKind::windows11_default:
      return "windows11-default";
    case SystemSettingsOperationKind::restart_explorer:
      return "restart-explorer";
  }
  return "unknown";
}

[[nodiscard]] char const* system_settings_operation_status_name(
    SystemSettingsOperationStatus status) noexcept {
  switch (status) {
    case SystemSettingsOperationStatus::completed:
      return "completed";
    case SystemSettingsOperationStatus::failed:
      return "failed";
    case SystemSettingsOperationStatus::waiting_explorer_restart:
      return "waiting-explorer-restart";
    case SystemSettingsOperationStatus::restored:
      return "restored";
    case SystemSettingsOperationStatus::skipped:
      return "skipped";
    case SystemSettingsOperationStatus::not_applicable:
      return "not-applicable";
    case SystemSettingsOperationStatus::confirmation_required:
      return "confirmation-required";
    case SystemSettingsOperationStatus::blocked:
      return "blocked";
  }
  return "unknown";
}

[[nodiscard]] char const* explorer_restart_result_name(
    SystemSettingsExplorerRestartResult result) noexcept {
  switch (result) {
    case SystemSettingsExplorerRestartResult::not_required:
      return "not-required";
    case SystemSettingsExplorerRestartResult::deferred:
      return "deferred";
    case SystemSettingsExplorerRestartResult::succeeded:
      return "succeeded";
    case SystemSettingsExplorerRestartResult::failed:
      return "failed";
    case SystemSettingsExplorerRestartResult::verification_failed:
      return "verification-failed";
  }
  return "unknown";
}

[[nodiscard]] char const* restart_requirement_name(
    domain::settings_catalog::RestartRequirement requirement) noexcept {
  switch (requirement) {
    case domain::settings_catalog::RestartRequirement::none:
      return "none";
    case domain::settings_catalog::RestartRequirement::explorer:
      return "explorer";
    case domain::settings_catalog::RestartRequirement::windows:
      return "windows";
  }
  return "unknown";
}

[[nodiscard]] std::string windows_version_name(
    domain::settings_catalog::WindowsVersion const& version) {
  auto const generation =
      version.generation == domain::settings_catalog::WindowsGeneration::windows_11
          ? "windows-11"
          : "windows-10";
  return std::string{generation} + "-" +
         std::to_string(version.feature_update_year) + "H" +
         std::to_string(version.feature_update_half);
}

[[nodiscard]] std::string windows_range_name(
    domain::settings_catalog::WindowsVersionRange const& range) {
  auto const minimum = range.minimum.has_value()
                           ? windows_version_name(*range.minimum)
                           : std::string{"unbounded"};
  auto const maximum = range.maximum.has_value()
                           ? windows_version_name(*range.maximum)
                           : std::string{"unbounded"};
  return minimum + ".." + maximum;
}

[[nodiscard]] char const* system_setting_value_name(
    WindowsSystemSettingValue const& value) noexcept {
  if (auto const* mode = std::get_if<ClassicContextMenuMode>(&value)) {
    return *mode == ClassicContextMenuMode::classic ? "classic" : "windows11";
  }
  if (auto const* mode = std::get_if<ExplorerPresentationMode>(&value)) {
    return *mode == ExplorerPresentationMode::windows10 ? "windows10"
                                                         : "windows11";
  }
  return "unknown";
}

[[nodiscard]] std::string unavailable_operation_fact_reason(
    std::string const& reason, std::string_view fallback) {
  return reason.empty() ? std::string{fallback} : reason;
}

void append_optional_system_setting_value(
    std::vector<HistoryFactProjection>& target, std::string key,
    std::optional<WindowsSystemSettingValue> const& value,
    std::string_view value_name) {
  if (value.has_value()) {
    append_fact(target, std::move(key), system_setting_value_name(*value));
    return;
  }
  append_unavailable_fact(
      target, std::move(key),
      "NOT_OBTAINED: immutable operation did not capture the " +
          std::string{value_name} + " setting value");
}

void append_system_settings_history(
    std::vector<HistoryEntryProjection>& target,
    SystemSettingsApplyService& service) {
  auto const history = service.operation_history();
  for (auto const& fact : history.facts) {
    HistoryEntryProjection entry{
        .kind = HistoryEntryKind::system_setting_apply,
        .stable_id = "system-settings-operation/" +
                     std::to_string(fact.fact_id),
        .state = system_settings_operation_status_name(fact.status),
        .detail = fact.reason,
    };
    append_fact(entry.facts, "settings.operation_fact_id",
                std::to_string(fact.fact_id));
    append_fact(entry.facts, "settings.operation",
                system_settings_operation_name(fact.operation));
    append_fact(entry.facts, "settings.operation_status", entry.state);
    if (fact.reason.empty()) {
      append_unavailable_fact(
          entry.facts, "settings.operation_reason",
          "NOT_OBTAINED: immutable operation did not retain an operation reason");
    } else {
      append_fact(entry.facts, "settings.operation_reason", fact.reason);
    }
    append_fact(entry.facts, "settings.selection_source",
                fact.selected_plan_id.has_value() ? "recommended-overall"
                                                   : "individual");
    if (fact.selected_plan_id.has_value()) {
      append_fact(entry.facts, "settings.selected_plan_id",
                  fact.selected_plan_id->value);
      if (fact.selected_plan_name.empty()) {
        append_unavailable_fact(
            entry.facts, "settings.selected_plan_name",
            "NOT_OBTAINED: immutable operation did not capture the selected plan name");
      } else {
        append_fact(entry.facts, "settings.selected_plan_name",
                    fact.selected_plan_name);
      }
    }
    if (fact.catalog_availability == SystemSettingsFactAvailability::obtained) {
      append_fact(entry.facts, "settings.catalog_identity", fact.catalog_identity);
      append_fact(entry.facts, "settings.catalog_revision",
                  std::to_string(fact.catalog_revision));
    } else {
      auto const reason = unavailable_operation_fact_reason(
          fact.catalog_reason,
          "NOT_OBTAINED: immutable operation did not capture catalog identity and revision");
      append_unavailable_fact(entry.facts, "settings.catalog_identity", reason);
      append_unavailable_fact(entry.facts, "settings.catalog_revision", reason);
    }
    if (fact.windows_environment.availability ==
            SystemSettingsFactAvailability::obtained &&
        !fact.windows_environment.display_version.empty()) {
      append_fact(entry.facts, "settings.windows_display_version",
                  fact.windows_environment.display_version);
      append_fact(entry.facts, "settings.observed_windows_version",
                  fact.windows_environment.display_version);
    } else {
      auto const reason = unavailable_operation_fact_reason(
          fact.windows_environment.reason,
          "NOT_OBTAINED: immutable operation did not capture the Windows display version");
      append_unavailable_fact(entry.facts, "settings.windows_display_version",
                              reason);
      append_unavailable_fact(entry.facts, "settings.observed_windows_version",
                              reason);
    }
    if (fact.windows_environment.availability ==
            SystemSettingsFactAvailability::obtained &&
        fact.windows_environment.internal_build != 0) {
      auto const build = std::to_string(fact.windows_environment.internal_build);
      append_fact(entry.facts, "settings.windows_internal_build", build);
      append_fact(entry.facts, "settings.observed_windows_build", build);
    } else {
      auto const reason = unavailable_operation_fact_reason(
          fact.windows_environment.reason,
          "NOT_OBTAINED: immutable operation did not capture the Windows internal build");
      append_unavailable_fact(entry.facts, "settings.windows_internal_build",
                              reason);
      append_unavailable_fact(entry.facts, "settings.observed_windows_build",
                              reason);
    }
    append_fact(entry.facts, "settings.explorer_restart_requested",
                fact.explorer_restart_requested ? "true" : "false");
    append_fact(entry.facts, "settings.explorer_restart_result",
                explorer_restart_result_name(fact.explorer_restart_result));
    append_fact(entry.facts, "settings.windows_restart_barrier",
                fact.windows_restart_barrier ? "true" : "false");
    append_fact(entry.facts, "settings.setting_count",
                std::to_string(fact.settings.size()));

    for (auto const& setting : fact.settings) {
      HistoryTimelineProjection timeline{
          .kind = HistoryTimelineKind::snapshot,
          .state = system_settings_operation_status_name(setting.status),
          .detail = setting.display_name,
      };
      if (setting.setting_id.value.empty()) {
        append_unavailable_fact(
            timeline.facts, "settings.setting_id",
            "NOT_OBTAINED: immutable operation did not capture a setting identifier");
      } else {
        append_fact(timeline.facts, "settings.setting_id", setting.setting_id.value);
      }
      if (setting.display_name.empty()) {
        append_unavailable_fact(
            timeline.facts, "settings.display_name",
            "NOT_OBTAINED: immutable operation did not capture a setting display name");
      } else {
        append_fact(timeline.facts, "settings.display_name", setting.display_name);
      }
      if (setting.controlled_identity.empty() ||
          setting.controlled_identity == "NOT_OBTAINED") {
        append_unavailable_fact(
            timeline.facts, "settings.controlled_identity",
            "NOT_OBTAINED: immutable operation did not capture a controlled setting identity");
      } else {
        append_fact(timeline.facts, "settings.controlled_identity",
                    setting.controlled_identity);
      }
      if (setting.declared_range_availability ==
          SystemSettingsFactAvailability::obtained) {
        append_fact(timeline.facts, "settings.declared_windows_range",
                    windows_range_name(setting.declared_windows_range));
      } else {
        append_unavailable_fact(
            timeline.facts, "settings.declared_windows_range",
            unavailable_operation_fact_reason(
                setting.declared_range_reason,
                "NOT_OBTAINED: immutable operation did not capture the declared Windows range"));
      }
      append_fact(timeline.facts, "settings.catalog_revision",
                  std::to_string(setting.catalog_revision));
      append_fact(timeline.facts, "settings.restart_requirement",
                  restart_requirement_name(setting.restart_requirement));
      append_fact(timeline.facts, "settings.force_attempt_confirmed",
                  setting.force_attempt_confirmed ? "true" : "false");
      append_fact(timeline.facts, "settings.status", timeline.state);
      append_optional_system_setting_value(timeline.facts, "settings.original_value",
                                           setting.original_value, "original");
      append_optional_system_setting_value(timeline.facts, "settings.target_value",
                                           setting.target_value, "target");
      if (fact.operation == SystemSettingsOperationKind::restore) {
        append_fact(timeline.facts, "settings.undo", "true");
        append_optional_system_setting_value(timeline.facts,
                                             "settings.restore_value",
                                             setting.target_value, "restore");
      }
      if (setting.recovery_record_id.has_value()) {
        append_fact(timeline.facts, "settings.recovery_record_id",
                    std::to_string(*setting.recovery_record_id));
      }
      if (setting.reason.empty()) {
        append_unavailable_fact(
            timeline.facts, "settings.reason",
            "NOT_OBTAINED: immutable operation did not retain a setting result reason");
      } else {
        append_fact(timeline.facts, "settings.reason", setting.reason);
      }
      entry.timeline.push_back(std::move(timeline));
    }
    for (auto const& stage : fact.timeline) {
      HistoryTimelineProjection timeline{
          .kind = HistoryTimelineKind::state_transition,
          .state = system_settings_operation_status_name(stage.status),
          .detail = stage.stage,
      };
      append_fact(timeline.facts, "settings.operation_timeline.ordinal",
                  std::to_string(stage.ordinal));
      append_fact(timeline.facts, "settings.operation_timeline.stage",
                  stage.stage);
      append_fact(timeline.facts, "settings.operation_timeline.status",
                  timeline.state);
      if (stage.reason.empty()) {
        append_unavailable_fact(
            timeline.facts, "settings.operation_timeline.reason",
            "NOT_OBTAINED: immutable operation timeline did not retain a reason");
      } else {
        append_fact(timeline.facts, "settings.operation_timeline.reason",
                    stage.reason);
      }
      entry.timeline.push_back(std::move(timeline));
    }
    target.push_back(std::move(entry));
  }
}

void append_external_handoff_history(
    std::vector<HistoryEntryProjection>& target,
    software_selection::SoftwareSelectionLifecycle& service,
    ExecutionLogSnapshot const& log) {
  auto const snapshot = service.snapshot();
  for (auto const& handoff : snapshot.handoffs) {
    auto const timeline_is_valid = handoff.timeline.valid();
    HistoryEntryProjection entry{
        .kind = HistoryEntryKind::external_install_handoff,
        .stable_id = handoff.software_id,
        .state = timeline_is_valid
                     ? domain::software_selection::to_string(handoff.status)
                     : "not-obtained",
        .detail = timeline_is_valid
                      ? domain::software_selection::to_string(
                            handoff.timeline.facts.back().kind)
                      : "external-handoff-timeline-unavailable",
    };
    append_fact(entry.facts, "handoff.software_id", handoff.software_id);
    if (!timeline_is_valid) {
      append_unavailable_fact(
          entry.facts, "handoff.timeline",
          "the durable external handoff record has no valid immutable timeline");
    } else {
      append_fact(entry.facts, "handoff.timeline_fact_count",
                  std::to_string(handoff.timeline.facts.size()));
      append_fact(entry.facts, "handoff.currently_incomplete",
                  handoff.status == domain::software_selection::
                                        ExternalHandoffStatus::
                                            waiting_for_external_install ||
                          handoff.status == domain::software_selection::
                                                ExternalHandoffStatus::skipped
                      ? "true"
                      : "false");
      for (auto const& fact : handoff.timeline.facts) {
        HistoryTimelineProjection timeline{
            .kind = HistoryTimelineKind::external_handoff,
            .state = domain::software_selection::to_string(fact.status),
            .detail = domain::software_selection::to_string(fact.kind),
        };
        append_fact(timeline.facts, "handoff.kind",
                    domain::software_selection::to_string(fact.kind));
        append_fact(timeline.facts, "handoff.status",
                    domain::software_selection::to_string(fact.status));
        append_fact(timeline.facts, "handoff.correlation_id",
                    fact.correlation_id);
        append_fact(timeline.facts, "handoff.declared_address_recorded",
                    fact.declared_address.empty() ? "false" : "true");
        append_unavailable_fact(
            timeline.facts, "handoff.declared_address",
            "the address is available only through the explicit handoff command and redacted event trail");
        append_unavailable_fact(
            timeline.facts, "handoff.detail",
            "raw handoff detail is available only through the centrally redacted event trail");
        if (fact.timestamp_availability ==
            domain::software_selection::ExternalHandoffFactAvailability::
                obtained) {
          timeline.recorded_at_milliseconds = fact.occurred_at_milliseconds;
          append_fact(timeline.facts, "handoff.occurred_at_utc_ms",
                      std::to_string(fact.occurred_at_milliseconds));
        } else {
          append_unavailable_fact(
              timeline.facts, "handoff.occurred_at_utc_ms",
              domain::software_selection::to_string(
                  fact.timestamp_not_obtained_reason));
        }
        if (fact.resolved_source.availability ==
            domain::software_selection::ExternalHandoffFactAvailability::
                obtained) {
          append_fact(timeline.facts, "source.resolved_at_utc_ms",
                      std::to_string(
                          fact.resolved_source.resolved_at_milliseconds));
          append_fact(timeline.facts, "source.version",
                      fact.resolved_source.resolved_version);
          append_fact(timeline.facts, "source.resolver_capability_version",
                      fact.resolved_source.resolver_capability_version);
          append_unavailable_fact(
              timeline.facts, "source.resolved_address",
              "the address is available only through the centrally redacted event trail");
        } else {
          append_unavailable_fact(
              timeline.facts, "source.snapshot",
              domain::software_selection::to_string(
                  fact.resolved_source.not_obtained_reason));
        }
        entry.timeline.push_back(std::move(timeline));
      }
    }
    append_id_timeline(entry, log, handoff.software_id);
    canonicalize_history_entry(entry);
    target.push_back(std::move(entry));
  }
}

[[nodiscard]] char const* restart_operation_name(
    restart_resume::RestartResumeOperation operation) noexcept {
  switch (operation) {
    case restart_resume::RestartResumeOperation::system_settings:
      return "system-settings";
    case restart_resume::RestartResumeOperation::installation_batch:
      return "installation-batch";
    case restart_resume::RestartResumeOperation::software_optimization_batch:
      return "software-optimization-batch";
    case restart_resume::RestartResumeOperation::driver_acquisition:
      return "driver-acquisition";
  }
  return "unknown";
}

void append_restart_resume_history(
    std::vector<HistoryEntryProjection>& target,
    restart_resume::RestartResumeService const* service,
    ExecutionLogSnapshot const& log) {
  if (service == nullptr) {
    return;
  }
  auto const snapshot = service->snapshot();
  if (snapshot.state == restart_resume::RestartResumeState::idle &&
      !snapshot.checkpoint.has_value()) {
    return;
  }
  HistoryEntryProjection entry{
      .kind = HistoryEntryKind::restart_resume,
      .stable_id = snapshot.checkpoint.has_value()
                       ? snapshot.checkpoint->correlation_id
                       : std::string{"restart-resume"},
      .state = restart_resume::to_string(snapshot.state),
      .detail = snapshot.detail,
  };
  append_fact(entry.facts, "restart.writable",
              snapshot.writable ? "true" : "false");
  append_fact(entry.facts, "restart.login_resume_registered",
              snapshot.login_resume_registered ? "true" : "false");
  if (snapshot.checkpoint.has_value()) {
    append_fact(entry.facts, "restart.correlation_id",
                snapshot.checkpoint->correlation_id);
    for (auto const& participant : snapshot.checkpoint->participants) {
      HistoryTimelineProjection participant_timeline{
          .kind = HistoryTimelineKind::recovery,
          .state = entry.state,
          .detail = participant.operation_id,
      };
      append_fact(participant_timeline.facts, "restart.operation",
                  restart_operation_name(participant.operation));
      append_fact(participant_timeline.facts, "restart.operation_id",
                  participant.operation_id);
      entry.timeline.push_back(std::move(participant_timeline));
    }
    append_correlation_timeline(entry, log, snapshot.checkpoint->correlation_id);
  } else {
    append_unavailable_fact(entry.facts, "restart.checkpoint",
                            "restart state has no durable checkpoint");
  }
  canonicalize_history_entry(entry);
  target.push_back(std::move(entry));
}

[[nodiscard]] char const* update_state_name(UpdateState state) noexcept {
  switch (state) {
    case UpdateState::idle:
      return "idle";
    case UpdateState::latest_stable:
      return "latest-stable";
    case UpdateState::update_available:
      return "update-available";
    case UpdateState::stable_switch_available:
      return "stable-switch-available";
    case UpdateState::no_matching_stable_asset:
      return "no-matching-stable-asset";
    case UpdateState::awaiting_user_confirmation:
      return "awaiting-user-confirmation";
    case UpdateState::deferred_initialization_operation:
      return "deferred-initialization-operation";
    case UpdateState::update_unavailable:
      return "update-unavailable";
    case UpdateState::update_failed_restored:
      return "update-failed-restored";
    case UpdateState::candidate_pending_start_health:
      return "candidate-pending-start-health";
    case UpdateState::awaiting_start_recovery_choice:
      return "awaiting-start-recovery-choice";
    case UpdateState::previous_pending_start_health:
      return "previous-pending-start-health";
    case UpdateState::recovery_read_only:
      return "recovery-read-only";
  }
  return "unknown";
}

void append_update_history(std::vector<HistoryEntryProjection>& target,
                           ApplicationUpdateLifecycle& service) {
  auto const update = service.snapshot();
  if (!update.current.valid() && update.state == UpdateState::idle) {
    return;
  }
  HistoryEntryProjection entry{
      .kind = HistoryEntryKind::application_update,
      .stable_id = update.current.valid() ? update.current.version
                                           : std::string{"workbench-update"},
      .state = update_state_name(update.state),
      .detail = update.detail,
  };
  if (update.current.valid()) {
    append_fact(entry.facts, "update.current_version", update.current.version);
    append_fact(entry.facts, "update.current_architecture",
                architecture_name(update.current.architecture));
  } else {
    append_unavailable_fact(entry.facts, "update.current_build",
                            "update owner has no valid current build identity");
  }
  if (update.candidate.has_value()) {
    append_fact(entry.facts, "update.candidate_version",
                update.candidate->target.version);
  }
  if (update.health.has_value()) {
    append_fact(entry.facts, "update.health_started_at_utc_ms",
                std::to_string(
                    update.health->started_at.time_since_epoch().count()));
  }
  append_fact(entry.facts, "update.read_only",
              update.read_only ? "true" : "false");
  HistoryTimelineProjection timeline{
      .kind = HistoryTimelineKind::snapshot,
      .state = entry.state,
      .detail = entry.detail,
      .facts = entry.facts,
  };
  entry.timeline.push_back(std::move(timeline));
  canonicalize_history_entry(entry);
  target.push_back(std::move(entry));
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
  std::string result{text};
  std::ranges::transform(result, result.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return result;
}

[[nodiscard]] bool contains_query(std::string_view value,
                                  std::string const& query) {
  return query.empty() || lower_ascii(value).find(query) != std::string::npos;
}

[[nodiscard]] bool facts_match_query(
    std::vector<HistoryFactProjection> const& facts,
    std::string const& query) {
  return std::ranges::any_of(facts, [&](auto const& fact) {
    return contains_query(fact.key, query) || contains_query(fact.value, query) ||
           contains_query(fact.reason, query);
  });
}

[[nodiscard]] bool history_matches(HistoryEntryProjection const& entry,
                                   HistoryAndLogsFilter const& filter) {
  if (filter.history_kind.has_value() && entry.kind != *filter.history_kind) {
    return false;
  }
  if (filter.query.empty()) {
    return true;
  }
  auto const query = lower_ascii(filter.query);
  if (contains_query(entry.stable_id, query) || contains_query(entry.state, query) ||
      contains_query(entry.detail, query) || facts_match_query(entry.facts, query)) {
    return true;
  }
  return std::ranges::any_of(entry.timeline, [&](auto const& timeline) {
    return contains_query(timeline.state, query) ||
           contains_query(timeline.detail, query) ||
           facts_match_query(timeline.facts, query);
  });
}

[[nodiscard]] bool event_matches(ExecutionLogEventProjection const& event,
                                  HistoryAndLogsFilter const& filter) {
  if (filter.event_kind.has_value() && event.kind != *filter.event_kind) {
    return false;
  }
  if (filter.event_result.has_value() && event.result != *filter.event_result) {
    return false;
  }
  if (!filter.correlation_id.empty() &&
      event.correlation.value != filter.correlation_id) {
    return false;
  }
  if (filter.query.empty()) {
    return true;
  }
  auto const query = lower_ascii(filter.query);
  if (contains_query(event.correlation.value, query) ||
      contains_query(event.component, query) || contains_query(event.stage, query)) {
    return true;
  }
  if (event.error.has_value() &&
      (contains_query(event.error->source, query) ||
       contains_query(event.error->message, query))) {
    return true;
  }
  return std::ranges::any_of(event.fields, [&](auto const& field) {
    return contains_query(field.key, query) || contains_query(field.value, query);
  });
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
    software_selection::SoftwareSelectionLifecycle& software_selection,
    DebugLogPolicySnapshotSource const* debug_log_policy,
    restart_resume::RestartResumeService const* restart_resume)
    : clock_(clock),
      application_updates_(application_updates),
      platform_info_(platform_info),
      hardware_overview_(hardware_overview),
      software_catalog_(software_catalog),
      log_(log),
      installation_batches_(installation_batches),
      software_optimization_batches_(software_optimization_batches),
      system_settings_(system_settings),
      software_selection_(software_selection),
      debug_log_policy_(debug_log_policy),
      restart_resume_(restart_resume) {}

HistoryAndLogsSnapshot HistoryAndLogsService::refresh() {
  return refresh({});
}

HistoryAndLogsSnapshot HistoryAndLogsService::refresh(
    HistoryAndLogsFilter const& filter) {
  HistoryAndLogsSnapshot result;
  result.applied_filter = filter;
  result.log = log_.snapshot();
  append_installation_history(result.history, installation_batches_, result.log);
  append_optimization_history(result.history, software_optimization_batches_,
                              result.log);
  append_system_settings_history(result.history, system_settings_);
  append_external_handoff_history(result.history, software_selection_, result.log);
  append_restart_resume_history(result.history, restart_resume_, result.log);
  append_update_history(result.history, application_updates_);
  if (debug_log_policy_ != nullptr) {
    result.debug = debug_log_policy_->snapshot();
  } else {
    result.debug.not_obtained_reason =
        "debug log policy is not provided by the current composition root";
  }
  if (!result.log.available && !result.log.error.empty()) {
    result.detail = "execution-log-unavailable";
  }
  if (filter.history_kind.has_value() || !filter.query.empty()) {
    std::erase_if(result.history, [&](auto const& entry) {
      return !history_matches(entry, filter);
    });
  }
  if (filter.event_kind.has_value() || filter.event_result.has_value() ||
      !filter.correlation_id.empty() || !filter.query.empty()) {
    std::erase_if(result.log.events, [&](auto const& event) {
      return !event_matches(event, filter);
    });
  }
  return result;
}

HistoryAndLogsSnapshot HistoryAndLogsService::locate(
    std::string_view stable_id) {
  auto result = refresh();
  auto const requested = std::string{stable_id};
  result.applied_filter.query = requested;
  std::erase_if(result.history, [&](auto const& entry) {
    return entry.stable_id != requested;
  });
  std::optional<std::string> frozen_batch_correlation;
  for (auto const& entry : result.history) {
    if (entry.kind != HistoryEntryKind::installation_batch &&
        entry.kind != HistoryEntryKind::software_optimization_batch) {
      continue;
    }
    auto const correlation = std::ranges::find_if(
        entry.facts, [](HistoryFactProjection const& fact) {
          return fact.key == "batch.correlation_id" &&
                 fact.disposition == HistoryFactDisposition::obtained;
        });
    if (correlation != entry.facts.end()) {
      frozen_batch_correlation = correlation->value;
    }
    break;
  }
  std::erase_if(result.log.events, [&](auto const& event) {
    if (frozen_batch_correlation.has_value()) {
      return event.correlation.value != *frozen_batch_correlation;
    }
    return !event_mentions_id(event, requested);
  });
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
  if (hardware.observation.has_value() &&
      hardware.observation->has_confirmed_physical_hardware()) {
    auto const& observed = *hardware.observation;
    if (!observed.oem_model.empty()) {
      append_retained(context, "hardware_model", observed.oem_model);
    } else {
      append_missing(context, "hardware_model",
                     "hardware owner did not provide a non-unique model");
    }
    auto summary_for = [&observed](HardwareDeviceKind kind) {
      std::string summary;
      for (auto const& device : observed.devices) {
        if (!device.confirmed_physical() || device.kind != kind) {
          continue;
        }
        if (!summary.empty()) {
          summary.append("; ");
        }
        summary.append(device.name);
      }
      return summary;
    };
    auto const cpu = summary_for(HardwareDeviceKind::cpu);
    auto const gpu = summary_for(HardwareDeviceKind::gpu);
    auto const network = summary_for(HardwareDeviceKind::network_adapter);
    if (!cpu.empty()) {
      append_retained(context, "hardware_cpu", cpu);
    }
    if (!gpu.empty()) {
      append_retained(context, "hardware_gpu", gpu);
    }
    if (!network.empty()) {
      append_retained(context, "network_adapter_category", network);
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
    std::string frozen_directory_identity{"identity="};
    frozen_directory_identity += catalog_identity_name(current.identity);
    frozen_directory_identity += ";content_identity=";
    frozen_directory_identity += current.content_identity.empty()
                                     ? "not-provided"
                                     : current.content_identity;
    frozen_directory_identity += ";revision=" +
                                 std::to_string(current.revision);
    frozen_directory_identity += ";origin=";
    frozen_directory_identity += catalog_origin_name(current.origin);
    context.frozen_directory_identity = {
        .value = std::move(frozen_directory_identity),
        .disposition = DiagnosticValueDisposition::retain,
    };
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
      context.directory_application_association = {
          .value = current.application_id,
          .disposition = DiagnosticValueDisposition::retain,
      };
    } else {
      append_missing(context, "catalog_application_id",
                     "active catalog has no application association identifier");
      context.directory_application_association.unavailable_reason =
          "active catalog has no application association identifier";
    }
    append_retained(context, "catalog_release_issue_count",
                    std::to_string(current.release_issues.size()));
    append_retained(context, "catalog_release_gate",
                    current.release_issues.empty() ? "passed" : "failed");
  } else {
    auto const reason = catalog.error.empty()
                            ? std::string{"catalog owner has no active catalog"}
                            : catalog.error;
    context.frozen_directory_identity.unavailable_reason = reason;
    context.directory_application_association.unavailable_reason = reason;
    append_missing(context, "catalog_revision", reason);
    append_missing(context, "catalog_identity", reason);
    append_missing(context, "catalog_application_id", reason);
    append_missing(context, "catalog_release_gate", reason);
  }
  if (catalog.current_document.has_value() &&
      catalog.current_document->release_state.has_value()) {
    auto const release_state =
        catalog_release_state_name(*catalog.current_document->release_state);
    append_retained(context, "catalog_release_state", release_state);
    append_retained(context, "catalog_schema",
                    std::to_string(catalog.current_document->schema_version));
    std::string release_result{"release_state="};
    release_result += release_state;
    if (catalog.current.has_value()) {
      release_result += ";release_gate=";
      release_result += catalog.current->release_issues.empty() ? "passed"
                                                                 : "failed";
    }
    context.directory_release_result = {
        .value = std::move(release_result),
        .disposition = DiagnosticValueDisposition::retain,
    };
  } else {
    append_missing(context, "catalog_release_state",
                   "catalog owner did not provide an active document release state");
    append_missing(context, "catalog_schema",
                   "catalog owner did not provide an active document schema");
    if (catalog.current.has_value()) {
      context.directory_release_result = {
          .value = std::string{"release_gate="} +
                   (catalog.current->release_issues.empty() ? "passed"
                                                            : "failed"),
          .disposition = DiagnosticValueDisposition::retain,
      };
    } else {
      context.directory_release_result.unavailable_reason =
          "catalog owner did not provide an active document release state";
    }
  }
  if (catalog.current_catalog.has_value()) {
    append_retained(context, "catalog_runtime_load", "accepted");
    context.directory_load_result = {
        .value = "accepted",
        .disposition = DiagnosticValueDisposition::retain,
    };
  } else {
    append_missing(context, "catalog_runtime_load",
                   "catalog owner has no runtime load projection");
    context.directory_load_result.unavailable_reason =
        "catalog owner has no runtime load projection";
  }
  append_diagnostic_batch_plan(context, installation_batches_,
                               software_optimization_batches_);

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
    append_retained(
        context, "execution_log_capacity_state",
        snapshot.log.capacity_state == ExecutionLogCapacityState::available
            ? "available"
            : "space-exhausted");
    append_retained(context, "execution_log_noncritical_dropped_count",
                    std::to_string(snapshot.log.noncritical_dropped_count));
    if (snapshot.log.noncritical_dropped_count != 0) {
      append_missing(
          context, "execution_log_coverage",
          std::to_string(snapshot.log.noncritical_dropped_count) +
              " noncritical events were suppressed while storage capacity was exhausted");
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
  auto const debug_context = make_debug_log_policy_context(snapshot.debug);
  if (!debug_context.facts_available) {
    context.debug_log_coverage.unavailable_reason =
        debug_context.not_obtained_reason.empty()
            ? "debug log policy owner did not provide a readable snapshot"
            : debug_context.not_obtained_reason;
    append_missing(context, "debug_log_policy",
                   context.debug_log_coverage.unavailable_reason);
  } else {
    context.debug_log_coverage = {
        .value = "mode=" + debug_context.debug_mode +
                 ";granularity=" + debug_context.granularity +
                 ";locating=" + debug_context.locating_semantics +
                 ";retention=" + debug_context.existing_log_retention,
        .disposition = DiagnosticValueDisposition::retain,
    };
    append_retained(context, "debug_log_filterable_fields",
                    std::to_string(debug_context.filterable_fields.size()));
    append_retained(context, "debug_log_coverage_count",
                    std::to_string(debug_context.coverage.size()));
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
    case HistoryEntryKind::system_setting_apply:
      return "system-setting-apply";
    case HistoryEntryKind::system_setting_recovery:
      return "system-setting-recovery";
    case HistoryEntryKind::external_install_handoff:
      return "external-install-handoff";
    case HistoryEntryKind::restart_resume:
      return "restart-resume";
    case HistoryEntryKind::application_update:
      return "application-update";
  }
  return "unknown";
}

char const* to_string(HistoryFactDisposition value) noexcept {
  switch (value) {
    case HistoryFactDisposition::obtained:
      return "obtained";
    case HistoryFactDisposition::not_obtained:
      return "not-obtained";
  }
  return "unknown";
}

char const* to_string(HistoryTimelineKind value) noexcept {
  switch (value) {
    case HistoryTimelineKind::snapshot:
      return "snapshot";
    case HistoryTimelineKind::user_command:
      return "user-command";
    case HistoryTimelineKind::state_transition:
      return "state-transition";
    case HistoryTimelineKind::adapter_result:
      return "adapter-result";
    case HistoryTimelineKind::recovery:
      return "recovery";
    case HistoryTimelineKind::external_handoff:
      return "external-handoff";
    case HistoryTimelineKind::coverage_gap:
      return "coverage-gap";
  }
  return "unknown";
}

}  // namespace azzs::application
