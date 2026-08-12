#pragma once

#include "azzs/application/application_update.hpp"
#include "azzs/application/device_state_store.hpp"

namespace azzs::adapters::infrastructure {

// Persists only application replacement health facts. It does not own release
// selection, update policy, device recovery records, execution history, or
// emergency withdrawal state.
class StateApplicationUpdateHealthStorage final
    : public application::ApplicationUpdateHealthStorage {
 public:
  explicit StateApplicationUpdateHealthStorage(
      application::DeviceStateStore& states) noexcept;

  [[nodiscard]] application::ApplicationUpdateHealthRead read() override;
  [[nodiscard]] application::UpdatePlatformResult write(
      application::ApplicationUpdateHealthRecord const& record) override;
  [[nodiscard]] application::UpdatePlatformResult clear() override;

 private:
  application::DeviceStateStore& states_;
};

}  // namespace azzs::adapters::infrastructure
