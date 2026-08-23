#pragma once

#include "azzs/application/application_update.hpp"
#include "azzs/application/device_state_store.hpp"

namespace azzs::adapters::infrastructure {

// Versioned, fail-closed persistence for the automatic update-check facts.
// This state is intentionally separate from the active-update health record.
class StateApplicationUpdateCheckStorage final
    : public application::ApplicationUpdateCheckStorage {
 public:
  explicit StateApplicationUpdateCheckStorage(
      application::DeviceStateStore& states) noexcept;

  [[nodiscard]] application::ApplicationUpdateCheckRead read() override;
  [[nodiscard]] application::UpdatePlatformResult write(
      application::ApplicationUpdateCheckState const& state) override;

 private:
  application::DeviceStateStore& states_;
};

}  // namespace azzs::adapters::infrastructure
