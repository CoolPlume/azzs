#include "azzs/adapters/infrastructure/state_application_update_health_storage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::adapters::infrastructure {
namespace {

using application::ApplicationBuildIdentity;
using application::ApplicationReleaseChannel;
using application::ApplicationReleaseEdition;
using application::ApplicationReleaseForm;
using application::ApplicationUpdateHealthRead;
using application::ApplicationUpdateHealthRecord;
using application::ApplicationUpdateHealthPhase;
using application::UpdatePlatformResult;
using application::UpdatePlatformResultCode;

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'U'}, std::byte{'H'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kMaximumPayloadBytes = 4096;
constexpr std::size_t kMaximumVersionBytes = 64;

[[nodiscard]] domain::StateKey health_key() {
  return domain::StateKey::machine(
      domain::AggregateId{"application-update-health"});
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

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    auto const bytes = std::as_bytes(std::span{value});
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  void raw(std::span<std::byte const> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  [[nodiscard]] domain::StateBytes finish() && { return std::move(bytes_); }

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
      std::uint8_t byte{};
      if (!u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t byte{};
      if (!u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
  }

  [[nodiscard]] bool i64(std::int64_t& value) {
    std::uint64_t encoded{};
    if (!u64(encoded)) {
      return false;
    }
    value = static_cast<std::int64_t>(encoded);
    return true;
  }

  [[nodiscard]] bool raw(std::span<std::byte> destination) {
    if (remaining() < destination.size()) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                destination.size(), destination.begin());
    position_ += destination.size();
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

[[nodiscard]] bool valid_enum(std::uint8_t value, std::uint8_t maximum) {
  return value <= maximum;
}

void encode_build(Encoder& encoder, ApplicationBuildIdentity const& build) {
  encoder.text(build.version);
  encoder.u8(static_cast<std::uint8_t>(build.channel));
  encoder.u8(static_cast<std::uint8_t>(build.architecture));
  encoder.u8(static_cast<std::uint8_t>(build.edition));
  encoder.u8(static_cast<std::uint8_t>(build.form));
}

[[nodiscard]] bool decode_build(Decoder& decoder, ApplicationBuildIdentity& build) {
  std::uint8_t channel{};
  std::uint8_t architecture{};
  std::uint8_t edition{};
  std::uint8_t form{};
  if (!decoder.text(build.version, kMaximumVersionBytes) ||
       !decoder.u8(channel) || !decoder.u8(architecture) ||
       !decoder.u8(edition) || !decoder.u8(form) ||
       !valid_enum(channel, static_cast<std::uint8_t>(
                                ApplicationReleaseChannel::prerelease)) ||
      !valid_enum(architecture, static_cast<std::uint8_t>(
                                    domain::SystemArchitecture::unknown)) ||
      !valid_enum(edition, static_cast<std::uint8_t>(
                               ApplicationReleaseEdition::large_offline)) ||
      !valid_enum(form,
                  static_cast<std::uint8_t>(ApplicationReleaseForm::installed))) {
    return false;
  }
  build.channel = static_cast<ApplicationReleaseChannel>(channel);
  build.architecture = static_cast<domain::SystemArchitecture>(architecture);
  build.edition = static_cast<ApplicationReleaseEdition>(edition);
  build.form = static_cast<ApplicationReleaseForm>(form);
  return build.valid();
}

[[nodiscard]] std::optional<domain::StateBytes> encode(
    std::optional<ApplicationUpdateHealthRecord> const& record) {
  Encoder encoder;
  encoder.raw(kMagic);
  encoder.u32(kFormatVersion);
  encoder.u8(record.has_value() ? 1 : 0);
  if (record.has_value()) {
    if (!record->previous.valid() || !record->target.valid()) {
      return std::nullopt;
    }
    auto const phase = static_cast<std::uint8_t>(record->phase);
    if (!valid_enum(phase, static_cast<std::uint8_t>(
                              ApplicationUpdateHealthPhase::
                                  previous_start_failed))) {
      return std::nullopt;
    }
    encoder.u8(phase);
    encode_build(encoder, record->previous);
    encode_build(encoder, record->target);
    encoder.u8(record->retry_attempted ? 1 : 0);
    encoder.i64(record->started_at.time_since_epoch().count());
  }
  auto bytes = std::move(encoder).finish();
  if (bytes.size() > kMaximumPayloadBytes) {
    return std::nullopt;
  }
  return bytes;
}

[[nodiscard]] std::optional<std::optional<ApplicationUpdateHealthRecord>> decode(
    std::span<std::byte const> bytes) {
  if (bytes.size() > kMaximumPayloadBytes) {
    return std::nullopt;
  }
  Decoder decoder{bytes};
  std::array<std::byte, kMagic.size()> magic{};
  std::uint32_t format{};
  std::uint8_t present{};
  if (!decoder.raw(magic) || magic != kMagic || !decoder.u32(format) ||
      format != kFormatVersion || !decoder.u8(present) || present > 1) {
    return std::nullopt;
  }
  if (present == 0) {
    return decoder.remaining() == 0
               ? std::optional<std::optional<ApplicationUpdateHealthRecord>>{
                     std::optional<ApplicationUpdateHealthRecord>{}}
               : std::nullopt;
  }

  ApplicationUpdateHealthRecord record;
  std::uint8_t phase{};
  std::uint8_t retry{};
  std::int64_t started{};
  if (!decoder.u8(phase) ||
      !valid_enum(phase, static_cast<std::uint8_t>(
                              ApplicationUpdateHealthPhase::
                                  previous_start_failed)) ||
      !decode_build(decoder, record.previous) ||
      !decode_build(decoder, record.target) || !decoder.u8(retry) || retry > 1 ||
      !decoder.i64(started) || decoder.remaining() != 0) {
    return std::nullopt;
  }
  record.phase = static_cast<ApplicationUpdateHealthPhase>(phase);
  record.retry_attempted = retry == 1;
  record.started_at = application::WallClockTime{
      std::chrono::milliseconds{started}};
  return std::optional<ApplicationUpdateHealthRecord>{std::move(record)};
}

[[nodiscard]] domain::DeviceState state_for(domain::StateBytes payload) {
  return {.value = {.schema = 2,
                    .minimum_reader = 1,
                    .minimum_writer = 2,
                    .payload = std::move(payload)}};
}

[[nodiscard]] UpdatePlatformResult failure(std::string detail) {
  return {.code = UpdatePlatformResultCode::failed, .detail = std::move(detail)};
}

[[nodiscard]] UpdatePlatformResult commit(
    application::DeviceStateStore& states,
    std::optional<ApplicationUpdateHealthRecord> const& record) {
  auto payload = encode(record);
  if (!payload.has_value()) {
    return failure("application update health record is invalid");
  }
  auto const key = health_key();
  auto observed = states.inspect(key);
  if (observed.mode == application::StateReadMode::uninitialized) {
    auto initialized = states.initialize(key, state_for(std::move(*payload)));
    if (initialized.status == application::StateCommitStatus::committed) {
      return {.code = UpdatePlatformResultCode::succeeded};
    }
    return failure(initialized.error.empty()
                       ? "could not initialize application update health record"
                       : std::move(initialized.error));
  }
  if (observed.mode != application::StateReadMode::writable ||
      !observed.snapshot.has_value()) {
    return failure(observed.error.empty()
                       ? "application update health record is not writable"
                       : std::move(observed.error));
  }
  auto committed = states.commit(application::StateCommitRequest{
      .key = key,
      .expected_revision = observed.snapshot->revision,
      .state = state_for(std::move(*payload)),
  });
  if (committed.status == application::StateCommitStatus::committed) {
    return {.code = UpdatePlatformResultCode::succeeded};
  }
  auto confirmed = states.inspect(key);
  if (confirmed.mode == application::StateReadMode::writable &&
      confirmed.snapshot.has_value()) {
    auto current = decode(confirmed.snapshot->state.value.payload);
    if (current.has_value() && *current == record) {
      return {.code = UpdatePlatformResultCode::succeeded,
              .detail = "health record commit was confirmed by authoritative reread"};
    }
  }
  return failure(committed.error.empty()
                     ? "could not commit application update health record"
                     : std::move(committed.error));
}

}  // namespace

StateApplicationUpdateHealthStorage::StateApplicationUpdateHealthStorage(
    application::DeviceStateStore& states) noexcept
    : states_(states) {}

application::ApplicationUpdateHealthRead
StateApplicationUpdateHealthStorage::read() {
  auto observed = states_.inspect(health_key());
  if (observed.mode == application::StateReadMode::uninitialized) {
    return {.code = UpdatePlatformResultCode::succeeded};
  }
  if (observed.mode != application::StateReadMode::writable ||
      !observed.snapshot.has_value()) {
    return {.code = UpdatePlatformResultCode::failed,
            .detail = observed.error.empty()
                          ? "application update health record is not readable"
                          : std::move(observed.error)};
  }
  auto record = decode(observed.snapshot->state.value.payload);
  if (!record.has_value()) {
    return {.code = UpdatePlatformResultCode::failed,
            .detail = "application update health payload is invalid"};
  }
  return {.code = UpdatePlatformResultCode::succeeded,
          .record = std::move(*record)};
}

application::UpdatePlatformResult
StateApplicationUpdateHealthStorage::write(
    application::ApplicationUpdateHealthRecord const& record) {
  return commit(states_, record);
}

application::UpdatePlatformResult StateApplicationUpdateHealthStorage::clear() {
  return commit(states_, std::nullopt);
}

}  // namespace azzs::adapters::infrastructure
