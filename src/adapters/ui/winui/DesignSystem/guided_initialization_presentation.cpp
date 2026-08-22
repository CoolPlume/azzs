#include "guided_initialization_presentation.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::ui::presentation {
namespace {

namespace guided = application::guided_initialization;

[[nodiscard]] CommandProjection command(std::string id, std::string label,
                                         CommandRole role, IntentKind kind,
                                         std::string target, bool enabled = true,
                                         bool default_focus = false,
                                         std::string disabled_reason = {}) {
  auto const command_id = id;
  return {.id = std::move(id),
          .label = std::move(label),
          .role = role,
          .enabled = enabled,
          .default_focus = default_focus,
          .disabled_reason = std::move(disabled_reason),
          .intent = {.kind = kind,
                     .target_id = std::move(target),
                     .command_id = command_id}};
}

[[nodiscard]] PresentationState presentation_state(guided::StageState state) {
  switch (state) {
    case guided::StageState::active:
      return PresentationState::in_progress;
    case guided::StageState::failed:
      return PresentationState::failed;
    case guided::StageState::result_confirmation_pending:
      return PresentationState::pending_confirmation;
    case guided::StageState::waiting_explorer_restart:
    case guided::StageState::waiting_for_restart:
      return PresentationState::waiting_for_restart;
    case guided::StageState::emergency_withdrawn:
      return PresentationState::withdrawn;
    case guided::StageState::external_handoff:
      return PresentationState::source_handoff;
    case guided::StageState::not_executed:
      return PresentationState::disabled;
    case guided::StageState::completed:
    case guided::StageState::skipped:
    case guided::StageState::no_applicable_items:
    case guided::StageState::partial:
      return PresentationState::completed;
    case guided::StageState::pending:
      return PresentationState::neutral;
  }
  return PresentationState::neutral;
}

[[nodiscard]] char const* stage_component_id(guided::Stage stage) noexcept {
  switch (stage) {
    case guided::Stage::drivers:
      return "guided.stage.drivers";
    case guided::Stage::system_optimization:
      return "guided.stage.system-optimization";
    case guided::Stage::software_installation:
      return "guided.stage.software-installation";
    case guided::Stage::software_optimization:
      return "guided.stage.software-optimization";
  }
  return "guided.stage.unknown";
}

[[nodiscard]] std::string stage_title(
    guided::Stage stage,
    GuidedInitializationPresentationText const& text) {
  switch (stage) {
    case guided::Stage::drivers:
      return text.drivers_stage_title;
    case guided::Stage::system_optimization:
      return text.system_optimization_stage_title;
    case guided::Stage::software_installation:
      return text.software_installation_stage_title;
    case guided::Stage::software_optimization:
      return text.software_optimization_stage_title;
  }
  return text.unknown_stage_title;
}

[[nodiscard]] std::string raw_detail(
    std::string const& detail,
    GuidedInitializationPresentationText const& text) {
  return detail.empty() ? text.stage_empty_body
                        : text.raw_detail_prefix + detail;
}

[[nodiscard]] std::string localized_detail(
    guided::StageRecord const& record,
    GuidedInitializationPresentationText const& text) {
  // These terminal states carry their own user-facing meaning even when the
  // owning service includes a free-form diagnostic detail.
  switch (record.state) {
    case guided::StageState::partial:
      return text.stage_partial_body;
    case guided::StageState::emergency_withdrawn:
      return text.stage_withdrawn_body;
    case guided::StageState::skipped:
      return text.stage_skipped_body;
    case guided::StageState::no_applicable_items:
      return text.stage_no_applicable_body;
    case guided::StageState::not_executed:
      return text.stage_not_executed_body;
    default:
      break;
  }

  auto const detail = std::string_view{record.detail};
  if (detail == "driver stage marked complete by the user" ||
      detail == "installation batch completed" ||
      detail == "software optimization batch completed" ||
      detail == "system settings completed" ||
      detail == "system settings are effective" ||
      detail == "system optimization was verified after restart" ||
      detail == "settings already match the recommended baseline" ||
      detail ==
          "restart verification is complete; explicit continuation is required") {
    return text.stage_completed_body;
  }
  if (detail == "stage temporarily skipped by the user") {
    return text.stage_skipped_body;
  }
  if (detail == "no system settings apply here" ||
      detail == "no software is available in the current catalog" ||
      detail == "no installable software remains" ||
      detail == "no software optimization is currently available" ||
      detail == "no executable software optimization is available") {
    return text.stage_no_applicable_body;
  }
  if (detail == "system settings are applying" ||
      detail == "installation batch is active" ||
      detail == "software optimization batch is active" ||
      detail == "driver handoff is in progress") {
    return text.stage_active_body;
  }
  if (detail == "waiting for the shared Windows restart barrier" ||
      detail == "installation batch is waiting for restart" ||
      detail == "software optimization is waiting for restart" ||
      detail == "driver handoff is waiting for restart") {
    return text.stage_waiting_restart_body;
  }
  if (detail == "system settings are waiting for Explorer restart") {
    return text.stage_waiting_explorer_body;
  }
  if (detail == "restart barrier is available only for read-only recovery" ||
      detail == "Windows restart is required before verification") {
    return text.stage_waiting_restart_body;
  }
  if (detail == "waiting for an external installation handoff to continue" ||
      detail == "software installation needs an external handoff" ||
      detail == "external installation remains an explicitly recognized fact" ||
      detail == "source resolution failed; external installation is available") {
    return text.stage_external_handoff_body;
  }
  if (detail == "driver handoff needs an explicit result") {
    return text.stage_confirmation_body;
  }
  if (detail == "driver handoff is read-only") {
    return text.stage_not_executed_body;
  }
  if (detail == "system settings reported a failure" ||
      detail == "driver handoff failed" ||
      detail == "installation batch needs explicit recovery" ||
      detail == "software optimization needs explicit recovery" ||
      detail == "software optimization catalog is incomplete") {
    return text.stage_failed_body;
  }
  if (detail == "driver handoff is available" ||
      detail == "driver handoff is not restored" ||
      detail == "system settings are ready for review" ||
      detail == "software installation is ready for selection" ||
      detail == "software optimization recommendations are ready") {
    return text.stage_pending_body;
  }
  return raw_detail(record.detail, text);
}

[[nodiscard]] std::string summary_body(guided::Summary const& summary,
                                        GuidedInitializationPresentationText const& text) {
  auto body = text.summary_prefix + std::to_string(summary.completed);
  body += text.summary_external_prefix + std::to_string(summary.externally_recognized);
  body += text.summary_partial_prefix + std::to_string(summary.partial);
  body += text.summary_failed_prefix + std::to_string(summary.failed);
  body += text.summary_skipped_prefix + std::to_string(summary.skipped);
  body += text.summary_no_applicable_prefix +
          std::to_string(summary.no_applicable_items);
  body += text.summary_not_executed_prefix +
          std::to_string(summary.not_executed);
  body += text.summary_confirmation_prefix +
          std::to_string(summary.result_confirmation_pending);
  body += text.summary_explorer_restart_prefix +
          std::to_string(summary.waiting_explorer_restart);
  body += text.summary_restart_prefix +
          std::to_string(summary.waiting_for_restart);
  body += text.summary_withdrawn_prefix +
          std::to_string(summary.emergency_withdrawn);
  return body;
}

[[nodiscard]] bool active_is_current(guided::Snapshot const& source,
                                      guided::Stage stage) noexcept {
  return source.active.has_value() && source.active->current_stage == stage;
}

}  // namespace

std::shared_ptr<PresentationSnapshot const>
make_guided_initialization_presentation(
    application::guided_initialization::Snapshot const& source,
    GuidedInitializationPresentationText text) {
  std::vector<ComponentProjection> components;
  auto summary = ComponentProjection{
      .id = "guided.summary",
      .automation_id = "AzzsGuidedInitializationSummary",
      .accessible_name = text.summary_accessible_name,
      .kind = ComponentKind::summary,
      .state = source.active.has_value() ? PresentationState::in_progress
                                          : PresentationState::ready,
      .title = text.summary_title,
      .body = summary_body(source.summary, text),
  };
  if (source.mode == guided::LifecycleMode::read_only ||
      source.mode == guided::LifecycleMode::failed) {
    summary.state = PresentationState::failed;
    summary.body += text.summary_error_suffix;
  }
  if (!source.active.has_value() && source.writable &&
      source.mode == guided::LifecycleMode::ready) {
    summary.commands.push_back(command(
        "start", text.start_command, CommandRole::primary,
        IntentKind::continue_workflow, summary.id, true, true));
  }
  summary.commands.push_back(command(
      "refresh", text.refresh_command, CommandRole::secondary,
      IntentKind::continue_workflow, summary.id, source.writable,
      false, source.writable ? "" : text.read_only_disabled_reason));
  summary.commands.push_back(command(
      "history", text.history_command, CommandRole::navigation,
      IntentKind::open_details, summary.id));
  if (source.active.has_value()) {
    summary.commands.push_back(command(
        "cancel", text.cancel_command, CommandRole::danger,
        IntentKind::continue_workflow, summary.id));
  }
  components.push_back(std::move(summary));

  constexpr std::array stages{guided::Stage::drivers,
                              guided::Stage::system_optimization,
                              guided::Stage::software_installation,
                              guided::Stage::software_optimization};
  for (auto const stage_id : stages) {
    auto const* record = source.active.has_value()
                             ? &source.active->stages[static_cast<std::size_t>(stage_id)]
                             : nullptr;
    auto const id = stage_component_id(stage_id);
    ComponentProjection stage{
        .id = id,
        .automation_id = std::string{"AzzsGuidedStage"} +
                         std::to_string(static_cast<int>(stage_id)),
        .accessible_name = stage_title(stage_id, text),
        .kind = ComponentKind::stage_summary,
        .state = record ? presentation_state(record->state)
                        : PresentationState::neutral,
        .title = stage_title(stage_id, text),
        .body = record ? localized_detail(*record, text) : text.stage_empty_body,
        .stage = static_cast<WorkflowStage>(stage_id),
    };
    stage.commands.push_back(command(
        "open", text.open_command, CommandRole::navigation,
        IntentKind::open_details, stage.id));
    if (record && active_is_current(source, stage_id) && source.writable) {
      if (source.active->state == guided::FlowState::awaiting_restart_continue &&
          source.evidence.restart_gate ==
              guided::RestartGateState::awaiting_user_continue) {
        stage.commands.push_back(command(
            "continue", text.continue_command, CommandRole::primary,
            IntentKind::continue_workflow, stage.id, true, true));
        components.push_back(std::move(stage));
        continue;
      }
      if (source.active->state != guided::FlowState::active) {
        components.push_back(std::move(stage));
        continue;
      }
      auto const terminal =
          record->state == guided::StageState::completed ||
          record->state == guided::StageState::skipped ||
          record->state == guided::StageState::no_applicable_items ||
          record->state == guided::StageState::partial ||
          record->state == guided::StageState::not_executed;
      if (record->state == guided::StageState::failed) {
        stage.commands.push_back(command(
            "retry", text.retry_command, CommandRole::primary,
            IntentKind::retry, stage.id, true, true));
      } else if (record->state == guided::StageState::result_confirmation_pending &&
                 stage_id == guided::Stage::drivers) {
        stage.commands.push_back(command(
            "continue", text.continue_command, CommandRole::primary,
            IntentKind::continue_workflow, stage.id, true, true));
      } else if (!terminal && record->state != guided::StageState::external_handoff) {
        stage.commands.push_back(command(
            "skip", text.skip_command, CommandRole::secondary,
            IntentKind::continue_workflow, stage.id));
      }
    }
    components.push_back(std::move(stage));
  }

  if (source.evidence.local_trial_software_catalog) {
    components.push_back(ComponentProjection{
        .id = "guided.local-trial",
        .automation_id = "AzzsGuidedLocalTrialCatalog",
        .accessible_name = text.local_trial_accessible_name,
        .kind = ComponentKind::status_band,
        .state = PresentationState::local_trial,
        .announcement = AnnouncementMode::none,
        .title = text.local_trial_title,
        .body = text.local_trial_body,
    });
  }

  for (auto const& handoff : source.evidence.external_handoffs) {
    if (!source.active.has_value() ||
        std::ranges::find(source.active->external_handoff_software_ids,
                          handoff.software_id) ==
            source.active->external_handoff_software_ids.end()) {
      continue;
    }
    auto const id = "guided.handoff." + handoff.software_id;
    auto const recognized =
        handoff.state == guided::ExternalHandoffState::externally_recognized;
    ComponentProjection component{
        .id = id,
        .automation_id = "AzzsGuidedHandoff." + handoff.software_id,
        .accessible_name = text.handoff_accessible_name,
        .kind = ComponentKind::status_band,
        .state = PresentationState::source_handoff,
        .announcement = AnnouncementMode::polite,
        .title = text.handoff_title,
        .body = recognized ? text.handoff_recognized_body
                           : text.handoff_waiting_body,
    };
    if (recognized && source.writable) {
      component.commands.push_back(command(
          "continue", text.handoff_continue_command, CommandRole::primary,
          IntentKind::continue_workflow, component.id, true, true));
    }
    components.push_back(std::move(component));
  }

  if (source.mode == guided::LifecycleMode::read_only ||
      source.mode == guided::LifecycleMode::failed) {
    components.push_back(ComponentProjection{
        .id = "guided.error",
        .automation_id = "AzzsGuidedInitializationError",
        .accessible_name = text.read_only_accessible_name,
        .kind = ComponentKind::inline_error,
        .state = PresentationState::failed,
        .announcement = AnnouncementMode::assertive,
        .title = text.read_only_title,
        .body = source.error.empty()
                    ? text.read_only_body
                    : text.raw_error_prefix + source.error,
    });
  }

  return std::make_shared<PresentationSnapshot const>(std::move(components));
}

}  // namespace azzs::ui::presentation
