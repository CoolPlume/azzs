#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace azzs::application {
class DeviceStateStore;
}

namespace azzs::application::restart_resume {

// A restart checkpoint only identifies already-persisted operations. It never
// carries an executable command, source address, or mutable execution plan.
enum class RestartResumeOperation {
  system_settings,
  installation_batch,
  software_optimization_batch,
};

struct RestartResumeParticipant final {
  RestartResumeOperation operation{RestartResumeOperation::system_settings};
  std::string operation_id;

  [[nodiscard]] bool valid() const noexcept;
};

struct RestartResumeRequest final {
  std::string correlation_id;
  std::vector<RestartResumeParticipant> participants;

  [[nodiscard]] bool valid() const noexcept;
};

enum class RestartResumeState {
  idle,
  waiting_for_windows_restart,
  awaiting_read_only_verification,
  awaiting_user_decision,
  read_only,
};

struct RestartResumeSnapshot final {
  RestartResumeState state{RestartResumeState::idle};
  std::optional<RestartResumeRequest> checkpoint;
  bool writable{false};
  bool login_resume_registered{false};
  std::string detail;
};

enum class LoginResumeRegistrationCode {
  registered,
  cleared,
  unavailable,
  failed,
};

struct LoginResumeRegistrationResult final {
  LoginResumeRegistrationCode code{LoginResumeRegistrationCode::failed};
  std::string detail;
};

// The Windows adapter owns the RunOnce/task mechanism. The core owns when it
// may be armed and never treats successful registration as user consent to
// continue the pending administrator operation.
class LoginResumeRegistration {
 public:
  virtual ~LoginResumeRegistration() = default;
  [[nodiscard]] virtual LoginResumeRegistrationResult register_once() = 0;
  [[nodiscard]] virtual LoginResumeRegistrationResult clear_once() = 0;
};

enum class RestartResumeActionCode {
  succeeded,
  not_restored,
  rejected,
  persistence_failed,
  registration_failed,
  read_only,
};

struct RestartResumeActionResult final {
  RestartResumeActionCode code{RestartResumeActionCode::rejected};
  RestartResumeSnapshot snapshot;
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == RestartResumeActionCode::succeeded;
  }
};

// This service is the sole writer for the restart gate. Its caller performs
// the participant-specific observations separately, then reports when all of
// them are read-only verified. Only a later explicit user action clears the
// gate; the service never starts a pending operation itself.
class RestartResumeService final {
 public:
  RestartResumeService(DeviceStateStore& states,
                       LoginResumeRegistration& registration);
  ~RestartResumeService();

  [[nodiscard]] RestartResumeActionResult restore();
  [[nodiscard]] RestartResumeSnapshot snapshot() const;
  [[nodiscard]] RestartResumeActionResult arm(RestartResumeRequest request);
  [[nodiscard]] RestartResumeActionResult resume_after_login();
  [[nodiscard]] RestartResumeActionResult complete_read_only_verification();
  [[nodiscard]] RestartResumeActionResult confirm_continue();
  [[nodiscard]] RestartResumeActionResult cancel();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] char const* to_string(RestartResumeState value) noexcept;
[[nodiscard]] char const* to_string(RestartResumeActionCode value) noexcept;

}  // namespace azzs::application::restart_resume
