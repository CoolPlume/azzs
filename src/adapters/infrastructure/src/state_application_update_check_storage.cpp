#include "azzs/adapters/infrastructure/state_application_update_check_storage.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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
using application::ApplicationUpdateCandidate;
using application::ApplicationUpdateCheckRead;
using application::ApplicationUpdateCheckState;
using application::ApplicationUpdateCheckOutcome;
using application::ApplicationUpdateCheckSchedule;
using application::UpdatePlatformResult;
using application::UpdatePlatformResultCode;

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'U'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kMaximumPayloadBytes = 16 * 1024;
constexpr std::size_t kMaximumIdBytes = 256;
constexpr std::size_t kMaximumUrlBytes = 2048;
constexpr std::size_t kMaximumSummaryBytes = 8192;

[[nodiscard]] domain::StateKey check_key() {
  return domain::StateKey::machine(
      domain::AggregateId{"application-update-check"});
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
    if (remaining() < 1) return false;
    value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }
  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      std::uint8_t byte{};
      if (!u8(byte)) return false;
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return true;
  }
  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t byte{};
      if (!u8(byte)) return false;
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
  }
  [[nodiscard]] bool i64(std::int64_t& value) {
    std::uint64_t encoded{};
    if (!u64(encoded)) return false;
    value = static_cast<std::int64_t>(encoded);
    return true;
  }
  [[nodiscard]] bool raw(std::span<std::byte> destination) {
    if (remaining() < destination.size()) return false;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                destination.size(), destination.begin());
    position_ += destination.size();
    return true;
  }
  [[nodiscard]] bool text(std::string& value, std::size_t maximum) {
    std::uint32_t size{};
    if (!u32(size) || size > maximum || remaining() < size) return false;
    value.assign(reinterpret_cast<char const*>(bytes_.data() + position_), size);
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

[[nodiscard]] bool safe_metadata(std::string_view value,
                                 std::size_t maximum) noexcept {
  if (value.size() > maximum) return false;
  for (auto const byte : value) {
    auto const code = static_cast<unsigned char>(byte);
    if (code != '\t' && code != '\n' && code != '\r' && code < 0x20) {
      return false;
    }
  }
  return true;
}

void encode_build(Encoder& encoder, ApplicationBuildIdentity const& build) {
  encoder.text(build.version);
  encoder.u8(static_cast<std::uint8_t>(build.channel));
  encoder.u8(static_cast<std::uint8_t>(build.architecture));
  encoder.u8(static_cast<std::uint8_t>(build.edition));
  encoder.u8(static_cast<std::uint8_t>(build.form));
}

[[nodiscard]] bool decode_build(Decoder& decoder, ApplicationBuildIdentity& build) {
  std::uint8_t channel{}, architecture{}, edition{}, form{};
  if (!decoder.text(build.version, 64) || !decoder.u8(channel) ||
      !decoder.u8(architecture) || !decoder.u8(edition) || !decoder.u8(form) ||
      !valid_enum(channel, static_cast<std::uint8_t>(
                              ApplicationReleaseChannel::prerelease)) ||
      !valid_enum(architecture, static_cast<std::uint8_t>(
                                    domain::SystemArchitecture::unknown)) ||
      !valid_enum(edition, static_cast<std::uint8_t>(
                               ApplicationReleaseEdition::large_offline)) ||
      !valid_enum(form, static_cast<std::uint8_t>(
                            ApplicationReleaseForm::installed))) {
    return false;
  }
  build.channel = static_cast<ApplicationReleaseChannel>(channel);
  build.architecture = static_cast<domain::SystemArchitecture>(architecture);
  build.edition = static_cast<ApplicationReleaseEdition>(edition);
  build.form = static_cast<ApplicationReleaseForm>(form);
  return build.valid();
}

void encode_candidate(Encoder& encoder, ApplicationUpdateCandidate const& candidate) {
  encoder.text(candidate.release_id);
  encoder.text(candidate.release_tag);
  encoder.text(candidate.asset_id);
  encode_build(encoder, candidate.target);
  encoder.text(candidate.release_title);
  encoder.text(candidate.release_url);
  encoder.text(candidate.published_at);
  encoder.text(candidate.summary);
}

[[nodiscard]] bool decode_candidate(Decoder& decoder,
                                    ApplicationUpdateCandidate& candidate) {
  if (!decoder.text(candidate.release_id, kMaximumIdBytes) ||
      !decoder.text(candidate.release_tag, kMaximumIdBytes) ||
      !decoder.text(candidate.asset_id, kMaximumIdBytes) ||
      !decode_build(decoder, candidate.target) ||
      !decoder.text(candidate.release_title, kMaximumIdBytes) ||
      !decoder.text(candidate.release_url, kMaximumUrlBytes) ||
      !decoder.text(candidate.published_at, kMaximumIdBytes) ||
      !decoder.text(candidate.summary, kMaximumSummaryBytes) ||
      !safe_metadata(candidate.release_id, kMaximumIdBytes) ||
      !safe_metadata(candidate.release_tag, kMaximumIdBytes) ||
      !safe_metadata(candidate.asset_id, kMaximumIdBytes) ||
      !safe_metadata(candidate.release_title, kMaximumIdBytes) ||
      !safe_metadata(candidate.release_url, kMaximumUrlBytes) ||
      !safe_metadata(candidate.published_at, kMaximumIdBytes) ||
      !safe_metadata(candidate.summary, kMaximumSummaryBytes)) {
    return false;
  }
  return !candidate.release_id.empty() && !candidate.release_tag.empty() &&
         !candidate.asset_id.empty();
}

[[nodiscard]] std::optional<domain::StateBytes> encode(
    ApplicationUpdateCheckState const& state) {
  if (static_cast<unsigned>(state.schedule) >
          static_cast<unsigned>(ApplicationUpdateCheckSchedule::weekly) ||
      static_cast<unsigned>(state.outcome) >
          static_cast<unsigned>(ApplicationUpdateCheckOutcome::deferred) ||
      !safe_metadata(state.detail, kMaximumSummaryBytes)) {
    return std::nullopt;
  }
  Encoder encoder;
  encoder.raw(kMagic);
  encoder.u32(kFormatVersion);
  encoder.u8(static_cast<std::uint8_t>(state.schedule));
  encoder.u8(static_cast<std::uint8_t>(state.outcome));
  encoder.u8(state.last_checked_at.has_value() ? 1 : 0);
  if (state.last_checked_at.has_value()) {
    encoder.i64(state.last_checked_at->time_since_epoch().count());
  }
  encoder.u8(state.candidate.has_value() ? 1 : 0);
  if (state.candidate.has_value()) {
    if (!state.candidate->target.valid() || state.candidate->release_id.empty() ||
        state.candidate->release_tag.empty() || state.candidate->asset_id.empty() ||
        !safe_metadata(state.candidate->release_id, kMaximumIdBytes) ||
        !safe_metadata(state.candidate->release_tag, kMaximumIdBytes) ||
        !safe_metadata(state.candidate->asset_id, kMaximumIdBytes) ||
        !safe_metadata(state.candidate->release_title, kMaximumIdBytes) ||
        !safe_metadata(state.candidate->release_url, kMaximumUrlBytes) ||
        !safe_metadata(state.candidate->published_at, kMaximumIdBytes) ||
        !safe_metadata(state.candidate->summary, kMaximumSummaryBytes)) {
      return std::nullopt;
    }
    encode_candidate(encoder, *state.candidate);
  }
  encoder.text(state.detail);
  auto bytes = std::move(encoder).finish();
  if (bytes.size() > kMaximumPayloadBytes) return std::nullopt;
  return bytes;
}

[[nodiscard]] std::optional<ApplicationUpdateCheckState> decode(
    std::span<std::byte const> bytes) {
  if (bytes.size() > kMaximumPayloadBytes) return std::nullopt;
  Decoder decoder{bytes};
  std::array<std::byte, kMagic.size()> magic{};
  std::uint32_t format{};
  std::uint8_t schedule{}, outcome{}, has_time{}, has_candidate{};
  if (!decoder.raw(magic) || magic != kMagic || !decoder.u32(format) ||
      format != kFormatVersion || !decoder.u8(schedule) ||
      !decoder.u8(outcome) || !decoder.u8(has_time) || has_time > 1 ||
      !valid_enum(schedule, static_cast<std::uint8_t>(
                              ApplicationUpdateCheckSchedule::weekly)) ||
      !valid_enum(outcome, static_cast<std::uint8_t>(
                              ApplicationUpdateCheckOutcome::deferred))) {
    return std::nullopt;
  }
  ApplicationUpdateCheckState state;
  state.schedule = static_cast<ApplicationUpdateCheckSchedule>(schedule);
  state.outcome = static_cast<ApplicationUpdateCheckOutcome>(outcome);
  if (has_time != 0) {
    std::int64_t timestamp{};
    if (!decoder.i64(timestamp)) return std::nullopt;
    state.last_checked_at = application::WallClockTime{
        std::chrono::milliseconds{timestamp}};
  }
  if (!decoder.u8(has_candidate) || has_candidate > 1) return std::nullopt;
  if (has_candidate != 0) {
    ApplicationUpdateCandidate candidate;
    if (!decode_candidate(decoder, candidate)) return std::nullopt;
    state.candidate = std::move(candidate);
  }
  if (!decoder.text(state.detail, kMaximumSummaryBytes) ||
      !safe_metadata(state.detail, kMaximumSummaryBytes) ||
      decoder.remaining() != 0) {
    return std::nullopt;
  }
  return state;
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
    ApplicationUpdateCheckState const& state) {
  auto payload = encode(state);
  if (!payload.has_value()) {
    return failure("application update check state is invalid");
  }
  auto const key = check_key();
  auto observed = states.inspect(key);
  if (observed.mode == application::StateReadMode::uninitialized) {
    auto initialized = states.initialize(key, state_for(std::move(*payload)));
    if (initialized.status == application::StateCommitStatus::committed) {
      return {.code = UpdatePlatformResultCode::succeeded};
    }
    return failure(initialized.error.empty()
                       ? "could not initialize application update check state"
                       : std::move(initialized.error));
  }
  if (observed.mode != application::StateReadMode::writable ||
      !observed.snapshot.has_value()) {
    return failure(observed.error.empty()
                       ? "application update check state is not writable"
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
    if (current.has_value() && *current == state) {
      return {.code = UpdatePlatformResultCode::succeeded,
              .detail = "check state commit was confirmed by authoritative reread"};
    }
  }
  return failure(committed.error.empty()
                     ? "could not commit application update check state"
                     : std::move(committed.error));
}

}  // namespace

StateApplicationUpdateCheckStorage::StateApplicationUpdateCheckStorage(
    application::DeviceStateStore& states) noexcept
    : states_(states) {}

ApplicationUpdateCheckRead StateApplicationUpdateCheckStorage::read() {
  auto observed = states_.inspect(check_key());
  if (observed.mode == application::StateReadMode::uninitialized) {
    return {.code = UpdatePlatformResultCode::succeeded};
  }
  if (observed.mode != application::StateReadMode::writable ||
      !observed.snapshot.has_value()) {
    return {.code = UpdatePlatformResultCode::failed,
            .detail = observed.error.empty()
                          ? "application update check state is not readable"
                          : std::move(observed.error)};
  }
  auto state = decode(observed.snapshot->state.value.payload);
  if (!state.has_value()) {
    return {.code = UpdatePlatformResultCode::failed,
            .detail = "application update check payload is invalid"};
  }
  return {.code = UpdatePlatformResultCode::succeeded, .state = std::move(*state)};
}

UpdatePlatformResult StateApplicationUpdateCheckStorage::write(
    ApplicationUpdateCheckState const& state) {
  return commit(states_, state);
}

}  // namespace azzs::adapters::infrastructure
