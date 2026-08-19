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
  directory_creation_failed,
  access_control_failed,
};

struct DeviceDataEnvironment final {
  std::string root_utf8;
  std::string subject_id;
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
  [[nodiscard]] static DeviceDataEnvironmentResult prepare();
};

}  // namespace azzs::adapters::windows
