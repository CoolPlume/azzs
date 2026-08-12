#pragma once

#include <optional>
#include <vector>

#include "azzs/application/system_settings_apply.hpp"
#include "azzs/application/device_state_store.hpp"

namespace azzs::adapters::infrastructure {

class SystemSettingsRecoveryStore final
    : public application::SystemSettingsRecoveryStore {
 public:
  explicit SystemSettingsRecoveryStore(application::DeviceStateStore& states)
      noexcept;

  [[nodiscard]] application::RecoveryStorageRead read() override;
  [[nodiscard]] application::RecoveryStorageWrite save(
      application::SystemSettingsRecoveryRecord record) override;

 private:
  application::DeviceStateStore& states_;
  std::optional<domain::RevisionToken> revision_;
  std::vector<application::SystemSettingsRecoveryRecord> records_;
  bool loaded_{false};
};

}  // namespace azzs::adapters::infrastructure
