#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "azzs/application/software_optimization_discovery.hpp"
#include "azzs/domain/software_optimization_batch.hpp"

namespace azzs::application {
class DeviceStateStore;
class EmergencyWithdrawalService;
class ExecutionLog;
class SharedOperationOccupancy;
class SoftwareOptimizationCatalogLifecycle;
}

namespace azzs::application::sogou_optimization {
class SogouOptimizationService;
}

namespace azzs::application::software_optimization_batch {

namespace batch_domain = domain::software_optimization_batch;
namespace discovery_app = application::software_optimization_discovery;

enum class StepExecutionCode {
  applied,
  already_effective,
  confirmation_required,
  version_not_supported,
  unsupported,
  failed,
};

struct StepExecutionObservation final {
  StepExecutionCode code{StepExecutionCode::failed};
  std::string detail;
};

enum class StepVerificationCode {
  optimized,
  not_optimized,
  confirmation_required,
  version_not_supported,
  failed,
};

struct StepVerificationObservation final {
  StepVerificationCode code{StepVerificationCode::failed};
  std::string detail;
};

struct TargetExitObservation final {
  bool known{false};
  bool exited{false};
  std::string detail;
};

struct TargetTerminationObservation final {
  bool known{false};
  bool terminated{false};
  std::string detail;
};

class SoftwareOptimizationStepExecutor {
 public:
  virtual ~SoftwareOptimizationStepExecutor() = default;
  [[nodiscard]] virtual StepExecutionObservation execute(
      batch_domain::FrozenOptimizationOption const& option) = 0;
  [[nodiscard]] virtual StepVerificationObservation verify(
      batch_domain::FrozenOptimizationOption const& option) = 0;
  [[nodiscard]] virtual TargetExitObservation observe_target_exit(
      batch_domain::FrozenOptimizationScheme const& scheme) = 0;
  // These operations default to unavailable so an incomplete platform
  // registration cannot silently become a process-control capability.
  [[nodiscard]] virtual TargetExitObservation force_close_target(
      batch_domain::FrozenOptimizationScheme const&) {
    return {.detail = "no verified controlled force-close adapter is registered"};
  }
  [[nodiscard]] virtual TargetTerminationObservation force_terminate(
      batch_domain::FrozenOptimizationScheme const&,
      batch_domain::FrozenOptimizationOption const&) {
    return {.detail = "no verified controlled force-termination adapter is registered"};
  }
};

// The executor only maps frozen, built-in rule identities to the existing
// Sogou service. It cannot manufacture a generic registry/process/UI surface.
class SogouOptimizationBatchExecutor final
    : public SoftwareOptimizationStepExecutor {
 public:
  explicit SogouOptimizationBatchExecutor(
      sogou_optimization::SogouOptimizationService& service) noexcept;

  [[nodiscard]] StepExecutionObservation execute(
      batch_domain::FrozenOptimizationOption const& option) override;
  [[nodiscard]] StepVerificationObservation verify(
      batch_domain::FrozenOptimizationOption const& option) override;
  [[nodiscard]] TargetExitObservation observe_target_exit(
      batch_domain::FrozenOptimizationScheme const& scheme) override;

 private:
  sogou_optimization::SogouOptimizationService& service_;
};

enum class WithdrawalAuthorizationCode {
  allowed,
  blocked,
  unavailable,
};

struct WithdrawalAuthorization final {
  WithdrawalAuthorizationCode code{WithdrawalAuthorizationCode::unavailable};
  std::uint64_t notice_revision{};
  std::string reason;
};

class SoftwareOptimizationWithdrawalAuthorization {
 public:
  virtual ~SoftwareOptimizationWithdrawalAuthorization() = default;
  [[nodiscard]] virtual WithdrawalAuthorization authorize(
      batch_domain::FrozenOptimizationScheme const& scheme,
      batch_domain::FrozenOptimizationOption const& option) = 0;
};

class EmergencyWithdrawalOptimizationAuthorization final
    : public SoftwareOptimizationWithdrawalAuthorization {
 public:
  explicit EmergencyWithdrawalOptimizationAuthorization(
      EmergencyWithdrawalService& withdrawals) noexcept;

  [[nodiscard]] WithdrawalAuthorization authorize(
      batch_domain::FrozenOptimizationScheme const& scheme,
      batch_domain::FrozenOptimizationOption const& option) override;

 private:
  EmergencyWithdrawalService& withdrawals_;
};

struct SchemeExecutionConfirmation final {
  std::string scheme_id;
  std::string risk_confirmation_id;
  bool forced_version_execution{false};
  std::string force_risk_version;
  std::optional<std::string> force_version_confirmation_id;
};

// A typed bridge from issue 26 selection into issue 27. The source freezes
// current catalog values once; it never creates a batch or starts an effect.
struct OptimizationBatchStartRequest final {
  std::string batch_id;
  std::string correlation_id;
  discovery_app::SoftwareOptimizationSubmissionRequest submission;
  std::vector<SchemeExecutionConfirmation> confirmations;
  std::int64_t frozen_at_milliseconds{};
};

struct FrozenPlanAdmission final {
  bool accepted{false};
  std::optional<batch_domain::FrozenOptimizationBatchPlan> plan;
  std::string detail;
};

class SoftwareOptimizationBatchPlanSource {
 public:
  virtual ~SoftwareOptimizationBatchPlanSource() = default;
  [[nodiscard]] virtual FrozenPlanAdmission freeze(
      OptimizationBatchStartRequest const& request) = 0;
};

class DiscoveryOptimizationBatchPlanSource final
    : public SoftwareOptimizationBatchPlanSource {
 public:
  DiscoveryOptimizationBatchPlanSource(
      SoftwareOptimizationCatalogLifecycle& catalogs,
      discovery_app::SoftwareOptimizationDiscoveryService& discovery) noexcept;

  [[nodiscard]] FrozenPlanAdmission freeze(
      OptimizationBatchStartRequest const& request) override;

 private:
  SoftwareOptimizationCatalogLifecycle& catalogs_;
  discovery_app::SoftwareOptimizationDiscoveryService& discovery_;
};

enum class OptimizationBatchActionCode {
  succeeded,
  not_restored,
  rejected,
  occupied,
  read_only,
  persistence_failed,
  outcome_unknown,
  no_active_batch,
  blocked,
  recovery_required,
  confirmation_required,
  unsupported,
};

struct OptimizationBatchActionResult final {
  OptimizationBatchActionCode code{OptimizationBatchActionCode::rejected};
  batch_domain::OptimizationBatchSnapshot snapshot;
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == OptimizationBatchActionCode::succeeded;
  }
};

// The single writer for optimization batches. It retains the same device lease
// as installation batches and serializes one frozen option at a time.
class SoftwareOptimizationBatchService final {
 public:
  SoftwareOptimizationBatchService(
      DeviceStateStore& states, SharedOperationOccupancy& occupancy,
      ExecutionLog& log, SoftwareOptimizationBatchPlanSource& plans,
      SoftwareOptimizationStepExecutor& executor,
      SoftwareOptimizationWithdrawalAuthorization& withdrawals);
  ~SoftwareOptimizationBatchService();

  [[nodiscard]] OptimizationBatchActionResult restore();
  [[nodiscard]] batch_domain::OptimizationBatchSnapshot snapshot() const;
  [[nodiscard]] OptimizationBatchActionResult create(
      OptimizationBatchStartRequest const& request);
  [[nodiscard]] OptimizationBatchActionResult advance();
  [[nodiscard]] OptimizationBatchActionResult confirm_current_target_exit();
  [[nodiscard]] OptimizationBatchActionResult confirm_current_complete();
  [[nodiscard]] OptimizationBatchActionResult stop_current();
  [[nodiscard]] OptimizationBatchActionResult request_force_close();
  [[nodiscard]] OptimizationBatchActionResult confirm_force_close();
  [[nodiscard]] OptimizationBatchActionResult cancel_force_close();
  [[nodiscard]] OptimizationBatchActionResult request_force_termination();
  [[nodiscard]] OptimizationBatchActionResult confirm_force_termination();
  [[nodiscard]] OptimizationBatchActionResult cancel_force_termination();
  [[nodiscard]] OptimizationBatchActionResult request_close();
  [[nodiscard]] OptimizationBatchActionResult recover_read_only();
  [[nodiscard]] OptimizationBatchActionResult continue_after_recovery();
  [[nodiscard]] OptimizationBatchActionResult retry_current(
      OptimizationBatchStartRequest const& request);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] char const* to_string(OptimizationBatchActionCode value) noexcept;

}  // namespace azzs::application::software_optimization_batch
