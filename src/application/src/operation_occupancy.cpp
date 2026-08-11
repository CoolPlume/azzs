#include "azzs/application/operation_occupancy.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace azzs::application {

bool OperationIdentity::valid() const noexcept {
  auto const valid_text = [](std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::ranges::all_of(value, [](unsigned char byte) {
             return byte >= 0x20 && byte < 0x7f;
           });
  };
  return valid_text(kind, 64) && valid_text(operation_id, 256) &&
         valid_text(correlation_id, 256);
}

SharedOperationOccupancy::SharedOperationOccupancy(
    OperationOccupancyStorage& storage,
    LeaseTokenSource& tokens) noexcept
    : storage_(storage), tokens_(tokens) {}

OccupancyResult SharedOperationOccupancy::storage_failure(
    OccupancyStorageError error,
    std::int64_t raw_error,
    std::string detail,
    OperationOccupancyRecord record) {
  auto code = OccupancyResultCode::storage_error;
  if (error == OccupancyStorageError::read_only) {
    code = OccupancyResultCode::read_only;
  } else if (error == OccupancyStorageError::conflict) {
    code = OccupancyResultCode::conflict;
  }
  return OccupancyResult{
      .code = code,
      .current = std::move(record.active),
      .revision = record.revision,
      .raw_error = raw_error,
      .detail = std::move(detail),
  };
}

OccupancyResult SharedOperationOccupancy::inspect() {
  auto read = storage_.read();
  if (read.error != OccupancyStorageError::none) {
    return storage_failure(read.error, read.raw_error, std::move(read.detail),
                           std::move(read.record));
  }
  return OccupancyResult{
      .code = OccupancyResultCode::observed,
      .current = std::move(read.record.active),
      .revision = read.record.revision,
  };
}

OccupancyResult SharedOperationOccupancy::try_acquire(
    OperationIdentity identity) {
  if (!identity.valid()) {
    return {.code = OccupancyResultCode::invalid_request,
            .detail = "operation identity fields must not be empty"};
  }
  auto read = storage_.read();
  if (read.error != OccupancyStorageError::none) {
    return storage_failure(read.error, read.raw_error, std::move(read.detail),
                           std::move(read.record));
  }
  if (read.record.active.has_value()) {
    return OccupancyResult{
        .code = OccupancyResultCode::occupied,
        .current = std::move(read.record.active),
        .revision = read.record.revision,
    };
  }

  auto token = tokens_.next_token();
  if (token.empty()) {
    return {.code = OccupancyResultCode::invalid_request,
            .detail = "lease token source returned an empty token"};
  }
  OperationOccupant occupant{
      .identity = std::move(identity),
      .lease_token = std::move(token),
  };
  auto written = storage_.compare_exchange(read.record.revision, occupant);
  if (written.error == OccupancyStorageError::conflict &&
      written.record.active.has_value()) {
    return OccupancyResult{
        .code = OccupancyResultCode::occupied,
        .current = std::move(written.record.active),
        .revision = written.record.revision,
    };
  }
  if (written.error != OccupancyStorageError::none) {
    return storage_failure(written.error, written.raw_error,
                           std::move(written.detail),
                           std::move(written.record));
  }
  if (!written.record.active.has_value() ||
      written.record.active->lease_token != occupant.lease_token) {
    return {.code = OccupancyResultCode::storage_error,
            .revision = written.record.revision,
            .detail = "occupancy storage did not retain the requested lease"};
  }
  auto const lease = OperationLease{
      .identity = written.record.active->identity,
      .lease_token = written.record.active->lease_token,
      .revision = written.record.revision,
  };
  return OccupancyResult{
      .code = OccupancyResultCode::acquired,
      .lease = lease,
      .current = std::move(written.record.active),
      .revision = written.record.revision,
  };
}

OccupancyResult SharedOperationOccupancy::release(
    OperationLease const& lease) {
  if (!lease.identity.valid() || lease.lease_token.empty()) {
    return {.code = OccupancyResultCode::invalid_request,
            .detail = "lease identity and token must not be empty"};
  }
  auto read = storage_.read();
  if (read.error != OccupancyStorageError::none) {
    return storage_failure(read.error, read.raw_error, std::move(read.detail),
                           std::move(read.record));
  }
  if (!read.record.active.has_value() ||
      read.record.revision != lease.revision ||
      read.record.active->lease_token != lease.lease_token ||
      read.record.active->identity != lease.identity) {
    return OccupancyResult{
        .code = OccupancyResultCode::stale_lease,
        .current = std::move(read.record.active),
        .revision = read.record.revision,
        .detail = "lease no longer owns the device occupancy",
    };
  }
  auto written = storage_.compare_exchange(read.record.revision, std::nullopt);
  if (written.error == OccupancyStorageError::conflict) {
    return OccupancyResult{
        .code = OccupancyResultCode::stale_lease,
        .current = std::move(written.record.active),
        .revision = written.record.revision,
        .detail = "device occupancy changed before release",
    };
  }
  if (written.error != OccupancyStorageError::none) {
    return storage_failure(written.error, written.raw_error,
                           std::move(written.detail),
                           std::move(written.record));
  }
  return OccupancyResult{
      .code = OccupancyResultCode::released,
      .revision = written.record.revision,
  };
}

}  // namespace azzs::application
