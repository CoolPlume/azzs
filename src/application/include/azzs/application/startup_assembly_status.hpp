#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace azzs::application::startup {

enum class StartupAssemblyStage {
  device_data_environment,
  core_records_unreadable,
  main_window_initialization,
  main_window_binding,
  main_window_navigation,
  main_window_activation,
};

enum class StartupDiagnosticAvailability {
  unavailable,
  available,
};

struct StartupAssemblyFailure final {
  StartupAssemblyStage stage{StartupAssemblyStage::device_data_environment};
  bool retry_on_next_process_start{true};
  StartupDiagnosticAvailability diagnostic_availability{
      StartupDiagnosticAvailability::unavailable};
  // This is the only text allowed into the startup failure window. It must not
  // be populated from platform error text, paths, identities, or HRESULT data.
  std::wstring public_statement;
};

enum class EmergencyPreflightStartState {
  not_attempted,
  started,
  not_started_thread_creation_failed,
};

struct EmergencyPreflightStartResult final {
  EmergencyPreflightStartState state{
      EmergencyPreflightStartState::not_attempted};
  std::int32_t system_error_code{0};

  [[nodiscard]] bool started() const noexcept {
    return state == EmergencyPreflightStartState::started;
  }
};

struct StartupAssemblyStatus final {
  bool workbench_ready{false};
  std::optional<StartupAssemblyFailure> failure;
  EmergencyPreflightStartResult emergency_preflight;
};

[[nodiscard]] inline StartupAssemblyStatus startup_assembly_failed(
    StartupAssemblyStage stage,
    StartupDiagnosticAvailability diagnostic_availability =
        StartupDiagnosticAvailability::unavailable,
    EmergencyPreflightStartResult emergency_preflight = {}) {
  return {.failure = StartupAssemblyFailure{
              .stage = stage,
              .retry_on_next_process_start = true,
              .diagnostic_availability = diagnostic_availability,
              .public_statement =
                  stage == StartupAssemblyStage::device_data_environment
                      ? L"应用数据尚未准备完成。请关闭后重新打开应用。"
                      : L"工作台尚未完成启动。请关闭后重新打开应用。"},
          .emergency_preflight = std::move(emergency_preflight)};
}

[[nodiscard]] inline StartupAssemblyStatus startup_assembly_ready(
    EmergencyPreflightStartResult emergency_preflight) {
  return {.workbench_ready = true,
          .emergency_preflight = std::move(emergency_preflight)};
}

template <typename Starter>
[[nodiscard]] EmergencyPreflightStartResult start_emergency_preflight(
    Starter&& starter) {
  try {
    std::forward<Starter>(starter)();
    return {.state = EmergencyPreflightStartState::started};
  } catch (std::system_error const& error) {
    return {.state =
                EmergencyPreflightStartState::not_started_thread_creation_failed,
            .system_error_code = error.code().value()};
  }
}

}  // namespace azzs::application::startup
