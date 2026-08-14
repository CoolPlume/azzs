#pragma once

#include "azzs/application/restart_resume.hpp"

namespace azzs::adapters::windows {

class WindowsLoginResumeRegistration final
    : public application::restart_resume::LoginResumeRegistration {
 public:
  [[nodiscard]] application::restart_resume::LoginResumeRegistrationResult
  register_once() override;
  [[nodiscard]] application::restart_resume::LoginResumeRegistrationResult
  clear_once() override;
};

[[nodiscard]] bool is_restart_resume_login_launch() noexcept;

}  // namespace azzs::adapters::windows
