#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace azzs::adapters::windows {

enum class DeviceDataEnvironmentError {
  none,
  program_data_unavailable,
  unsupported_storage_location,
  unsafe_storage_path,
  interactive_subject_unavailable,
  alternate_credentials_not_supported,
  invalid_test_override,
  directory_creation_failed,
  access_control_failed,
};

struct DeviceDataEnvironmentOptions final {
  // Tests may inject an isolated root. Production leaves this empty and always
  // resolves the machine ProgramData known folder.
  std::optional<std::string> root_override_utf8;
  // A subject override is accepted only together with a root override. It must
  // be a valid Windows SID string and exists solely for adapter contract tests.
  std::optional<std::string> subject_override;
};

struct DeviceDataEnvironment final {
  std::string root_utf8;
  std::string subject_id;
  bool uses_test_root{false};
};

struct DeviceDataEnvironmentResult final {
  std::optional<DeviceDataEnvironment> environment;
  DeviceDataEnvironmentError error{DeviceDataEnvironmentError::none};
  std::uint32_t raw_error{0};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return environment.has_value();
  }
};

class WindowsDeviceDataEnvironment final {
 public:
  [[nodiscard]] static DeviceDataEnvironmentResult prepare(
      DeviceDataEnvironmentOptions options = {});
};

}  // namespace azzs::adapters::windows
