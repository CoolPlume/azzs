#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/driver_acquisition.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/restart_resume.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace drivers = azzs::application::driver_acquisition;
namespace restart = azzs::application::restart_resume;

using azzs::application::CorrelationId;
using azzs::application::DeviceStateStore;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::HardwareObservation;
using azzs::application::HardwareObservationCode;
using azzs::application::HardwareObservationResult;
using azzs::application::HardwareDeviceKind;
using azzs::application::HardwareDevicePhysicality;
using azzs::application::HardwareDeviceStatus;
using azzs::application::HardwareObservationConfidence;
using azzs::application::HardwareObservationSource;
using azzs::application::HardwareVendor;
using azzs::application::HardwareObserver;
using azzs::application::HardwareOverviewService;
using azzs::application::HardwareOverviewState;
using azzs::application::StateFileSlot;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::StateFileOperation;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "driver acquisition contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return {.value = "driver-contract-" + std::to_string(++next_correlation)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events.push_back(event);
    return {.persisted = true, .sequence = events.size()};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    return {.cleared = true};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {};
  }

  int next_correlation{0};
  std::vector<ExecutionEvent> events;
};

class FixtureHardwareObserver final : public HardwareObserver {
 public:
  [[nodiscard]] HardwareObservationResult observe(std::stop_token) override {
    ++observe_calls;
    HardwareObservation observation{
        .cpu = "AMD Ryzen 7",
        .gpu = "NVIDIA GeForce RTX",
        .motherboard = "ASUS PRIME",
        .network_adapter = "Intel Ethernet",
        .oem_model = "Dell Desktop",
        .oem_vendor = HardwareVendor::dell,
    };
    observation.devices = {
        {.kind = HardwareDeviceKind::cpu,
         .name = observation.cpu,
         .physicality = HardwareDevicePhysicality::confirmed_physical,
         .source = HardwareObservationSource::wmi,
         .confidence = HardwareObservationConfidence::confirmed,
         .status = HardwareDeviceStatus::enabled,
         .vendor = HardwareVendor::amd,
         .physically_present = true,
         .filter_reason = "contract fixture"},
        {.kind = HardwareDeviceKind::gpu,
         .name = observation.gpu,
         .physicality = HardwareDevicePhysicality::confirmed_physical,
         .source = HardwareObservationSource::wmi,
         .confidence = HardwareObservationConfidence::confirmed,
         .status = HardwareDeviceStatus::enabled,
         .vendor = HardwareVendor::nvidia,
         .physically_present = true,
         .filter_reason = "contract fixture"},
        {.kind = HardwareDeviceKind::motherboard,
         .name = observation.motherboard,
         .physicality = HardwareDevicePhysicality::confirmed_physical,
         .source = HardwareObservationSource::wmi,
         .confidence = HardwareObservationConfidence::confirmed,
         .status = HardwareDeviceStatus::enabled,
         .vendor = HardwareVendor::asus,
         .physically_present = true,
         .filter_reason = "contract fixture"},
        {.kind = HardwareDeviceKind::network_adapter,
         .name = observation.network_adapter,
         .physicality = HardwareDevicePhysicality::confirmed_physical,
         .source = HardwareObservationSource::wmi,
         .confidence = HardwareObservationConfidence::confirmed,
         .status = HardwareDeviceStatus::enabled,
         .vendor = HardwareVendor::intel,
         .physically_present = true,
         .filter_reason = "contract fixture"},
    };
    return {.code = HardwareObservationCode::succeeded,
            .observation = std::move(observation)};
  }

  std::size_t observe_calls{0};
};

class FixturePlatform final : public drivers::DriverHandoffPlatform {
 public:
  struct Request final {
    drivers::DriverEntrypoint entrypoint;
    drivers::DriverAssistantAction action;
  };

  struct RescueFolderRequest final {
    drivers::RescueToolTarget target;
  };

  [[nodiscard]] bool assistant_installed() const noexcept override {
    return assistant_present;
  }

  [[nodiscard]] bool open(drivers::DriverEntrypoint entrypoint,
                          drivers::DriverAssistantAction action,
                          std::string& error) override {
    requests.push_back({.entrypoint = entrypoint, .action = action});
    if (fail_open) {
      error = "injected fixed-entrypoint launch failure";
      return false;
    }
    return true;
  }

  [[nodiscard]] bool open_rescue_folder(drivers::RescueToolTarget target,
                                        std::string& error) override {
    rescue_folder_requests.push_back({.target = target});
    if (fail_rescue_folder_open) {
      error = "injected fixed rescue folder open failure";
      return false;
    }
    return true;
  }

  bool assistant_present{false};
  bool fail_open{false};
  bool fail_rescue_folder_open{false};
  std::vector<Request> requests;
  std::vector<RescueFolderRequest> rescue_folder_requests;
};

class FixtureNetwork final : public drivers::DriverNetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override { return available_now; }

  bool available_now{true};
};

class FixtureRegistration final : public restart::LoginResumeRegistration {
 public:
  [[nodiscard]] restart::LoginResumeRegistrationResult register_once() override {
    ++register_calls;
    if (fail_register) {
      return {.code = restart::LoginResumeRegistrationCode::failed,
              .detail = "injected RunOnce registration failure"};
    }
    return {.code = restart::LoginResumeRegistrationCode::registered};
  }

  [[nodiscard]] restart::LoginResumeRegistrationResult clear_once() override {
    ++clear_calls;
    return {.code = restart::LoginResumeRegistrationCode::cleared};
  }

  int register_calls{0};
  int clear_calls{0};
  bool fail_register{false};
};

struct Fixture final {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{}};
  DeviceStateStore states{files, clock};
  FixtureHardwareObserver hardware_observer;
  HardwareOverviewService hardware{hardware_observer, clock};
  FixturePlatform platform;
  FixtureNetwork network;
  RecordingLog log;
  FixtureRegistration registration;
  restart::RestartResumeService restart_resume{states, registration};
  drivers::DriverAcquisitionService service{states, hardware, platform, network,
                                             log, restart_resume};

  [[nodiscard]] bool restore() {
    return restart_resume.restore().succeeded() && service.restore().succeeded();
  }
};

[[nodiscard]] bool has_field(ExecutionEvent const& event, std::string const& key,
                             std::string const& value) {
  return std::ranges::any_of(event.fields, [&](auto const& field) {
    return field.key == key && field.value == value;
  });
}

[[nodiscard]] bool external_completion_is_observed_but_never_verified() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the driver fixture must restore");
  auto started = fixture.service.begin_external_handoff(
      drivers::DriverEntrypoint::amd_software);
  passed &= expect(started.succeeded() &&
                       started.snapshot.state ==
                           drivers::DriverAcquisitionState::handoff_in_progress &&
                       fixture.platform.requests.size() == 1 &&
                       fixture.platform.requests.front().action ==
                           drivers::DriverAssistantAction::install,
                   "a pre-persisted AMD handoff must open only its fixed install page");

  drivers::DriverAcquisitionService reopened{
      fixture.states, fixture.hardware, fixture.platform, fixture.network,
      fixture.log, fixture.restart_resume};
  auto restored = reopened.restore();
  passed &= expect(restored.succeeded() &&
                       restored.snapshot.state ==
                           drivers::DriverAcquisitionState::awaiting_user_decision,
                   "a later launch must require an explicit user result decision");
  auto completed = reopened.decide(drivers::DriverHandoffDecision::completed_externally);
  passed &= expect(completed.succeeded() && completed.refreshed_hardware.has_value() &&
                       completed.snapshot.state == drivers::DriverAcquisitionState::ready &&
                       completed.snapshot.last_observation.has_value() &&
                       completed.snapshot.last_observation->network_available &&
                       fixture.hardware_observer.observe_calls == 1,
                   "external completion must re-observe hardware and network without a driver claim");
  auto const confirmation = std::ranges::find_if(
      fixture.log.events, [](ExecutionEvent const& event) {
        return event.stage == "user-result-confirmation";
      });
  passed &= expect(confirmation != fixture.log.events.end() &&
                       confirmation->result == azzs::application::ExecutionResult::unknown &&
                       has_field(*confirmation, "driver_success", "unverified"),
                   "the completion record must explicitly remain an unverified observation");
  return passed;
}

[[nodiscard]] bool recommendations_and_fixed_pages_remain_independent() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the recommendation fixture must restore");
  static_cast<void>(fixture.hardware.refresh());
  auto snapshot = fixture.service.snapshot();
  passed &= expect(std::ranges::find(snapshot.recommended_entrypoints,
                                     drivers::DriverEntrypoint::amd_software) !=
                       snapshot.recommended_entrypoints.end() &&
                       std::ranges::find(snapshot.recommended_entrypoints,
                                         drivers::DriverEntrypoint::nvidia_drivers) !=
                           snapshot.recommended_entrypoints.end() &&
                       std::ranges::find(snapshot.recommended_entrypoints,
                                         drivers::DriverEntrypoint::dell_support) !=
                           snapshot.recommended_entrypoints.end(),
                   "recognized hardware must recommend matching fixed entrypoints");
  auto started = fixture.service.begin_external_handoff(
      drivers::DriverEntrypoint::nvidia_drivers);
  return passed && expect(started.succeeded() &&
                              fixture.platform.requests.size() == 1 &&
                              fixture.platform.requests.front().action ==
                                  drivers::DriverAssistantAction::open_page,
                          "a fixed vendor page must remain available without a package match");
}

[[nodiscard]] azzs::domain::StateBytes legacy_driver_handoff_payload() {
  return {
      std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
      std::byte{'D'}, std::byte{'R'}, std::byte{'V'}, std::byte{'1'},
      std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{static_cast<unsigned char>(
          drivers::DriverAcquisitionState::handoff_in_progress)},
      std::byte{static_cast<unsigned char>(
          drivers::DriverEntrypoint::intel_driver_assistant)},
      std::byte{0},
  };
}

[[nodiscard]] bool fixed_rescue_folders_require_an_explicit_return_and_never_claim_success() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the rescue-folder fixture must restore");
  fixture.network.available_now = false;
  auto opened = fixture.service.begin_external_rescue_handoff(
      drivers::RescueToolTarget::generic_network_driver);
  passed &= expect(
      opened.succeeded() &&
          opened.snapshot.state ==
              drivers::DriverAcquisitionState::handoff_in_progress &&
          fixture.platform.requests.empty() &&
          fixture.platform.rescue_folder_requests.size() == 1 &&
          fixture.platform.rescue_folder_requests.front().target ==
              drivers::RescueToolTarget::generic_network_driver,
      "a rescue handoff must open only its fixed directory, never a driver entrypoint");
  passed &= expect(
      fixture.service.external_flow_returned().succeeded() &&
          fixture.service.snapshot().state ==
              drivers::DriverAcquisitionState::awaiting_user_decision,
      "a rescue folder requires an explicit external-return boundary before a decision");

  auto completed = fixture.service.decide(
      drivers::DriverHandoffDecision::completed_externally);
  passed &= expect(
      completed.succeeded() && completed.refreshed_hardware.has_value() &&
          completed.snapshot.state == drivers::DriverAcquisitionState::ready &&
          completed.snapshot.last_observation.has_value() &&
          !completed.snapshot.last_observation->network_available &&
          fixture.hardware_observer.observe_calls == 1,
      "the user decision must re-observe hardware and network without trusting an external executable");

  auto const confirmation = std::ranges::find_if(
      fixture.log.events, [](ExecutionEvent const& event) {
        return event.stage == "user-result-confirmation";
      });
  return passed && expect(
      confirmation != fixture.log.events.end() &&
          confirmation->result == azzs::application::ExecutionResult::unknown &&
          has_field(*confirmation, "rescue_target", "generic-network-driver") &&
          has_field(*confirmation, "driver_success", "unverified"),
      "rescue logging must record the handoff and user decision, not a repair success");
}

[[nodiscard]] bool rescue_handoff_restores_to_the_same_explicit_decision_boundary() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the rescued-reopen fixture must restore");
  passed &= expect(
      fixture.service
          .begin_external_rescue_handoff(
              drivers::RescueToolTarget::offline_network_diagnostics)
          .succeeded(),
      "a fixed rescue folder must persist before the external handoff begins");

  drivers::DriverAcquisitionService reopened{
      fixture.states, fixture.hardware, fixture.platform, fixture.network,
      fixture.log, fixture.restart_resume};
  auto const restored = reopened.restore();
  auto const return_boundary = std::ranges::find_if(
      fixture.log.events, [](ExecutionEvent const& event) {
        return event.stage == "restore-return-boundary";
      });
  return passed && expect(
      restored.succeeded() &&
          restored.snapshot.state ==
              drivers::DriverAcquisitionState::awaiting_user_decision &&
          restored.snapshot.active_entrypoint == std::nullopt &&
          restored.snapshot.active_rescue_target ==
              drivers::RescueToolTarget::offline_network_diagnostics &&
          return_boundary != fixture.log.events.end() &&
          has_field(*return_boundary, "rescue_target", "offline-network-diagnostics"),
      "a reopened rescue handoff must retain its fixed target and require the same explicit decision");
}

[[nodiscard]] bool rescue_restore_persistence_failure_keeps_the_target_in_diagnostics() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the rescue restore-failure fixture must restore");
  passed &= expect(
      fixture.service
          .begin_external_rescue_handoff(
              drivers::RescueToolTarget::offline_network_diagnostics)
          .succeeded(),
      "a rescue handoff must persist before the later restore failure");

  drivers::DriverAcquisitionService reopened{
      fixture.states, fixture.hardware, fixture.platform, fixture.network,
      fixture.log, fixture.restart_resume};
  fixture.files.fail_next(StateFileOperation::write, StateFileSlot::candidate,
                          "injected rescue return-boundary persistence failure");
  auto const restored = reopened.restore();
  auto const return_boundary = std::ranges::find_if(
      fixture.log.events, [](ExecutionEvent const& event) {
        return event.stage == "restore-return-boundary";
      });
  return passed && expect(
                       restored.code == drivers::DriverActionCode::persistence_failed &&
                           restored.snapshot.state ==
                               drivers::DriverAcquisitionState::read_only &&
                           return_boundary != fixture.log.events.end() &&
                           has_field(*return_boundary, "rescue_target",
                                     "offline-network-diagnostics"),
                       "a failed rescue return-boundary persistence must retain its target in diagnostics");
}

[[nodiscard]] bool legacy_driver_handoff_state_remains_readable() {
  Fixture fixture;
  bool passed = expect(fixture.restart_resume.restore().succeeded(),
                       "the legacy-driver fixture restart state must restore");
  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"driver-acquisition"});
  auto seeded = fixture.states.initialize(
      key, {.value = {.schema = 2,
                      .minimum_reader = 1,
                      .minimum_writer = 2,
                      .payload = legacy_driver_handoff_payload()}});
  passed &= expect(seeded.status == azzs::application::StateCommitStatus::committed,
                   "the legacy driver handoff fixture must seed durable state");

  drivers::DriverAcquisitionService reopened{
      fixture.states, fixture.hardware, fixture.platform, fixture.network,
      fixture.log, fixture.restart_resume};
  auto const restored = reopened.restore();
  auto const stored = fixture.states.inspect(key);
  return passed && expect(
      restored.succeeded() &&
          restored.snapshot.state ==
              drivers::DriverAcquisitionState::awaiting_user_decision &&
          restored.snapshot.active_entrypoint ==
              drivers::DriverEntrypoint::intel_driver_assistant &&
          !restored.snapshot.active_rescue_target.has_value() &&
          stored.snapshot.has_value() &&
          stored.snapshot->state.value.payload.size() > 8 &&
          std::to_integer<unsigned char>(stored.snapshot->state.value.payload[8]) == 2,
      "legacy driver handoff state must restore through the same decision boundary and migrate to format v2");
}

[[nodiscard]] bool restart_choice_uses_the_existing_barrier_only() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the restart fixture must restore");
  passed &= expect(fixture.service
                       .begin_external_handoff(drivers::DriverEntrypoint::intel_driver_assistant)
                       .succeeded() &&
                       fixture.service.external_flow_returned().succeeded(),
                   "restart setup must first establish an external-return decision");
  auto waiting = fixture.service.decide(drivers::DriverHandoffDecision::restart_required);
  passed &= expect(waiting.succeeded() &&
                       waiting.snapshot.state ==
                           drivers::DriverAcquisitionState::waiting_for_restart &&
                       fixture.restart_resume.snapshot().state ==
                           restart::RestartResumeState::waiting_for_windows_restart &&
                       fixture.registration.register_calls == 1,
                   "only the explicit restart decision may arm the shared barrier");
  passed &= expect(fixture.restart_resume.resume_after_login().succeeded() &&
                       fixture.service.recover_after_restart().succeeded() &&
                       fixture.restart_resume.complete_read_only_verification().succeeded(),
                   "driver recovery must remain read-only until the shared barrier permits a decision");
  auto skipped = fixture.service.decide(drivers::DriverHandoffDecision::skip_for_now);
  return passed && expect(skipped.succeeded() &&
                              fixture.restart_resume.snapshot().state ==
                                  restart::RestartResumeState::idle,
                          "an explicit skip may clear only the driver-owned barrier");
}

[[nodiscard]] bool failed_external_launch_cannot_offer_a_return_decision() {
  Fixture fixture;
  fixture.platform.fail_open = true;
  bool passed = expect(fixture.restore(), "the failed-launch fixture must restore");
  auto failed = fixture.service.begin_external_handoff(
      drivers::DriverEntrypoint::amd_software);
  passed &= expect(failed.code == drivers::DriverActionCode::launcher_failed &&
                       failed.snapshot.state == drivers::DriverAcquisitionState::ready &&
                       !failed.snapshot.detail.empty(),
                   "a failed external launch must close the handoff before projection");
  return passed && expect(fixture.service.external_flow_returned().code ==
                              drivers::DriverActionCode::rejected,
                          "a failed external launch must not expose a return decision");
}

[[nodiscard]] bool failed_restart_arm_rolls_back_the_driver_handoff() {
  Fixture fixture;
  fixture.registration.fail_register = true;
  bool passed = expect(fixture.restore(), "the restart-failure fixture must restore");
  passed &= expect(fixture.service
                       .begin_external_handoff(drivers::DriverEntrypoint::intel_driver_assistant)
                       .succeeded() &&
                       fixture.service.external_flow_returned().succeeded(),
                   "restart-failure setup must reach the external-return decision");
  auto failed = fixture.service.decide(drivers::DriverHandoffDecision::restart_required);
  return passed && expect(
      failed.code == drivers::DriverActionCode::restart_barrier_failed &&
          failed.snapshot.state == drivers::DriverAcquisitionState::awaiting_user_decision &&
          fixture.restart_resume.snapshot().state == restart::RestartResumeState::idle,
      "a failed restart arm must leave the driver and restart states at the same retry boundary");
}

[[nodiscard]] bool failed_driver_rollback_persistence_stays_fail_closed() {
  Fixture fixture;
  fixture.registration.fail_register = true;
  bool passed = expect(fixture.restore(), "the rollback-persistence fixture must restore");
  passed &= expect(fixture.service
                       .begin_external_handoff(drivers::DriverEntrypoint::intel_driver_assistant)
                       .succeeded() &&
                       fixture.service.external_flow_returned().succeeded(),
                   "rollback-persistence setup must reach the external-return decision");
  fixture.files.fail_on({.operation = StateFileOperation::write,
                         .slot = StateFileSlot::candidate,
                         .occurrence = 4,
                         .error = "injected driver rollback persistence failure"});
  auto failed = fixture.service.decide(drivers::DriverHandoffDecision::restart_required);
  passed &= expect(
      failed.code == drivers::DriverActionCode::persistence_failed &&
          failed.snapshot.state == drivers::DriverAcquisitionState::read_only &&
          fixture.restart_resume.snapshot().state == restart::RestartResumeState::idle,
      "a driver rollback persistence failure must close the service while the barrier is idle");

  drivers::DriverAcquisitionService reopened{
      fixture.states, fixture.hardware, fixture.platform, fixture.network,
      fixture.log, fixture.restart_resume};
  auto restored = reopened.restore();
  return passed && expect(
      restored.succeeded() &&
          restored.snapshot.state ==
              drivers::DriverAcquisitionState::awaiting_user_decision &&
          fixture.restart_resume.snapshot().state == restart::RestartResumeState::idle,
      "a later writable restore must repair a stale driver restart checkpoint only to a decision boundary");
}

[[nodiscard]] bool failed_decision_persistence_restores_pending_handoff() {
  Fixture entrypoint_fixture;
  bool passed = expect(entrypoint_fixture.restore(),
                       "the decision-persistence entrypoint fixture must restore");
  passed &= expect(
      entrypoint_fixture.service
              .begin_external_handoff(
                  drivers::DriverEntrypoint::intel_driver_assistant)
              .succeeded() &&
          entrypoint_fixture.service.external_flow_returned().succeeded(),
      "the decision-persistence entrypoint fixture must reach a decision boundary");
  entrypoint_fixture.files.fail_next(
      StateFileOperation::write, StateFileSlot::candidate,
      "injected driver skip persistence failure");
  auto const skipped_failure = entrypoint_fixture.service.decide(
      drivers::DriverHandoffDecision::skip_for_now);
  passed &= expect(
      skipped_failure.code == drivers::DriverActionCode::persistence_failed &&
          skipped_failure.snapshot.state ==
              drivers::DriverAcquisitionState::awaiting_user_decision &&
          skipped_failure.snapshot.active_entrypoint ==
              drivers::DriverEntrypoint::intel_driver_assistant &&
          !skipped_failure.snapshot.active_rescue_target.has_value() &&
          !skipped_failure.snapshot.last_observation.has_value(),
      "a failed skip persistence must retain the pending entrypoint decision");

  auto const restart_retry = entrypoint_fixture.service.decide(
      drivers::DriverHandoffDecision::restart_required);
  auto const restart_event = std::ranges::find_if(
      entrypoint_fixture.log.events, [](ExecutionEvent const& event) {
        return event.stage == "restart-barrier" &&
               event.result == azzs::application::ExecutionResult::started;
      });
  passed &= expect(
      restart_retry.succeeded() &&
          restart_retry.snapshot.state ==
              drivers::DriverAcquisitionState::waiting_for_restart &&
          restart_retry.snapshot.active_entrypoint ==
              drivers::DriverEntrypoint::intel_driver_assistant &&
          restart_event != entrypoint_fixture.log.events.end() &&
          has_field(*restart_event, "entrypoint", "intel-driver-assistant"),
      "a retry after failed skip persistence must retain the entrypoint for restart logging");

  Fixture rescue_fixture;
  passed &= expect(rescue_fixture.restore(),
                   "the decision-persistence rescue fixture must restore");
  passed &= expect(
      rescue_fixture.service
              .begin_external_rescue_handoff(
                  drivers::RescueToolTarget::offline_network_diagnostics)
              .succeeded() &&
          rescue_fixture.service.external_flow_returned().succeeded(),
      "the decision-persistence rescue fixture must reach a decision boundary");
  rescue_fixture.files.fail_next(
      StateFileOperation::write, StateFileSlot::candidate,
      "injected driver completion persistence failure");
  auto const completion_failure = rescue_fixture.service.decide(
      drivers::DriverHandoffDecision::completed_externally);
  passed &= expect(
      completion_failure.code == drivers::DriverActionCode::persistence_failed &&
          completion_failure.snapshot.state ==
              drivers::DriverAcquisitionState::awaiting_user_decision &&
          !completion_failure.snapshot.active_entrypoint.has_value() &&
          completion_failure.snapshot.active_rescue_target ==
              drivers::RescueToolTarget::offline_network_diagnostics &&
          !completion_failure.snapshot.last_observation.has_value() &&
          rescue_fixture.hardware_observer.observe_calls == 1,
      "a failed completion persistence must discard its uncommitted observation and retain the rescue target");

  auto const skip_retry = rescue_fixture.service.decide(
      drivers::DriverHandoffDecision::skip_for_now);
  auto const confirmation = std::ranges::find_if(
      rescue_fixture.log.events, [](ExecutionEvent const& event) {
        return event.stage == "user-result-confirmation" &&
               event.result == azzs::application::ExecutionResult::cancelled;
      });
  return passed && expect(
                       skip_retry.succeeded() &&
                           skip_retry.snapshot.state ==
                               drivers::DriverAcquisitionState::ready &&
                           !skip_retry.snapshot.last_observation.has_value() &&
                           confirmation != rescue_fixture.log.events.end() &&
                           has_field(*confirmation, "rescue_target",
                                     "offline-network-diagnostics"),
                       "a retry after failed completion persistence must retain the rescue target without committing its observation");
}

[[nodiscard]] bool unsupported_driver_values_are_rejected() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the unsupported-value fixture must restore");
  auto const invalid_entrypoint =
      static_cast<drivers::DriverEntrypoint>(999);
  auto const invalid_start =
      fixture.service.begin_external_handoff(invalid_entrypoint);
  passed &= expect(
      invalid_start.code == drivers::DriverActionCode::rejected &&
          invalid_start.snapshot.state == drivers::DriverAcquisitionState::ready &&
          fixture.platform.requests.empty(),
      "an unsupported entrypoint must not reach the platform adapter");
  auto const invalid_rescue_target =
      static_cast<drivers::RescueToolTarget>(999);
  auto const invalid_rescue_start =
      fixture.service.begin_external_rescue_handoff(invalid_rescue_target);
  passed &= expect(
      invalid_rescue_start.code == drivers::DriverActionCode::rejected &&
          invalid_rescue_start.snapshot.state ==
              drivers::DriverAcquisitionState::ready &&
          fixture.platform.rescue_folder_requests.empty(),
      "an unsupported rescue target must not reach the platform adapter");
  passed &= expect(fixture.service
                       .begin_external_handoff(drivers::DriverEntrypoint::intel_driver_assistant)
                       .succeeded() &&
                       fixture.service.external_flow_returned().succeeded(),
                   "unsupported-decision setup must reach the external-return decision");
  auto const invalid_decision =
      fixture.service.decide(static_cast<drivers::DriverHandoffDecision>(999));
  return passed && expect(
      invalid_decision.code == drivers::DriverActionCode::rejected &&
          invalid_decision.snapshot.state ==
              drivers::DriverAcquisitionState::awaiting_user_decision,
      "an unsupported decision must not resolve an external handoff");
}

}  // namespace

int main() {
  return external_completion_is_observed_but_never_verified() &&
                 recommendations_and_fixed_pages_remain_independent() &&
                 fixed_rescue_folders_require_an_explicit_return_and_never_claim_success() &&
                 rescue_handoff_restores_to_the_same_explicit_decision_boundary() &&
                 rescue_restore_persistence_failure_keeps_the_target_in_diagnostics() &&
                 legacy_driver_handoff_state_remains_readable() &&
                 restart_choice_uses_the_existing_barrier_only() &&
                 failed_external_launch_cannot_offer_a_return_decision() &&
                 failed_restart_arm_rolls_back_the_driver_handoff() &&
                 failed_driver_rollback_persistence_stays_fail_closed() &&
                 failed_decision_persistence_restores_pending_handoff() &&
                 unsupported_driver_values_are_rejected()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
