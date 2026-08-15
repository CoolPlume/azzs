#pragma once

#include <memory>
#include <string>

#include "azzs/application/driver_acquisition.hpp"

namespace azzs::adapters::windows {

// This narrow adapter-internal seam keeps the contract test headless. The
// application layer still supplies only RescueToolTarget, never a folder path.
class WindowsRescueFolderExplorer {
 public:
  virtual ~WindowsRescueFolderExplorer() = default;
  [[nodiscard]] virtual bool open_folder(std::wstring const& folder,
                                         std::string& error) = 0;
};

// Keeps every browser and executable boundary fixed in the platform adapter.
// The core may choose only an enum value; it cannot construct a URL, package,
// command line, driver match, download, or installer invocation.
class WindowsDriverHandoffPlatform final
    : public application::driver_acquisition::DriverHandoffPlatform {
 public:
  WindowsDriverHandoffPlatform();
  explicit WindowsDriverHandoffPlatform(
      std::unique_ptr<WindowsRescueFolderExplorer> rescue_folder_explorer);
  ~WindowsDriverHandoffPlatform() override;

  [[nodiscard]] bool assistant_installed() const noexcept override;
  [[nodiscard]] bool open(
      application::driver_acquisition::DriverEntrypoint entrypoint,
      application::driver_acquisition::DriverAssistantAction action,
      std::string& error) override;
  [[nodiscard]] bool open_rescue_folder(
      application::driver_acquisition::RescueToolTarget target,
      std::string& error) override;

 private:
  std::unique_ptr<WindowsRescueFolderExplorer> rescue_folder_explorer_;
};

class WindowsDriverNetworkObserver final
    : public application::driver_acquisition::DriverNetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override;
};

}  // namespace azzs::adapters::windows
