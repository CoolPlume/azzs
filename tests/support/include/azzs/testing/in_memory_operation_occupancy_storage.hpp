#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "azzs/application/operation_occupancy.hpp"

namespace azzs::testing {

class InMemoryOperationOccupancyStorage final
    : public application::OperationOccupancyStorage {
 public:
  [[nodiscard]] application::OccupancyStorageRead read() override {
    std::scoped_lock lock{mutex_};
    return {.record = record_};
  }

  [[nodiscard]] application::OccupancyStorageWrite compare_exchange(
      std::uint64_t expected_revision,
      std::optional<application::OperationOccupant> desired) override {
    std::scoped_lock lock{mutex_};
    if (fail_writes_) {
      return {.error = application::OccupancyStorageError::io_error,
              .raw_error = 112,
              .detail = "injected occupancy write failure"};
    }
    if (record_.revision != expected_revision) {
      return {.error = application::OccupancyStorageError::conflict,
              .record = record_};
    }
    record_.revision += 1;
    record_.active = std::move(desired);
    return {.record = record_};
  }

  void fail_writes(bool value) {
    std::scoped_lock lock{mutex_};
    fail_writes_ = value;
  }

 private:
  std::mutex mutex_;
  application::OperationOccupancyRecord record_;
  bool fail_writes_{false};
};

class SequenceLeaseTokenSource final
    : public application::LeaseTokenSource {
 public:
  explicit SequenceLeaseTokenSource(std::string prefix)
      : prefix_(std::move(prefix)) {}

  [[nodiscard]] std::string next_token() override {
    return prefix_ + std::to_string(next_++);
  }

 private:
  std::string prefix_;
  std::uint64_t next_{1};
};

}  // namespace azzs::testing
