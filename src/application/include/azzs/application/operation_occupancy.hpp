#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace azzs::application {

struct OperationIdentity final {
  std::string kind;
  std::string operation_id;
  std::string correlation_id;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(OperationIdentity const&,
                         OperationIdentity const&) = default;
};

struct OperationOccupant final {
  OperationIdentity identity;
  std::string lease_token;

  friend bool operator==(OperationOccupant const&,
                         OperationOccupant const&) = default;
};

struct OperationOccupancyRecord final {
  std::uint64_t revision{0};
  std::optional<OperationOccupant> active;

  friend bool operator==(OperationOccupancyRecord const&,
                         OperationOccupancyRecord const&) = default;
};

enum class OccupancyStorageError {
  none,
  conflict,
  read_only,
  io_error,
};

struct OccupancyStorageRead final {
  OccupancyStorageError error{OccupancyStorageError::none};
  OperationOccupancyRecord record;
  std::int64_t raw_error{0};
  std::string detail;
};

struct OccupancyStorageWrite final {
  OccupancyStorageError error{OccupancyStorageError::none};
  OperationOccupancyRecord record;
  std::int64_t raw_error{0};
  std::string detail;
};

// Production and test adapters implement the compare/exchange in one durable
// critical section. The primitive deliberately knows nothing about installation
// or optimization state machines.
class OperationOccupancyStorage {
 public:
  virtual ~OperationOccupancyStorage() = default;
  [[nodiscard]] virtual OccupancyStorageRead read() = 0;
  [[nodiscard]] virtual OccupancyStorageWrite compare_exchange(
      std::uint64_t expected_revision,
      std::optional<OperationOccupant> desired) = 0;
};

class LeaseTokenSource {
 public:
  virtual ~LeaseTokenSource() = default;
  [[nodiscard]] virtual std::string next_token() = 0;
};

struct OperationLease final {
  OperationIdentity identity;
  std::string lease_token;
  std::uint64_t revision{0};
};

enum class OccupancyResultCode {
  invalid_request,
  observed,
  acquired,
  occupied,
  released,
  stale_lease,
  conflict,
  read_only,
  storage_error,
};

struct OccupancyResult final {
  OccupancyResultCode code{OccupancyResultCode::storage_error};
  std::optional<OperationLease> lease;
  std::optional<OperationOccupant> current;
  std::uint64_t revision{0};
  std::int64_t raw_error{0};
  std::string detail;
};

class SharedOperationOccupancy final {
 public:
  SharedOperationOccupancy(OperationOccupancyStorage& storage,
                           LeaseTokenSource& tokens) noexcept;

  [[nodiscard]] OccupancyResult inspect();
  [[nodiscard]] OccupancyResult try_acquire(OperationIdentity identity);
  [[nodiscard]] OccupancyResult release(OperationLease const& lease);

 private:
  [[nodiscard]] static OccupancyResult storage_failure(
      OccupancyStorageError error,
      std::int64_t raw_error,
      std::string detail,
      OperationOccupancyRecord record = {});

  OperationOccupancyStorage& storage_;
  LeaseTokenSource& tokens_;
};

}  // namespace azzs::application
