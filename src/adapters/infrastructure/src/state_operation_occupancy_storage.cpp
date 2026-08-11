#include "azzs/adapters/infrastructure/state_operation_occupancy_storage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::adapters::infrastructure {
namespace {

using application::OccupancyStorageError;
using application::OccupancyStorageRead;
using application::OccupancyStorageWrite;
using application::OperationOccupancyRecord;

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'O'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kMaxKindBytes = 64;
constexpr std::size_t kMaxIdentityBytes = 256;

[[nodiscard]] domain::StateKey occupancy_key() {
  return domain::StateKey::machine(domain::AggregateId{"operation-occupancy"});
}

class Encoder final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void raw(std::span<std::byte const> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(std::as_bytes(std::span{value}));
  }

  [[nodiscard]] domain::StateBytes finish() { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class Decoder final {
 public:
  explicit Decoder(std::span<std::byte const> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t& value) {
    if (remaining() < 1) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(part) << shift;
    }
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) {
        return false;
      }
      value |= static_cast<std::uint64_t>(part) << shift;
    }
    return true;
  }

  [[nodiscard]] bool raw(std::span<std::byte> target) {
    if (remaining() < target.size()) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                target.size(), target.begin());
    position_ += target.size();
    return true;
  }

  [[nodiscard]] bool text(std::string& value, std::size_t maximum) {
    std::uint32_t size{};
    if (!u32(size) || size > maximum || remaining() < size) {
      return false;
    }
    value.assign(reinterpret_cast<char const*>(bytes_.data() + position_),
                 size);
    position_ += size;
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

 private:
  std::span<std::byte const> bytes_;
  std::size_t position_{0};
};

[[nodiscard]] bool safe_text(std::string_view value,
                             std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char byte) {
    return byte >= 0x20 && byte < 0x7f;
  });
}

[[nodiscard]] bool valid_occupant(
    application::OperationOccupant const& occupant) noexcept {
  return safe_text(occupant.identity.kind, kMaxKindBytes) &&
         safe_text(occupant.identity.operation_id, kMaxIdentityBytes) &&
         safe_text(occupant.identity.correlation_id, kMaxIdentityBytes) &&
         safe_text(occupant.lease_token, kMaxIdentityBytes);
}

[[nodiscard]] domain::StateBytes encode(OperationOccupancyRecord const& record) {
  Encoder encoder;
  encoder.raw(kMagic);
  encoder.u32(kFormatVersion);
  encoder.u64(record.revision);
  encoder.u8(record.active.has_value() ? 1 : 0);
  if (record.active.has_value()) {
    encoder.text(record.active->identity.kind);
    encoder.text(record.active->identity.operation_id);
    encoder.text(record.active->identity.correlation_id);
    encoder.text(record.active->lease_token);
  }
  return encoder.finish();
}

[[nodiscard]] std::optional<OperationOccupancyRecord> decode(
    domain::StateBytes const& bytes) {
  Decoder decoder{bytes};
  std::array<std::byte, kMagic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t active{};
  OperationOccupancyRecord record;
  if (!decoder.raw(magic) || magic != kMagic || !decoder.u32(version) ||
      version != kFormatVersion || !decoder.u64(record.revision) ||
      !decoder.u8(active) || active > 1) {
    return std::nullopt;
  }
  if (active == 1) {
    application::OperationOccupant occupant;
    if (!decoder.text(occupant.identity.kind, kMaxKindBytes) ||
        !decoder.text(occupant.identity.operation_id, kMaxIdentityBytes) ||
        !decoder.text(occupant.identity.correlation_id, kMaxIdentityBytes) ||
        !decoder.text(occupant.lease_token, kMaxIdentityBytes) ||
        !valid_occupant(occupant)) {
      return std::nullopt;
    }
    record.active = std::move(occupant);
  }
  if (decoder.remaining() != 0) {
    return std::nullopt;
  }
  return record;
}

[[nodiscard]] OccupancyStorageError map_commit_status(
    application::StateCommitStatus status) noexcept {
  using application::StateCommitStatus;
  switch (status) {
    case StateCommitStatus::committed:
      return OccupancyStorageError::none;
    case StateCommitStatus::conflict:
    case StateCommitStatus::busy:
      return OccupancyStorageError::conflict;
    case StateCommitStatus::read_only:
      return OccupancyStorageError::read_only;
    case StateCommitStatus::failed:
    case StateCommitStatus::outcome_unknown:
      return OccupancyStorageError::io_error;
  }
  return OccupancyStorageError::io_error;
}

struct LoadedRecord final {
  OccupancyStorageRead read;
  std::optional<domain::DeviceStateSnapshot> snapshot;
  bool uninitialized{false};
};

[[nodiscard]] LoadedRecord load(application::DeviceStateStore& states) {
  auto state = states.inspect(occupancy_key());
  switch (state.mode) {
    case application::StateReadMode::uninitialized:
      return {.read = {}, .uninitialized = true};
    case application::StateReadMode::writable:
      break;
    case application::StateReadMode::recovered_previous:
      return {.read = {
                  .error = OccupancyStorageError::read_only,
                  .detail =
                      "occupancy current generation is damaged; N-1 cannot safely grant or release a lease"},
              .snapshot = std::move(state.snapshot)};
    case application::StateReadMode::busy:
      return {.read = {.error = OccupancyStorageError::conflict,
                       .detail = std::move(state.error)}};
    case application::StateReadMode::read_only_future:
    case application::StateReadMode::read_only_corrupt:
      return {.read = {.error = OccupancyStorageError::read_only,
                       .detail = std::move(state.error)}};
    case application::StateReadMode::failed:
      return {.read = {.error = OccupancyStorageError::io_error,
                       .detail = std::move(state.error)}};
  }
  if (!state.snapshot.has_value()) {
    return {.read = {.error = OccupancyStorageError::io_error,
                     .detail = "state store returned no occupancy snapshot"}};
  }
  auto record = decode(state.snapshot->state.value.payload);
  if (!record.has_value()) {
    return {.read = {.error = OccupancyStorageError::read_only,
                     .detail = "occupancy payload is invalid or from an unknown format"},
            .snapshot = std::move(state.snapshot)};
  }
  return {.read = {.record = std::move(*record)},
          .snapshot = std::move(state.snapshot)};
}

}  // namespace

StateOperationOccupancyStorage::StateOperationOccupancyStorage(
    application::DeviceStateStore& states) noexcept
    : states_(states) {}

application::OccupancyStorageRead StateOperationOccupancyStorage::read() {
  return load(states_).read;
}

application::OccupancyStorageWrite
StateOperationOccupancyStorage::compare_exchange(
    std::uint64_t expected_revision,
    std::optional<application::OperationOccupant> desired) {
  if (desired.has_value() && !valid_occupant(*desired)) {
    return {.error = OccupancyStorageError::io_error,
            .detail = "occupancy identity or lease token is invalid"};
  }
  auto loaded = load(states_);
  if (loaded.read.error != OccupancyStorageError::none) {
    return {.error = loaded.read.error,
            .record = std::move(loaded.read.record),
            .raw_error = loaded.read.raw_error,
            .detail = std::move(loaded.read.detail)};
  }
  if (loaded.read.record.revision != expected_revision ||
      expected_revision == std::numeric_limits<std::uint64_t>::max()) {
    return {.error = OccupancyStorageError::conflict,
            .record = std::move(loaded.read.record),
            .detail = "occupancy revision changed"};
  }

  OperationOccupancyRecord desired_record{
      .revision = expected_revision + 1,
      .active = std::move(desired),
  };
  domain::DeviceState desired_state{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode(desired_record)},
  };
  application::StateCommitResult committed;
  if (loaded.uninitialized) {
    committed = states_.initialize(occupancy_key(), std::move(desired_state));
  } else if (loaded.snapshot.has_value()) {
    committed = states_.commit(application::StateCommitRequest{
        .key = occupancy_key(),
        .expected_revision = loaded.snapshot->revision,
        .state = std::move(desired_state),
    });
  } else {
    return {.error = OccupancyStorageError::io_error,
            .record = std::move(loaded.read.record),
            .detail = "occupancy snapshot disappeared before commit"};
  }
  if (committed.status == application::StateCommitStatus::committed) {
    return {.record = std::move(desired_record)};
  }

  auto current = load(states_).read;
  if (current.error == OccupancyStorageError::none &&
      current.record == desired_record) {
    return {.record = std::move(desired_record),
            .detail = "occupancy commit was confirmed by authoritative reread"};
  }
  return {.error = map_commit_status(committed.status),
          .record = std::move(current.record),
          .raw_error = current.raw_error,
          .detail = committed.error.empty() ? std::move(current.detail)
                                            : std::move(committed.error)};
}

}  // namespace azzs::adapters::infrastructure
