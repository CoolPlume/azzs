#pragma once

#include <string>

#include "azzs/application/driver_acquisition.hpp"

namespace azzs::adapters::windows {

// Keeps every browser and executable boundary fixed in the platform adapter.
// The core may choose only an enum value; it cannot construct a URL, package,
// command line, driver match, download, or installer invocation.
class WindowsDriverHandoffPlatform final
    : public application::driver_acquisition::DriverHandoffPlatform {
 public:
  [[nodiscard]] bool assistant_installed() const noexcept override;
  [[nodiscard]] bool open(
      application::driver_acquisition::DriverEntrypoint entrypoint,
      application::driver_acquisition::DriverAssistantAction action,
      std::string& error) override;
};

class WindowsDriverNetworkObserver final
    : public application::driver_acquisition::DriverNetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override;
};

}  // namespace azzs::adapters::windows
