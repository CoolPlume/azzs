#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "azzs/adapters/windows/windows_device_data_environment.hpp"

namespace azzs::testing {

struct WindowsDeviceDataIdentityEvidence final {
  std::optional<std::string> process_sid;
  std::optional<std::string> wts_session_sid;
  std::optional<std::string> desktop_shell_sid;
  std::optional<std::string> wts_user_name;
  std::optional<std::string> wts_domain_name;
  // When WTS returns a username but cannot retrieve its domain, the exact
  // query error determines whether a same-session shell may be considered.
  // ERROR_NONE_MAPPED is an unqualified identity and must fail closed;
  // transport failures may use the shell fallback.
  std::uint32_t wts_domain_raw_error{0};
  std::uint32_t wts_raw_error{0};
  // A shell candidate is unusable until the test explicitly proves that it
  // belongs to the current interactive session.
  bool desktop_shell_session_matches{false};
  std::uint32_t desktop_shell_raw_error{0};
};

struct WindowsDeviceDataTestOptions final {
  std::string root_override_utf8;
  WindowsDeviceDataIdentityEvidence test_identity_evidence;
  std::optional<std::string> subject_override;
};

struct WindowsDeviceDataSubjectResolution final {
  std::optional<std::string> subject_id;
  adapters::windows::DeviceDataEnvironmentError error{
      adapters::windows::DeviceDataEnvironmentError::none};
  std::uint32_t raw_error{0};

  [[nodiscard]] explicit operator bool() const noexcept {
    return subject_id.has_value();
  }
};

[[nodiscard]] WindowsDeviceDataSubjectResolution
resolve_windows_device_data_subject_for_test(
    WindowsDeviceDataIdentityEvidence evidence);

[[nodiscard]] adapters::windows::DeviceDataEnvironmentResult
prepare_windows_device_data_for_test(WindowsDeviceDataTestOptions options);

}  // namespace azzs::testing
