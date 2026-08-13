#pragma once

#include "azzs/application/system_settings_apply.hpp"

namespace azzs::adapters::windows {

class WindowsSystemSettingsAdapter final
    : public application::SystemSettingsPlatformAdapter {
 public:
  [[nodiscard]] std::optional<
      application::settings_domain::WindowsVersion>
  windows_version() const override;
  [[nodiscard]] application::SystemSettingsRead read(
      application::ControlledSystemSetting setting) override;
  [[nodiscard]] application::SystemSettingsAdapterResult apply(
      application::ControlledSystemSetting setting) override;
  [[nodiscard]] application::SystemSettingsAdapterResult restore(
      application::ControlledSystemSetting setting,
      application::WindowsSystemSettingValue value) override;
  [[nodiscard]] application::SystemSettingsAdapterResult
  restart_explorer() override;
};

}  // namespace azzs::adapters::windows
