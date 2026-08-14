#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/software_optimization_batch.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace app = azzs::application;
namespace batch_app = azzs::application::software_optimization_batch;
namespace batch = azzs::domain::software_optimization_batch;
namespace catalog = azzs::domain::software_optimization_catalog;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "software optimization batch runner contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingLog final : public app::ExecutionLog {
 public:
  [[nodiscard]] app::CorrelationId begin_correlation() override {
    return {.value = "optimization-batch-contract-" + std::to_string(next_++)};
  }

  [[nodiscard]] app::ExecutionLogReceipt append(
      app::CorrelationId const&, app::ExecutionEvent const& event) override {
    std::scoped_lock lock{mutex_};
    events.push_back(event);
    events_changed_.notify_all();
    if (fail_next_append) {
      fail_next_append = false;
      return {.persisted = false, .error = "injected execution log failure"};
    }
    return {.persisted = true, .segment = 1, .sequence = next_++};
  }

  [[nodiscard]] app::ExecutionLogClearReceipt clear() override {
    return {.cleared = true};
  }

  [[nodiscard]] app::DiagnosticExportReceipt export_diagnostic(
      app::DiagnosticContext const&) override {
    return {.produced = true};
  }

  [[nodiscard]] bool wait_for_stage(std::string_view stage) {
    std::unique_lock lock{mutex_};
    return events_changed_.wait_for(lock, std::chrono::seconds{5}, [&] {
      for (auto const& event : events) {
        if (event.stage == stage) {
          return true;
        }
      }
      return false;
    });
  }

  bool fail_next_append{false};
  std::vector<app::ExecutionEvent> events;

 private:
  std::mutex mutex_;
  std::condition_variable events_changed_;
  std::uint64_t next_{1};
};

class ScriptedExecutor final : public batch_app::SoftwareOptimizationStepExecutor {
 public:
  [[nodiscard]] batch_app::StepExecutionObservation execute(
      batch::FrozenOptimizationOption const& option) override {
    executed_ids.push_back(option.option.id.value);
    if (block_execution) {
      std::unique_lock lock{execution_mutex};
      execution_entered = true;
      execution_condition.notify_all();
      execution_condition.wait(lock, [&] { return release_execution; });
    }
    if (executions.empty()) {
      return {.code = batch_app::StepExecutionCode::applied};
    }
    auto result = std::move(executions.front());
    executions.erase(executions.begin());
    return result;
  }

  [[nodiscard]] batch_app::StepVerificationObservation verify(
      batch::FrozenOptimizationOption const& option) override {
    verified_ids.push_back(option.option.id.value);
    if (verifications.empty()) {
      return {.code = batch_app::StepVerificationCode::optimized};
    }
    auto result = std::move(verifications.front());
    verifications.erase(verifications.begin());
    return result;
  }

  [[nodiscard]] batch_app::TargetExitObservation observe_target_exit(
      batch::FrozenOptimizationScheme const& scheme) override {
    observed_targets.push_back(scheme.target.id.value);
    if (target_exits.empty()) {
      return {.known = true, .exited = true};
    }
    auto result = std::move(target_exits.front());
    target_exits.erase(target_exits.begin());
    return result;
  }

  [[nodiscard]] batch_app::TargetExitObservation force_close_target(
      batch::FrozenOptimizationScheme const& scheme) override {
    forced_close_targets.push_back(scheme.target.id.value);
    if (force_closes.empty()) {
      return {.detail = "no scripted force-close result"};
    }
    auto result = std::move(force_closes.front());
    force_closes.erase(force_closes.begin());
    return result;
  }

  [[nodiscard]] batch_app::TargetTerminationObservation force_terminate(
      batch::FrozenOptimizationScheme const& scheme,
      batch::FrozenOptimizationOption const& option) override {
    force_terminated_ids.push_back(scheme.target.id.value + "/" +
                                   option.option.id.value);
    if (force_terminations.empty()) {
      return {.detail = "no scripted force-termination result"};
    }
    auto result = std::move(force_terminations.front());
    force_terminations.erase(force_terminations.begin());
    return result;
  }

  [[nodiscard]] bool wait_for_execution() {
    std::unique_lock lock{execution_mutex};
    return execution_condition.wait_for(
        lock, std::chrono::seconds{5}, [&] { return execution_entered; });
  }

  void release_blocked_execution() {
    std::scoped_lock lock{execution_mutex};
    release_execution = true;
    execution_condition.notify_all();
  }

  std::vector<batch_app::StepExecutionObservation> executions;
  std::vector<batch_app::StepVerificationObservation> verifications;
  std::vector<batch_app::TargetExitObservation> target_exits;
  std::vector<batch_app::TargetExitObservation> force_closes;
  std::vector<batch_app::TargetTerminationObservation> force_terminations;
  std::vector<std::string> executed_ids;
  std::vector<std::string> verified_ids;
  std::vector<std::string> observed_targets;
  std::vector<std::string> forced_close_targets;
  std::vector<std::string> force_terminated_ids;
  bool block_execution{false};

 private:
  std::mutex execution_mutex;
  std::condition_variable execution_condition;
  bool execution_entered{false};
  bool release_execution{false};
};

class ScriptedWithdrawals final
    : public batch_app::SoftwareOptimizationWithdrawalAuthorization {
 public:
  [[nodiscard]] batch_app::WithdrawalAuthorization authorize(
      batch::FrozenOptimizationScheme const& scheme,
      batch::FrozenOptimizationOption const& option) override {
    requests.push_back(scheme.scheme.id.value + "/" + option.option.id.value);
    if (authorizations.empty()) {
      return {.code = batch_app::WithdrawalAuthorizationCode::allowed,
              .notice_revision = 9};
    }
    auto result = std::move(authorizations.front());
    authorizations.erase(authorizations.begin());
    return result;
  }

  std::vector<batch_app::WithdrawalAuthorization> authorizations;
  std::vector<std::string> requests;
};

class FixedPlanSource final : public batch_app::SoftwareOptimizationBatchPlanSource {
 public:
  explicit FixedPlanSource(batch::FrozenOptimizationBatchPlan plan)
      : next_plan(std::move(plan)) {}

  [[nodiscard]] batch_app::FrozenPlanAdmission freeze(
      batch_app::OptimizationBatchStartRequest const&) override {
    ++calls;
    if (!accept) return {.detail = "injected plan rejection"};
    return {.accepted = true, .plan = next_plan};
  }

  bool accept{true};
  std::size_t calls{};
  batch::FrozenOptimizationBatchPlan next_plan;
};

[[nodiscard]] catalog::TargetSoftware make_target(std::string suffix) {
  return {
      .id = {"target-" + suffix},
      .identity_anchor = {"vendor.product." + suffix},
      .support_mode = catalog::SupportMode::supported,
      .supported_versions = {"1.0", "2.0"},
      .install_detection = {catalog::RuleKind::built_in_definition,
                            {"detect-install-" + suffix}},
      .version_detection = {catalog::RuleKind::built_in_definition,
                            {"detect-version-" + suffix}},
      .explanation_source = "contract target",
  };
}

[[nodiscard]] batch::FrozenOptimizationBatchPlan make_plan(
    std::string batch_id, std::size_t count,
    catalog::ExitRequirement exit_requirement = catalog::ExitRequirement::none,
    catalog::RestartRequirement restart_requirement = catalog::RestartRequirement::none) {
  batch::FrozenOptimizationBatchPlan plan{
      .batch_id = std::move(batch_id),
      .correlation_id = "optimization-contract-correlation",
      .catalog_revision = 7,
      .emergency_notice_revision = 4,
      .frozen_at_milliseconds = 100,
  };
  for (std::size_t index = 0; index < count; ++index) {
    auto suffix = std::to_string(index + 1);
    auto target = make_target(suffix);
    catalog::SoftwareOptimizationScheme scheme{
        .id = {"scheme-" + suffix},
        .target_id = target.id,
        .automation = catalog::AutomationSupport::controlled,
        .supported_versions = {"1.0", "2.0"},
        .impact = "project-owned optimization",
        .risk = catalog::RiskLevel::low,
        .exit_requirement = exit_requirement,
        .restart_requirement = restart_requirement,
        .explanation_source = "contract scheme",
        .availability = catalog::SchemeAvailability::available,
    };
    catalog::SoftwareOptimizationOption option{
        .id = {"option-" + suffix},
        .scheme_id = scheme.id,
        .supported_versions = {"1.0", "2.0"},
        .impact = "project-owned option",
        .automation = catalog::AutomationSupport::controlled,
        .execution = {catalog::RuleKind::built_in_definition,
                      {"execute-option-" + suffix}},
        .state_detection = {catalog::RuleKind::built_in_definition,
                            {"detect-option-" + suffix}},
        .explanation_source = "contract option",
    };
    plan.schemes.push_back({
        .target = std::move(target),
        .scheme = std::move(scheme),
        .detected_version = "1.5",
        .risk_confirmation_id = "risk-confirmation-" + suffix,
        .selected_options = {{.option = std::move(option)}},
    });
  }
  return plan;
}

[[nodiscard]] batch_app::OptimizationBatchStartRequest start_request(
    std::string batch_id) {
  return {.batch_id = std::move(batch_id),
          .correlation_id = "requested-correlation",
          .frozen_at_milliseconds = 101};
}

struct Fixture final {
  Fixture(batch::FrozenOptimizationBatchPlan plan, std::string lease_prefix = "lease-")
      : clock{app::WallClockTime{std::chrono::milliseconds{1000}}},
        states{files, clock},
        tokens{std::move(lease_prefix)},
        occupancy{occupancy_storage, tokens},
        plans{std::move(plan)},
        service{states, occupancy, log, plans, executor, withdrawals} {}

  azzs::testing::FixedClock clock;
  azzs::testing::InMemoryStateFileSystem files;
  app::DeviceStateStore states;
  azzs::testing::InMemoryOperationOccupancyStorage occupancy_storage;
  azzs::testing::SequenceLeaseTokenSource tokens;
  app::SharedOperationOccupancy occupancy;
  RecordingLog log;
  FixedPlanSource plans;
  ScriptedExecutor executor;
  ScriptedWithdrawals withdrawals;
  batch_app::SoftwareOptimizationBatchService service;
};

[[nodiscard]] bool frozen_serial_and_shared_occupancy_contract() {
  Fixture fixture{make_plan("batch-serial", 2)};
  bool passed = expect(fixture.service.restore().succeeded(),
                       "an uninitialized batch state must restore writable");
  passed &= expect(fixture.service.create(start_request("batch-serial")).succeeded(),
                   "one or more selected options must create a frozen batch");

  azzs::testing::SequenceLeaseTokenSource peer_tokens{"peer-"};
  app::SharedOperationOccupancy peer{fixture.occupancy_storage, peer_tokens};
  passed &= expect(peer.try_acquire({.kind = "installation-batch",
                                    .operation_id = "other-installation",
                                    .correlation_id = "peer"})
                       .code == app::OccupancyResultCode::occupied,
                   "installation and optimization batches must share device occupancy");

  passed &= expect(fixture.service.advance().succeeded(),
                   "the first frozen option must execute");
  auto first = fixture.service.snapshot();
  passed &= expect(first.active.has_value() &&
                       first.active->steps[0].state == batch::OptimizationStepState::optimized &&
                       first.active->steps[1].state == batch::OptimizationStepState::pending &&
                       fixture.executor.executed_ids == std::vector<std::string>{"option-1"},
                   "execution must remain serial and retain the frozen first option");
  passed &= expect(fixture.service.advance().succeeded(),
                   "the second frozen option must execute after the first result");
  auto completed = fixture.service.snapshot();
  passed &= expect(completed.active.has_value() &&
                       completed.active->state == batch::OptimizationBatchState::completed &&
                       completed.active->plan.schemes.size() == 2 &&
                       fixture.executor.executed_ids ==
                           std::vector<std::string>{"option-1", "option-2"},
                   "the final snapshot must retain the immutable two-option plan");
  auto released = peer.try_acquire({.kind = "installation-batch",
                                    .operation_id = "after-optimization",
                                    .correlation_id = "peer"});
  passed &= expect(released.code == app::OccupancyResultCode::acquired,
                   "terminal batches must release shared occupancy");
  if (released.lease.has_value()) static_cast<void>(peer.release(*released.lease));
  return passed;
}

[[nodiscard]] bool withdrawal_and_unknown_result_contract() {
  Fixture fixture{make_plan("batch-withdrawal", 2)};
  bool passed = expect(fixture.service.restore().succeeded(), "batch must restore");
  passed &= expect(fixture.service.create(start_request("batch-withdrawal")).succeeded(),
                   "batch must be created before withdrawal handling");
  fixture.withdrawals.authorizations.push_back(
      {.code = batch_app::WithdrawalAuthorizationCode::blocked,
       .notice_revision = 21,
       .reason = "emergency withdrawal"});
  passed &= expect(fixture.service.advance().succeeded(),
                   "a withdrawn option must be recorded without executing it");
  auto withdrawn = fixture.service.snapshot();
  passed &= expect(withdrawn.active->steps[0].state ==
                           batch::OptimizationStepState::blocked_by_withdrawal &&
                       withdrawn.active->steps[0].emergency_notice_revision == 21 &&
                       fixture.executor.executed_ids.empty(),
                   "withdrawal must block only the matching unstarted option");

  fixture.executor.executions.push_back(
      {.code = batch_app::StepExecutionCode::confirmation_required,
       .detail = "adapter result unknown"});
  passed &= expect(fixture.service.advance().succeeded(),
                   "an unrelated later option must remain independently runnable");
  auto pending = fixture.service.snapshot();
  passed &= expect(pending.active.has_value() &&
                       pending.active->steps[1].state ==
                           batch::OptimizationStepState::result_confirmation_pending &&
                       pending.active->state == batch::OptimizationBatchState::awaiting_user,
                   "unknown execution must never be reported as optimized");
  fixture.executor.verifications.push_back(
      {.code = batch_app::StepVerificationCode::confirmation_required,
       .detail = "automatic verification remains unknown"});
  passed &= expect(fixture.service.confirm_current_complete().succeeded(),
                   "a user-confirmed result must finish a pending optimization");
  auto confirmed = fixture.service.snapshot();
  passed &= expect(confirmed.active.has_value() &&
                       confirmed.active->steps[1].state ==
                           batch::OptimizationStepState::optimized &&
                       confirmed.active->steps[1].detail ==
                           "the user confirmed the optimization result after inspecting the target software",
                   "user confirmation must be recorded separately from an uncertain automatic result");
  passed &= expect(!fixture.log.events.empty() &&
                       fixture.log.events.back().stage == "user-result-confirmation",
                   "user confirmation must use its own durable log stage");
  return passed;
}

[[nodiscard]] bool creation_withdrawal_contract() {
  Fixture fixture{make_plan("batch-create-withdrawal", 1)};
  bool passed = expect(fixture.service.restore().succeeded(),
                       "withdrawal batch must restore");
  fixture.withdrawals.authorizations.push_back(
      {.code = batch_app::WithdrawalAuthorizationCode::blocked,
       .notice_revision = 34,
       .reason = "emergency withdrawal before batch creation"});
  auto created = fixture.service.create(start_request("batch-create-withdrawal"));
  passed &= expect(created.code == batch_app::OptimizationBatchActionCode::blocked &&
                       !created.snapshot.active.has_value(),
                   "a current emergency withdrawal must prevent a new batch record");
  return passed;
}

[[nodiscard]] bool retry_and_close_recovery_contract() {
  Fixture fixture{make_plan("batch-retry-original", 1)};
  bool passed = expect(fixture.service.restore().succeeded(), "batch must restore");
  passed &= expect(fixture.service.create(start_request("batch-retry-original")).succeeded(),
                   "retry source batch must be created");
  fixture.executor.executions.push_back(
      {.code = batch_app::StepExecutionCode::failed, .detail = "injected failure"});
  passed &= expect(fixture.service.advance().succeeded(),
                   "a failed option must be recorded before retry");
  fixture.executor.verifications.push_back(
      {.code = batch_app::StepVerificationCode::not_optimized,
       .detail = "still retryable"});
  fixture.plans.next_plan = make_plan("batch-retry-new", 1);
  passed &= expect(fixture.service.retry_current(start_request("batch-retry-new")).succeeded(),
                   "retry must archive and release the original batch before creating a fresh plan");
  auto retried = fixture.service.snapshot();
  passed &= expect(retried.history.size() == 1 && retried.active.has_value() &&
                       retried.active->plan.batch_id == "batch-retry-new" &&
                       retried.active->plan.retry_of_batch_id == "batch-retry-original",
                   "retry must preserve immutable original history and link the fresh batch");

  fixture.executor.executions.push_back(
      {.code = batch_app::StepExecutionCode::confirmation_required,
       .detail = "interrupted capability"});
  passed &= expect(fixture.service.advance().succeeded(),
                   "new retry batch must reach a durable unknown result state");
  passed &= expect(fixture.service.request_close().succeeded(),
                   "normal close must persist a paused batch boundary");

  batch_app::SoftwareOptimizationBatchService recovered{
      fixture.states, fixture.occupancy, fixture.log, fixture.plans,
      fixture.executor, fixture.withdrawals};
  passed &= expect(recovered.restore().succeeded(),
                   "a later process must restore the persisted optimization batch");
  auto restored = recovered.snapshot();
  passed &= expect(restored.active.has_value() &&
                       restored.active->state == batch::OptimizationBatchState::recovery_required &&
                       restored.active->steps.front().state ==
                           batch::OptimizationStepState::result_confirmation_pending,
                   "close/restart must require read-only recovery rather than replaying a step");
  auto executions_before = fixture.executor.executed_ids.size();
  fixture.executor.verifications.push_back(
      {.code = batch_app::StepVerificationCode::confirmation_required,
       .detail = "still unknown"});
  passed &= expect(recovered.recover_read_only().succeeded(),
                   "recovery may observe an uncertain result without starting work");
  passed &= expect(fixture.executor.executed_ids.size() == executions_before &&
                       recovered.snapshot().active->state ==
                           batch::OptimizationBatchState::recovery_required,
                   "read-only recovery must neither execute nor auto-continue pending work");
  return passed;
}

[[nodiscard]] bool target_exit_restart_and_stop_contract() {
  Fixture exit_fixture{make_plan("batch-exit", 1, catalog::ExitRequirement::graceful_exit)};
  bool passed = expect(exit_fixture.service.restore().succeeded(), "exit batch must restore");
  passed &= expect(exit_fixture.service.create(start_request("batch-exit")).succeeded(),
                   "exit batch must create");
  passed &= expect(exit_fixture.service.advance().succeeded(),
                   "a graceful-exit scheme must first wait for target exit");
  exit_fixture.executor.target_exits.push_back(
      {.known = false, .detail = "unverified Windows target identity"});
  passed &= expect(exit_fixture.service.confirm_current_target_exit().code ==
                       batch_app::OptimizationBatchActionCode::blocked &&
                       exit_fixture.executor.executed_ids.empty(),
                   "unverified target exit must prevent configuration changes");

  Fixture restart_fixture{make_plan("batch-restart", 1, catalog::ExitRequirement::none,
                                    catalog::RestartRequirement::windows)};
  passed &= expect(restart_fixture.service.restore().succeeded(), "restart batch must restore");
  passed &= expect(restart_fixture.service.create(start_request("batch-restart")).succeeded(),
                   "restart batch must create");
  passed &= expect(restart_fixture.service.advance().succeeded(),
                   "restart-requiring option must execute through its verifier");
  auto waiting = restart_fixture.service.snapshot();
  passed &= expect(waiting.active.has_value() &&
                       waiting.active->state == batch::OptimizationBatchState::waiting_restart &&
                       waiting.active->steps.front().state ==
                           batch::OptimizationStepState::waiting_restart,
                   "restart requirement must persist a handoff rather than claiming completion");

  Fixture stop_fixture{make_plan("batch-stop", 2)};
  passed &= expect(stop_fixture.service.restore().succeeded(), "stop batch must restore");
  passed &= expect(stop_fixture.service.create(start_request("batch-stop")).succeeded(),
                   "stop batch must create");
  passed &= expect(stop_fixture.service.stop_current().succeeded(),
                   "stopping before an effect must safely retain unstarted work");
  auto stopped = stop_fixture.service.snapshot();
  passed &= expect(stopped.active.has_value() &&
                       stopped.active->state == batch::OptimizationBatchState::stopped &&
                       stopped.active->steps[0].state == batch::OptimizationStepState::not_executed &&
                       stopped.active->steps[1].state == batch::OptimizationStepState::not_executed,
                   "normal stop must not claim unstarted options were executed");
  return passed;
}

[[nodiscard]] bool close_recovery_and_force_controls_contract() {
  Fixture closing_fixture{make_plan("batch-close-in-flight", 2)};
  bool passed = expect(closing_fixture.service.restore().succeeded(),
                       "in-flight close batch must restore");
  passed &= expect(closing_fixture.service.create(
                       start_request("batch-close-in-flight"))
                       .succeeded(),
                   "in-flight close batch must create");
  closing_fixture.executor.block_execution = true;
  std::thread advancing_close{[&] {
    static_cast<void>(closing_fixture.service.advance());
  }};
  passed &= expect(closing_fixture.executor.wait_for_execution(),
                   "normal close must observe an in-flight controlled effect");
  auto closing = std::async(std::launch::async, [&] {
    return closing_fixture.service.request_close();
  });
  passed &= expect(closing_fixture.log.wait_for_stage("normal-close-requested"),
                   "normal close must persist its boundary before waiting");
  passed &= expect(closing.wait_for(std::chrono::milliseconds{0}) ==
                       std::future_status::timeout,
                   "normal close must wait for the in-flight effect to reach a safe boundary");
  closing_fixture.executor.release_blocked_execution();
  advancing_close.join();
  auto close_result = closing.get();
  auto closed_in_flight = closing_fixture.service.snapshot();
  passed &= expect(close_result.succeeded() && closed_in_flight.active.has_value() &&
                       closed_in_flight.active->state ==
                           batch::OptimizationBatchState::closing &&
                       closed_in_flight.active->steps.front().state ==
                           batch::OptimizationStepState::optimized &&
                       closed_in_flight.active->steps[1].state ==
                           batch::OptimizationStepState::pending,
                   "normal close must retain the observed current result and pause later work");

  Fixture close_fixture{make_plan("batch-close-continue", 1)};
  passed &= expect(close_fixture.service.restore().succeeded(),
                   "close recovery batch must restore");
  passed &= expect(close_fixture.service.create(
                       start_request("batch-close-continue"))
                       .succeeded(),
                   "close recovery batch must create");
  passed &= expect(close_fixture.service.request_close().succeeded(),
                   "normal close must persist a pending batch without executing it");
  batch_app::SoftwareOptimizationBatchService recovered{
      close_fixture.states, close_fixture.occupancy, close_fixture.log,
      close_fixture.plans, close_fixture.executor, close_fixture.withdrawals};
  passed &= expect(recovered.restore().succeeded() &&
                       close_fixture.executor.executed_ids.empty(),
                   "restoring a closed pending batch must not start an effect");
  passed &= expect(recovered.continue_after_recovery().succeeded(),
                   "a user may explicitly continue a normally closed batch");
  passed &= expect(recovered.advance().succeeded() &&
                       close_fixture.executor.executed_ids ==
                           std::vector<std::string>{"option-1"},
                   "only explicit continuation may start the recovered pending step");

  Fixture force_close_fixture{
      make_plan("batch-force-close", 1, catalog::ExitRequirement::graceful_exit)};
  passed &= expect(force_close_fixture.service.restore().succeeded(),
                   "force-close batch must restore");
  passed &= expect(force_close_fixture.service.create(
                       start_request("batch-force-close"))
                       .succeeded(),
                   "force-close batch must create");
  passed &= expect(force_close_fixture.service.advance().succeeded(),
                   "force-close batch must wait for the target exit first");
  passed &= expect(force_close_fixture.service.request_force_close().succeeded(),
                   "force-close must first persist an independent confirmation request");
  force_close_fixture.executor.force_closes.push_back(
      {.known = true, .exited = true, .detail = "controlled target closed"});
  passed &= expect(force_close_fixture.service.confirm_force_close().succeeded(),
                   "force-close confirmation must require a verified target exit");
  auto forced_closed = force_close_fixture.service.snapshot();
  passed &= expect(forced_closed.active.has_value() &&
                       forced_closed.active->steps.front().force_close_confirmation_requested &&
                       forced_closed.active->steps.front().force_close_completed &&
                       forced_closed.active->steps.front().target_exit_confirmed,
                   "force-close request and verified result must both remain in history");

  Fixture force_termination_fixture{make_plan("batch-force-termination", 1)};
  passed &= expect(force_termination_fixture.service.restore().succeeded(),
                   "force-termination batch must restore");
  passed &= expect(force_termination_fixture.service.create(
                       start_request("batch-force-termination"))
                       .succeeded(),
                   "force-termination batch must create");
  force_termination_fixture.executor.block_execution = true;
  std::optional<batch_app::OptimizationBatchActionResult> advance_result;
  std::thread advancing{[&] {
    advance_result = force_termination_fixture.service.advance();
  }};
  passed &= expect(force_termination_fixture.executor.wait_for_execution(),
                   "the controlled execution must become observable before termination");
  passed &= expect(
      force_termination_fixture.service.request_force_termination().code ==
          batch_app::OptimizationBatchActionCode::confirmation_required,
      "force termination must persist its own risk confirmation boundary");
  force_termination_fixture.executor.force_terminations.push_back(
      {.known = true, .terminated = true, .detail = "controlled optimization terminated"});
  passed &= expect(
      force_termination_fixture.service.confirm_force_termination().code ==
          batch_app::OptimizationBatchActionCode::confirmation_required,
      "confirmed force termination must leave the optimization result pending");
  force_termination_fixture.executor.release_blocked_execution();
  advancing.join();
  auto terminated = force_termination_fixture.service.snapshot();
  passed &= expect(advance_result.has_value() &&
                       advance_result->code ==
                           batch_app::OptimizationBatchActionCode::confirmation_required &&
                       terminated.active.has_value() &&
                       terminated.active->steps.front().force_termination_confirmation_requested &&
                       terminated.active->steps.front().force_termination_completed &&
                       terminated.active->steps.front().state ==
                           batch::OptimizationStepState::result_confirmation_pending,
                   "a late executor result must not overwrite the forced-termination boundary");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= frozen_serial_and_shared_occupancy_contract();
  passed &= withdrawal_and_unknown_result_contract();
  passed &= creation_withdrawal_contract();
  passed &= retry_and_close_recovery_contract();
  passed &= target_exit_restart_and_stop_contract();
  passed &= close_recovery_and_force_controls_contract();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
