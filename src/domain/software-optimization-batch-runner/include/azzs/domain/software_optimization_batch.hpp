#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "azzs/domain/software_optimization_catalog.hpp"

namespace azzs::domain::software_optimization_batch {

namespace catalog = domain::software_optimization_catalog;

// A batch retains the validated catalog declarations that determine effects.
// Catalog updates can therefore affect only future batches, never this plan.
struct FrozenOptimizationOption final {
  catalog::SoftwareOptimizationOption option;
  std::optional<std::string> selected_value;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenOptimizationOption const&) const = default;
};

struct FrozenOptimizationScheme final {
  catalog::TargetSoftware target;
  catalog::SoftwareOptimizationScheme scheme;
  std::string detected_version;
  std::string risk_confirmation_id;
  bool forced_version_execution{false};
  std::string force_risk_version;
  bool force_version_confirmed{false};
  std::optional<std::string> force_version_confirmation_id;
  std::vector<FrozenOptimizationOption> selected_options;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenOptimizationScheme const&) const = default;
};

struct FrozenOptimizationBatchPlan final {
  std::string batch_id;
  std::string correlation_id;
  std::optional<std::string> retry_of_batch_id;
  std::uint64_t catalog_revision{};
  std::uint64_t emergency_notice_revision{};
  std::vector<FrozenOptimizationScheme> schemes;
  std::int64_t frozen_at_milliseconds{};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenOptimizationBatchPlan const&) const = default;
};

enum class OptimizationStepState {
  pending,
  executing,
  awaiting_target_exit,
  force_close_confirmation_pending,
  result_confirmation_pending,
  waiting_restart,
  optimized,
  failed,
  blocked_by_withdrawal,
  not_executed,
};

enum class OptimizationBatchState {
  ready,
  running,
  stopping,
  awaiting_user,
  waiting_restart,
  closing,
  stopped,
  completed,
  recovery_required,
  failed_closed,
};

enum class OptimizationBatchCommand {
  start,
  stop,
  request_force_close,
  confirm_force_close,
  cancel_force_close,
  request_force_termination,
  confirm_force_termination,
  cancel_force_termination,
  confirm_current_complete,
  request_close,
  recover_read_only,
  continue_after_recovery,
};

struct OptimizationStepProgress final {
  std::string scheme_id;
  std::string option_id;
  OptimizationStepState state{OptimizationStepState::pending};
  std::uint32_t attempt{};
  // Persisted before the effect begins. An interrupted process is therefore
  // recovered as an unknown result instead of being replayed automatically.
  bool execution_started{false};
  bool target_exit_confirmed{false};
  bool force_close_confirmation_requested{false};
  bool force_close_completed{false};
  bool force_termination_confirmation_requested{false};
  bool force_termination_completed{false};
  std::uint64_t emergency_notice_revision{};
  std::string detail;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(OptimizationStepProgress const&) const = default;
};

struct DurableLeaseBinding final {
  std::string kind;
  std::string operation_id;
  std::string correlation_id;
  std::string lease_token_fingerprint;
  std::uint64_t occupancy_revision{};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(DurableLeaseBinding const&) const = default;
};

enum class DurableTransitionOutcome {
  committed,
  outcome_unknown,
  failed_closed,
};

struct LastDurableTransition final {
  std::uint64_t generation{};
  std::string scheme_id;
  std::string option_id;
  OptimizationStepState step_state{OptimizationStepState::pending};
  DurableTransitionOutcome outcome{DurableTransitionOutcome::failed_closed};
  bool coverage_gap{false};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(LastDurableTransition const&) const = default;
};

struct OptimizationBatchRecord final {
  FrozenOptimizationBatchPlan plan;
  OptimizationBatchState state{OptimizationBatchState::ready};
  bool close_requested{false};
  bool stop_requested{false};
  std::vector<OptimizationStepProgress> steps;
  std::uint64_t generation{};
  std::optional<DurableLeaseBinding> active_lease;
  LastDurableTransition last_transition;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(OptimizationBatchRecord const&) const = default;
};

struct OptimizationBatchHistory final {
  FrozenOptimizationBatchPlan plan;
  OptimizationBatchState final_state{OptimizationBatchState::failed_closed};
  std::vector<OptimizationStepProgress> steps;
  std::string reason;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(OptimizationBatchHistory const&) const = default;
};

struct OptimizationBatchSnapshot final {
  std::optional<OptimizationBatchRecord> active;
  std::vector<OptimizationBatchHistory> history;
  bool writable{false};
  std::string error;
};

[[nodiscard]] bool is_terminal(OptimizationStepState state) noexcept;
[[nodiscard]] bool blocks_batch(OptimizationStepState state) noexcept;
[[nodiscard]] bool requires_fresh_retry_snapshot(
    OptimizationStepState state) noexcept;
[[nodiscard]] bool command_allowed(OptimizationStepState state,
                                   OptimizationBatchCommand command) noexcept;
[[nodiscard]] char const* to_string(OptimizationStepState value) noexcept;
[[nodiscard]] char const* to_string(OptimizationBatchState value) noexcept;

}  // namespace azzs::domain::software_optimization_batch
