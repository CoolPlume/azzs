#include <cstdlib>
#include <iostream>
#include <system_error>
#include <utility>

#include "azzs/application/startup_assembly_status.hpp"

namespace {

using azzs::application::startup::EmergencyPreflightStartState;
using azzs::application::startup::StartupAssemblyStage;
using azzs::application::startup::StartupDiagnosticAvailability;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool device_data_failure_contract() {
  auto status = azzs::application::startup::startup_assembly_failed(
      StartupAssemblyStage::device_data_environment);

  return expect(!status.workbench_ready,
                "device-data failure must not report a ready workbench") &&
         expect(status.failure.has_value(),
                "device-data failure must return a typed failure") &&
         expect(status.failure->stage ==
                    StartupAssemblyStage::device_data_environment,
                "device-data failure must preserve its startup stage") &&
         expect(status.failure->retry_on_next_process_start,
                "startup failure must defer retry to the next process start") &&
         expect(status.failure->diagnostic_availability ==
                    StartupDiagnosticAvailability::unavailable,
                "pre-service device-data failure must not claim diagnostics") &&
         expect(status.failure->public_statement.find(L"应用数据") !=
                    std::wstring::npos,
                "device-data failure must use a redacted public statement");
}

[[nodiscard]] bool preflight_thread_failure_contract() {
  auto const unavailable =
      std::make_error_code(std::errc::resource_unavailable_try_again);
  auto not_started =
      azzs::application::startup::start_emergency_preflight([&] {
        throw std::system_error(unavailable, "thread creation");
      });
  bool launched = false;
  auto started =
      azzs::application::startup::start_emergency_preflight([&] {
        launched = true;
      });

  return expect(not_started.state ==
                    EmergencyPreflightStartState::
                        not_started_thread_creation_failed,
                "thread creation failure must remain observable") &&
         expect(!not_started.started() &&
                    not_started.system_error_code == unavailable.value(),
                "preflight failure must be nonblocking and preserve its code") &&
         expect(started.state == EmergencyPreflightStartState::started &&
                    started.started() && launched,
                "a successful launcher must report an observable started state");
}

[[nodiscard]] bool main_window_failure_contract() {
  auto status = azzs::application::startup::startup_assembly_failed(
      StartupAssemblyStage::main_window_navigation,
      StartupDiagnosticAvailability::available);

  return expect(!status.workbench_ready && status.failure.has_value(),
                "a WinRT stage failure must not report a ready workbench") &&
         expect(status.failure->stage ==
                    StartupAssemblyStage::main_window_navigation,
                "the failed WinRT stage must remain inspectable") &&
         expect(status.failure->retry_on_next_process_start &&
                    status.failure->diagnostic_availability ==
                        StartupDiagnosticAvailability::available,
                "post-service failure must expose next-launch retry and existing diagnostics") &&
         expect(status.emergency_preflight.state ==
                    EmergencyPreflightStartState::not_attempted,
                "a main-window failure must not launch preflight") &&
         expect(status.failure->stage !=
                    StartupAssemblyStage::device_data_environment,
                "WinRT failure must not be mislabeled as a device-data failure");
}

[[nodiscard]] bool unexpected_exception_contract() {
  auto status = azzs::application::startup::startup_assembly_failed(
      StartupAssemblyStage::unexpected_assembly_exception);

  return expect(!status.workbench_ready && status.failure.has_value(),
                "an unexpected assembly exception must block normal startup") &&
         expect(status.failure->stage ==
                    StartupAssemblyStage::unexpected_assembly_exception,
                "an unexpected assembly exception must remain typed") &&
         expect(status.emergency_preflight.state ==
                    EmergencyPreflightStartState::not_attempted,
                "an unexpected assembly exception must not claim preflight");
}

[[nodiscard]] bool core_record_readability_contract() {
  auto status = azzs::application::startup::startup_assembly_failed(
      StartupAssemblyStage::core_records_unreadable,
      StartupDiagnosticAvailability::unavailable);

  return expect(!status.workbench_ready && status.failure.has_value(),
                "unreadable core records must block normal workbench startup") &&
         expect(status.failure->stage ==
                    StartupAssemblyStage::core_records_unreadable,
                "core-record gate must remain a distinct startup stage") &&
         expect(status.failure->diagnostic_availability ==
                    StartupDiagnosticAvailability::unavailable &&
                    status.emergency_preflight.state ==
                        EmergencyPreflightStartState::not_attempted,
                "unreadable records must not claim diagnostics or launch preflight");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= device_data_failure_contract();
  passed &= preflight_thread_failure_contract();
  passed &= main_window_failure_contract();
  passed &= unexpected_exception_contract();
  passed &= core_record_readability_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "startup assembly contract passed\n";
  return EXIT_SUCCESS;
}
