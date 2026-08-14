#include "azzs/domain/software_optimization_batch.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>

namespace azzs::domain::software_optimization_batch {
namespace {

[[nodiscard]] bool bounded(std::string_view value,
                           std::size_t maximum = 256) noexcept {
  return !value.empty() && value.size() <= maximum;
}

[[nodiscard]] bool version_range_valid(catalog::VersionRange const& value) {
  return bounded(value.minimum) && bounded(value.maximum) &&
         value.minimum <= value.maximum;
}

[[nodiscard]] bool controlled_rule_valid(catalog::ControlledRule const& rule) {
  return rule.kind == catalog::RuleKind::built_in_definition &&
         rule.definition.valid();
}

[[nodiscard]] bool option_shape_valid(
    catalog::SoftwareOptimizationOption const& option) {
  if (!option.id.valid() || !option.scheme_id.valid() ||
      !version_range_valid(option.supported_versions) ||
      option.automation != catalog::AutomationSupport::controlled ||
      !controlled_rule_valid(option.execution) ||
      !controlled_rule_valid(option.state_detection) ||
      !bounded(option.impact, 4096)) {
    return false;
  }
  if (option.default_value.has_value() &&
      std::ranges::find(option.allowed_values, *option.default_value) ==
          option.allowed_values.end()) {
    return false;
  }
  return std::ranges::all_of(option.allowed_values, [](auto const& value) {
    return bounded(value);
  });
}

[[nodiscard]] bool terminal_batch_state(OptimizationBatchState state) noexcept {
  return state == OptimizationBatchState::stopped ||
         state == OptimizationBatchState::completed ||
         state == OptimizationBatchState::failed_closed;
}

[[nodiscard]] bool valid_step_state(OptimizationStepState state) noexcept {
  switch (state) {
    case OptimizationStepState::pending:
    case OptimizationStepState::executing:
    case OptimizationStepState::awaiting_target_exit:
    case OptimizationStepState::force_close_confirmation_pending:
    case OptimizationStepState::result_confirmation_pending:
    case OptimizationStepState::waiting_restart:
    case OptimizationStepState::optimized:
    case OptimizationStepState::failed:
    case OptimizationStepState::blocked_by_withdrawal:
    case OptimizationStepState::not_executed:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_batch_state(OptimizationBatchState state) noexcept {
  switch (state) {
    case OptimizationBatchState::ready:
    case OptimizationBatchState::running:
    case OptimizationBatchState::stopping:
    case OptimizationBatchState::awaiting_user:
    case OptimizationBatchState::waiting_restart:
    case OptimizationBatchState::closing:
    case OptimizationBatchState::stopped:
    case OptimizationBatchState::completed:
    case OptimizationBatchState::recovery_required:
    case OptimizationBatchState::failed_closed:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_transition_outcome(
    DurableTransitionOutcome outcome) noexcept {
  return outcome == DurableTransitionOutcome::committed ||
         outcome == DurableTransitionOutcome::outcome_unknown ||
         outcome == DurableTransitionOutcome::failed_closed;
}

}  // namespace

bool FrozenOptimizationOption::valid() const noexcept {
  if (!option_shape_valid(option)) {
    return false;
  }
  if (!selected_value.has_value()) {
    return !option.default_value.has_value() ||
           option.allowed_values.empty();
  }
  return std::ranges::find(option.allowed_values, *selected_value) !=
         option.allowed_values.end();
}

bool FrozenOptimizationScheme::valid() const noexcept {
  if (!target.id.valid() || !target.identity_anchor.valid() ||
      !scheme.id.valid() || scheme.target_id != target.id ||
      scheme.automation != catalog::AutomationSupport::controlled ||
      !version_range_valid(scheme.supported_versions) ||
      !bounded(detected_version) || !bounded(risk_confirmation_id) ||
      selected_options.empty() ||
      selected_options.size() > 128) {
    return false;
  }
  if (forced_version_execution != force_version_confirmed) {
    return false;
  }
  if (forced_version_execution && !bounded(force_risk_version)) {
    return false;
  }
  if (forced_version_execution &&
      (!force_version_confirmation_id.has_value() ||
       !bounded(*force_version_confirmation_id))) {
    return false;
  }
  if (!forced_version_execution && !force_risk_version.empty()) {
    return false;
  }
  if (!forced_version_execution && force_version_confirmation_id.has_value()) {
    return false;
  }
  for (auto const& option : selected_options) {
    if (!option.valid() || option.option.scheme_id != scheme.id) {
      return false;
    }
  }
  for (std::size_t index = 0; index < selected_options.size(); ++index) {
    for (std::size_t other = index + 1; other < selected_options.size(); ++other) {
      if (selected_options[index].option.id == selected_options[other].option.id) {
        return false;
      }
    }
  }
  return true;
}

bool FrozenOptimizationBatchPlan::valid() const noexcept {
  if (!bounded(batch_id) || !bounded(correlation_id) || catalog_revision == 0 ||
      frozen_at_milliseconds < 0 || schemes.empty() || schemes.size() > 64) {
    return false;
  }
  if (retry_of_batch_id.has_value() &&
      (!bounded(*retry_of_batch_id) || *retry_of_batch_id == batch_id)) {
    return false;
  }
  for (auto const& scheme : schemes) {
    if (!scheme.valid()) {
      return false;
    }
  }
  for (std::size_t index = 0; index < schemes.size(); ++index) {
    for (std::size_t other = index + 1; other < schemes.size(); ++other) {
      if (schemes[index].scheme.id == schemes[other].scheme.id) {
        return false;
      }
    }
  }
  return true;
}

bool OptimizationStepProgress::valid() const noexcept {
  return bounded(scheme_id) && bounded(option_id) && attempt <= 1024 &&
         valid_step_state(state) && detail.size() <= 4096 &&
         !(force_close_completed && !force_close_confirmation_requested);
}

bool DurableLeaseBinding::valid() const noexcept {
  return bounded(kind) && bounded(operation_id) && bounded(correlation_id) &&
         bounded(lease_token_fingerprint) && occupancy_revision > 0;
}

bool LastDurableTransition::valid() const noexcept {
  return generation > 0 && bounded(scheme_id) && bounded(option_id) &&
         valid_step_state(step_state) && valid_transition_outcome(outcome);
}

bool OptimizationBatchRecord::valid() const noexcept {
  if (!plan.valid() || !valid_batch_state(state) || steps.empty() || generation == 0 ||
      !last_transition.valid()) {
    return false;
  }
  if (active_lease.has_value() && !active_lease->valid()) {
    return false;
  }
  if (last_transition.generation > generation) {
    return false;
  }
  std::size_t expected_steps{};
  for (auto const& scheme : plan.schemes) {
    expected_steps += scheme.selected_options.size();
  }
  if (expected_steps != steps.size()) {
    return false;
  }
  std::size_t cursor{};
  for (auto const& scheme : plan.schemes) {
    for (auto const& option : scheme.selected_options) {
      auto const& step = steps[cursor++];
      if (!step.valid() || step.scheme_id != scheme.scheme.id.value ||
          step.option_id != option.option.id.value) {
        return false;
      }
    }
  }
  return true;
}

bool OptimizationBatchHistory::valid() const noexcept {
  if (!plan.valid() || !terminal_batch_state(final_state) ||
      reason.size() > 4096 || steps.empty()) {
    return false;
  }
  OptimizationBatchRecord record{
      .plan = plan,
      .state = final_state,
      .steps = steps,
      .generation = 1,
      .last_transition = {.generation = 1,
                          .scheme_id = steps.front().scheme_id,
                          .option_id = steps.front().option_id,
                          .step_state = steps.front().state,
                          .outcome = DurableTransitionOutcome::committed},
  };
  return record.valid();
}

bool is_terminal(OptimizationStepState state) noexcept {
  return state == OptimizationStepState::optimized ||
         state == OptimizationStepState::failed ||
         state == OptimizationStepState::blocked_by_withdrawal ||
         state == OptimizationStepState::not_executed;
}

bool blocks_batch(OptimizationStepState state) noexcept {
  return state == OptimizationStepState::executing ||
         state == OptimizationStepState::awaiting_target_exit ||
         state == OptimizationStepState::force_close_confirmation_pending ||
         state == OptimizationStepState::result_confirmation_pending ||
         state == OptimizationStepState::waiting_restart;
}

bool requires_fresh_retry_snapshot(OptimizationStepState state) noexcept {
  return state == OptimizationStepState::failed ||
         state == OptimizationStepState::not_executed ||
         state == OptimizationStepState::blocked_by_withdrawal;
}

bool command_allowed(OptimizationStepState state,
                     OptimizationBatchCommand command) noexcept {
  switch (command) {
    case OptimizationBatchCommand::start:
      return state == OptimizationStepState::pending ||
             state == OptimizationStepState::executing;
    case OptimizationBatchCommand::stop:
    case OptimizationBatchCommand::request_close:
      return !is_terminal(state);
    case OptimizationBatchCommand::request_force_close:
      return state == OptimizationStepState::awaiting_target_exit;
    case OptimizationBatchCommand::confirm_force_close:
    case OptimizationBatchCommand::cancel_force_close:
      return state == OptimizationStepState::force_close_confirmation_pending;
    case OptimizationBatchCommand::confirm_current_complete:
      return state == OptimizationStepState::result_confirmation_pending;
    case OptimizationBatchCommand::recover_read_only:
    case OptimizationBatchCommand::continue_after_recovery:
      return blocks_batch(state);
  }
  return false;
}

char const* to_string(OptimizationStepState value) noexcept {
  switch (value) {
    case OptimizationStepState::pending:
      return "pending";
    case OptimizationStepState::executing:
      return "executing";
    case OptimizationStepState::awaiting_target_exit:
      return "awaiting-target-exit";
    case OptimizationStepState::force_close_confirmation_pending:
      return "force-close-confirmation-pending";
    case OptimizationStepState::result_confirmation_pending:
      return "result-confirmation-pending";
    case OptimizationStepState::waiting_restart:
      return "waiting-restart";
    case OptimizationStepState::optimized:
      return "optimized";
    case OptimizationStepState::failed:
      return "failed";
    case OptimizationStepState::blocked_by_withdrawal:
      return "blocked-by-withdrawal";
    case OptimizationStepState::not_executed:
      return "not-executed";
  }
  return "unknown";
}

char const* to_string(OptimizationBatchState value) noexcept {
  switch (value) {
    case OptimizationBatchState::ready:
      return "ready";
    case OptimizationBatchState::running:
      return "running";
    case OptimizationBatchState::stopping:
      return "stopping";
    case OptimizationBatchState::awaiting_user:
      return "awaiting-user";
    case OptimizationBatchState::waiting_restart:
      return "waiting-restart";
    case OptimizationBatchState::closing:
      return "closing";
    case OptimizationBatchState::stopped:
      return "stopped";
    case OptimizationBatchState::completed:
      return "completed";
    case OptimizationBatchState::recovery_required:
      return "recovery-required";
    case OptimizationBatchState::failed_closed:
      return "failed-closed";
  }
  return "unknown";
}

}  // namespace azzs::domain::software_optimization_batch
