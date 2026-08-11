#pragma once

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/operation_occupancy.hpp"

namespace azzs::adapters::infrastructure {

// Persists the cross-instance operation lease as its own device aggregate.
// It deliberately stores only the generic occupancy primitive; installation
// and optimization state machines remain consumers owned by later issues.
class StateOperationOccupancyStorage final
    : public application::OperationOccupancyStorage {
 public:
  explicit StateOperationOccupancyStorage(
      application::DeviceStateStore& states) noexcept;

  [[nodiscard]] application::OccupancyStorageRead read() override;
  [[nodiscard]] application::OccupancyStorageWrite compare_exchange(
      std::uint64_t expected_revision,
      std::optional<application::OperationOccupant> desired) override;

 private:
  application::DeviceStateStore& states_;
};

}  // namespace azzs::adapters::infrastructure
