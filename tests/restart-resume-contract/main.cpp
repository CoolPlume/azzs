#include <cstdlib>
#include <iostream>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/restart_resume.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

using azzs::application::DeviceStateStore;
using azzs::application::restart_resume::LoginResumeRegistration;
using azzs::application::restart_resume::LoginResumeRegistrationCode;
using azzs::application::restart_resume::LoginResumeRegistrationResult;
using azzs::application::restart_resume::RestartResumeActionCode;
using azzs::application::restart_resume::RestartResumeOperation;
using azzs::application::restart_resume::RestartResumeRequest;
using azzs::application::restart_resume::RestartResumeService;
using azzs::application::restart_resume::RestartResumeState;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;

struct Registration final : LoginResumeRegistration {
  bool fail_register{false};
  int register_calls{0};
  int clear_calls{0};

  LoginResumeRegistrationResult register_once() override {
    ++register_calls;
    return fail_register
               ? LoginResumeRegistrationResult{
                     .code = LoginResumeRegistrationCode::failed,
                     .detail = "registration blocked"}
               : LoginResumeRegistrationResult{
                     .code = LoginResumeRegistrationCode::registered};
  }

  LoginResumeRegistrationResult clear_once() override {
    ++clear_calls;
    return {.code = LoginResumeRegistrationCode::cleared};
  }
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "restart resume contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] RestartResumeRequest request() {
  return {.correlation_id = "correlation-12",
          .participants = {{
              .operation = RestartResumeOperation::installation_batch,
              .operation_id = "batch-12",
          }}};
}

[[nodiscard]] bool persists_a_barrier_and_requires_explicit_decision() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{}};
  DeviceStateStore states{files, clock};
  Registration registration;
  RestartResumeService service{states, registration};
  bool passed = expect(service.restore().succeeded(), "initial state must restore");
  auto armed = service.arm(request());
  passed &= expect(armed.succeeded() &&
                       armed.snapshot.state == RestartResumeState::waiting_for_windows_restart,
                   "arming must persist a waiting-for-restart checkpoint");

  RestartResumeService reopened{states, registration};
  auto restored = reopened.restore();
  passed &= expect(restored.succeeded() &&
                       restored.snapshot.state == RestartResumeState::waiting_for_windows_restart,
                   "a later process must restore the barrier");
  passed &= expect(reopened.confirm_continue().code == RestartResumeActionCode::rejected,
                   "continuation before login recovery must be rejected");
  auto login = reopened.resume_after_login();
  passed &= expect(login.succeeded() && registration.clear_calls == 1 &&
                       login.snapshot.state ==
                           RestartResumeState::awaiting_read_only_verification,
                   "login recovery must clear the one-shot registration and stay read-only");
  passed &= expect(reopened.complete_read_only_verification().succeeded() &&
                       reopened.snapshot().state == RestartResumeState::awaiting_user_decision,
                   "read-only verification must precede the user decision");
  passed &= expect(reopened.confirm_continue().succeeded() &&
                       reopened.snapshot().state == RestartResumeState::idle,
                   "only explicit continuation may clear the checkpoint");
  return passed;
}

[[nodiscard]] bool registration_failure_keeps_the_durable_gate() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{}};
  DeviceStateStore states{files, clock};
  Registration registration;
  registration.fail_register = true;
  RestartResumeService service{states, registration};
  bool passed = expect(service.restore().succeeded(), "failure fixture must restore");
  auto armed = service.arm(request());
  passed &= expect(armed.code == RestartResumeActionCode::registration_failed,
                   "registration failure must be reported");
  RestartResumeService reopened{states, registration};
  auto restored = reopened.restore();
  return passed && expect(restored.succeeded() &&
                              restored.snapshot.state ==
                                  RestartResumeState::waiting_for_windows_restart,
                          "registration failure must retain the durable restart gate");
}

[[nodiscard]] bool cancellation_requires_the_same_explicit_recovery_boundary() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{}};
  DeviceStateStore states{files, clock};
  Registration registration;
  RestartResumeService service{states, registration};
  bool passed = expect(service.restore().succeeded(), "cancellation fixture must restore");
  passed &= expect(service.arm(request()).succeeded(),
                   "cancellation fixture must persist a restart checkpoint");
  passed &= expect(service.resume_after_login().succeeded() &&
                       service.complete_read_only_verification().succeeded(),
                   "cancellation must follow login recovery and read-only verification");
  auto cancelled = service.cancel();
  passed &= expect(cancelled.succeeded() &&
                       cancelled.snapshot.state == RestartResumeState::idle,
                   "explicit cancellation must clear only the restart gate");
  passed &= expect(service.confirm_continue().code == RestartResumeActionCode::rejected,
                   "a cancelled checkpoint must not be continued later");
  return passed;
}

[[nodiscard]] bool unverified_recovery_keeps_the_restart_gate_closed() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{}};
  DeviceStateStore states{files, clock};
  Registration registration;
  RestartResumeService service{states, registration};
  bool passed = expect(service.restore().succeeded(), "unverified fixture must restore");
  passed &= expect(service.arm(request()).succeeded(),
                   "unverified fixture must persist a restart checkpoint");
  passed &= expect(service.resume_after_login().succeeded(),
                   "unverified fixture must enter read-only verification");
  passed &= expect(service.snapshot().state ==
                       RestartResumeState::awaiting_read_only_verification,
                   "a failed, missing, or lease-blocked participant recovery must remain read-only");
  passed &= expect(service.confirm_continue().code == RestartResumeActionCode::rejected,
                   "continuation must remain blocked until every participant is verified");
  passed &= expect(service.cancel().code == RestartResumeActionCode::rejected,
                   "cancellation must remain blocked until every participant is verified");
  return passed;
}

}  // namespace

int main() {
  return persists_a_barrier_and_requires_explicit_decision() &&
                 registration_failure_keeps_the_durable_gate() &&
                 cancellation_requires_the_same_explicit_recovery_boundary() &&
                 unverified_recovery_keeps_the_restart_gate_closed()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
