#include "azzs/application/emergency_withdrawal_service.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace azzs::application {
namespace {

namespace withdrawal = domain::emergency_withdrawal;

constexpr std::array<std::byte, 8> k_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'E'}, std::byte{'W'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t k_payload_version = 1;
constexpr std::size_t k_max_payload = 4U * 1024U * 1024U;
constexpr std::size_t k_max_notices = 64;
constexpr std::size_t k_max_entries = 4096;

[[nodiscard]] bool notice_set_valid(
    std::vector<withdrawal::EmergencyWithdrawalNotice> const& notices) {
  if (notices.size() > k_max_notices) {
    return false;
  }
  std::size_t entries = 0;
  for (std::size_t index = 0; index < notices.size(); ++index) {
    if (!notices[index].valid() ||
        notices[index].entries.size() > k_max_entries - entries) {
      return false;
    }
    entries += notices[index].entries.size();
    for (std::size_t other = index + 1; other < notices.size(); ++other) {
      if (notices[index].source == notices[other].source) {
        return false;
      }
    }
  }
  return true;
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
    std::uint64_t raw{};
    if (!u64(raw)) return false;
    value = static_cast<std::int64_t>(raw);
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

void encode_range(Encoder& encoder, withdrawal::VersionRange const& range) {
  encoder.text(range.minimum);
  encoder.text(range.maximum);
}

bool decode_range(Decoder& decoder, withdrawal::VersionRange& range) {
  return decoder.text(range.minimum, 128) && decoder.text(range.maximum, 128);
}

std::optional<domain::StateBytes> encode_snapshot(
    EmergencyWithdrawalSnapshot const& snapshot) {
  if (!notice_set_valid(snapshot.notices)) {
    return std::nullopt;
  }
  Encoder encoder;
  encoder.raw(k_magic);
  encoder.u32(k_payload_version);
  encoder.u8(static_cast<std::uint8_t>(snapshot.notices.size()));
  for (auto const& notice : snapshot.notices) {
    encoder.text(notice.source);
    encoder.u64(notice.revision);
    encoder.i64(notice.published_at.time_since_epoch().count());
    encoder.u32(static_cast<std::uint32_t>(notice.entries.size()));
    for (auto const& entry : notice.entries) {
      encoder.text(entry.stable_id);
      encoder.u8(static_cast<std::uint8_t>(entry.category));
      encode_range(encoder, entry.affected_versions);
      encoder.text(entry.reason);
      encoder.u8(static_cast<std::uint8_t>(entry.action));
    }
  }
  encoder.u8(snapshot.last_observed_at.has_value() ? 1 : 0);
  if (snapshot.last_observed_at.has_value()) {
    encoder.i64(snapshot.last_observed_at->time_since_epoch().count());
  }
  auto bytes = std::move(encoder).finish();
  if (bytes.size() > k_max_payload) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<EmergencyWithdrawalSnapshot> decode_snapshot(
    std::span<std::byte const> bytes) {
  if (bytes.size() > k_max_payload || bytes.size() < k_magic.size() + 5) {
    return std::nullopt;
  }
  Decoder decoder{bytes};
  std::array<std::byte, 8> magic{};
  for (auto& byte : magic) {
    std::uint8_t value{};
    if (!decoder.u8(value)) return std::nullopt;
    byte = std::byte{value};
  }
  if (magic != k_magic) return std::nullopt;
  std::uint32_t version{};
  std::uint8_t notice_count{};
  if (!decoder.u32(version) || version != k_payload_version ||
      !decoder.u8(notice_count) || notice_count > k_max_notices) {
    return std::nullopt;
  }
  EmergencyWithdrawalSnapshot snapshot;
  snapshot.state = EmergencyWithdrawalServiceState::cached;
  for (std::uint8_t index = 0; index < notice_count; ++index) {
    withdrawal::EmergencyWithdrawalNotice notice;
    std::int64_t published{};
    std::uint32_t entry_count{};
    if (!decoder.text(notice.source, 256) || !decoder.u64(notice.revision) ||
        !decoder.i64(published) || !decoder.u32(entry_count) ||
        entry_count == 0 || entry_count > k_max_entries) {
      return std::nullopt;
    }
    notice.published_at = withdrawal::Timestamp{
        std::chrono::milliseconds{published}};
    for (std::uint32_t entry_index = 0; entry_index < entry_count;
         ++entry_index) {
      withdrawal::WithdrawalEntry entry;
      std::uint8_t category{}, action{};
      if (!decoder.text(entry.stable_id, 256) || !decoder.u8(category) ||
          !decode_range(decoder, entry.affected_versions) ||
          !decoder.text(entry.reason, 4096) || !decoder.u8(action) ||
          category < static_cast<std::uint8_t>(withdrawal::OperationCategory::software_installation) ||
          category > static_cast<std::uint8_t>(withdrawal::OperationCategory::software_optimization) ||
          action < static_cast<std::uint8_t>(withdrawal::NoticeAction::withdraw) ||
          action > static_cast<std::uint8_t>(withdrawal::NoticeAction::release)) {
        return std::nullopt;
      }
      entry.category = static_cast<withdrawal::OperationCategory>(category);
      entry.action = static_cast<withdrawal::NoticeAction>(action);
      if (!entry.valid()) return std::nullopt;
      notice.entries.push_back(std::move(entry));
    }
    if (!notice.valid()) return std::nullopt;
    snapshot.notices.push_back(std::move(notice));
  }
  std::uint8_t has_observed{};
  if (!decoder.u8(has_observed) || has_observed > 1) return std::nullopt;
  if (has_observed != 0) {
    std::int64_t observed{};
    if (!decoder.i64(observed)) return std::nullopt;
    snapshot.last_observed_at = WallClockTime{std::chrono::milliseconds{observed}};
  }
  if (decoder.remaining() != 0) return std::nullopt;
  if (!notice_set_valid(snapshot.notices)) return std::nullopt;
  return snapshot;
}

[[nodiscard]] domain::StateKey emergency_key() {
  return domain::StateKey::machine(
      domain::AggregateId{"emergency-withdrawals"});
}

[[nodiscard]] domain::DeviceState empty_state(domain::StateBytes payload) {
  return {.value = {.schema = 2,
                    .minimum_reader = 1,
                    .minimum_writer = 2,
                    .payload = std::move(payload)}};
}

[[nodiscard]] std::string category_name(
    withdrawal::OperationCategory category) {
  switch (category) {
    case withdrawal::OperationCategory::software_installation:
      return "software_installation";
    case withdrawal::OperationCategory::system_optimization:
      return "system_optimization";
    case withdrawal::OperationCategory::software_optimization:
      return "software_optimization";
  }
  return "invalid";
}

[[nodiscard]] std::string action_name(withdrawal::NoticeAction action) {
  switch (action) {
    case withdrawal::NoticeAction::withdraw:
      return "withdraw";
    case withdrawal::NoticeAction::release:
      return "release";
  }
  return "invalid";
}

[[nodiscard]] std::string authorization_name(
    OperationAuthorizationCode code) {
  switch (code) {
    case OperationAuthorizationCode::allowed:
      return "allowed";
    case OperationAuthorizationCode::allowed_unknown:
      return "allowed_unknown";
    case OperationAuthorizationCode::blocked:
      return "blocked";
    case OperationAuthorizationCode::invalid_request:
      return "invalid_request";
    case OperationAuthorizationCode::read_only:
      return "read_only";
    case OperationAuthorizationCode::failed:
      return "failed";
  }
  return "failed";
}

[[nodiscard]] ExecutionResult authorization_execution_result(
    OperationAuthorizationCode code) {
  switch (code) {
    case OperationAuthorizationCode::allowed:
      return ExecutionResult::succeeded;
    case OperationAuthorizationCode::allowed_unknown:
      return ExecutionResult::unknown;
    case OperationAuthorizationCode::blocked:
    case OperationAuthorizationCode::invalid_request:
    case OperationAuthorizationCode::read_only:
    case OperationAuthorizationCode::failed:
      return ExecutionResult::failed;
  }
  return ExecutionResult::failed;
}

[[nodiscard]] bool check_succeeded(EmergencyWithdrawalCheckCode code) {
  return code == EmergencyWithdrawalCheckCode::updated ||
         code == EmergencyWithdrawalCheckCode::stale_ignored;
}

void append_matching_notice_fields(
    std::vector<DiagnosticField>& fields,
    OperationAuthorizationResult const& result) {
  if (!result.matching_notice.has_value()) {
    return;
  }
  auto const& entry = *result.matching_notice;
  fields.push_back({"notice_source", result.notice_source,
                    DiagnosticValueDisposition::retain});
  fields.push_back({"affected_stable_id", entry.stable_id,
                    DiagnosticValueDisposition::retain});
  fields.push_back({"affected_category", category_name(entry.category),
                    DiagnosticValueDisposition::retain});
  fields.push_back({"affected_version_min", entry.affected_versions.minimum,
                    DiagnosticValueDisposition::retain});
  fields.push_back({"affected_version_max", entry.affected_versions.maximum,
                    DiagnosticValueDisposition::retain});
  fields.push_back({"withdrawal_action", action_name(entry.action),
                    DiagnosticValueDisposition::retain});
  fields.push_back({"withdrawal_reason", entry.reason,
                    DiagnosticValueDisposition::retain});
  if (result.observed_at.has_value()) {
    fields.push_back({"notice_observed_at_ms",
                      std::to_string(
                          result.observed_at->time_since_epoch().count()),
                      DiagnosticValueDisposition::retain});
  }
}

[[nodiscard]] std::vector<DiagnosticField> notice_fields(
    withdrawal::EmergencyWithdrawalNotice const& notice,
    std::string_view check_result, std::optional<WallClockTime> observed_at) {
  std::vector<DiagnosticField> fields{
      {"notice_check", std::string{check_result},
       DiagnosticValueDisposition::retain},
      {"notice_source", notice.source, DiagnosticValueDisposition::retain},
      {"notice_revision", std::to_string(notice.revision),
       DiagnosticValueDisposition::retain},
      {"notice_published_at_ms",
       std::to_string(notice.published_at.time_since_epoch().count()),
       DiagnosticValueDisposition::retain},
      {"notice_entry_count", std::to_string(notice.entries.size()),
       DiagnosticValueDisposition::retain},
  };
  if (observed_at.has_value()) {
    fields.push_back({"notice_observed_at_ms",
                      std::to_string(observed_at->time_since_epoch().count()),
                      DiagnosticValueDisposition::retain});
  }
  for (auto const& entry : notice.entries) {
    fields.push_back({"affected_stable_id", entry.stable_id,
                      DiagnosticValueDisposition::retain});
    fields.push_back({"affected_category", category_name(entry.category),
                      DiagnosticValueDisposition::retain});
    fields.push_back({"affected_version_min", entry.affected_versions.minimum,
                      DiagnosticValueDisposition::retain});
    fields.push_back({"affected_version_max", entry.affected_versions.maximum,
                      DiagnosticValueDisposition::retain});
    fields.push_back({"withdrawal_action", action_name(entry.action),
                      DiagnosticValueDisposition::retain});
    fields.push_back({"withdrawal_reason", entry.reason,
                      DiagnosticValueDisposition::retain});
  }
  return fields;
}

}  // namespace

EmergencyWithdrawalService::EmergencyWithdrawalService(
    DeviceStateStore& states, Clock const& clock,
    EmergencyWithdrawalNoticeSource& source,
    EmergencyWithdrawalLogContext log) noexcept
    : states_(states),
      clock_(clock),
      source_(source),
      log_(log.log),
      correlation_(std::move(log.correlation)) {}

domain::StateKey EmergencyWithdrawalService::state_key() const {
  return emergency_key();
}

EmergencyWithdrawalSnapshot EmergencyWithdrawalService::load_snapshot(
    StateRead* read_result) {
  auto read = states_.inspect(state_key());
  if (read_result != nullptr) *read_result = read;
  return load_snapshot(read);
}

EmergencyWithdrawalSnapshot EmergencyWithdrawalService::load_snapshot(
    StateRead const& read) const {
  if (read.mode == StateReadMode::uninitialized) return {};
  if (read.mode == StateReadMode::read_only_corrupt ||
      read.mode == StateReadMode::read_only_future) {
    EmergencyWithdrawalSnapshot result;
    result.state = EmergencyWithdrawalServiceState::read_only;
    result.error = read.error;
    return result;
  }
  if (read.mode != StateReadMode::writable &&
      read.mode != StateReadMode::recovered_previous) {
    EmergencyWithdrawalSnapshot result;
    result.state = EmergencyWithdrawalServiceState::failed;
    result.error = read.error;
    return result;
  }
  if (!read.snapshot.has_value()) {
    EmergencyWithdrawalSnapshot result;
    result.state = EmergencyWithdrawalServiceState::failed;
    result.error = "state store returned no emergency withdrawal snapshot";
    return result;
  }
  auto decoded = decode_snapshot(read.snapshot->state.value.payload);
  if (!decoded.has_value()) {
    EmergencyWithdrawalSnapshot result;
    result.state = EmergencyWithdrawalServiceState::read_only;
    result.error = "emergency withdrawal payload is invalid";
    return result;
  }
  decoded->state = EmergencyWithdrawalServiceState::cached;
  decoded->possibly_stale = true;
  return *decoded;
}

StateCommitResult EmergencyWithdrawalService::persist(
    EmergencyWithdrawalSnapshot const& snapshot,
    std::optional<domain::RevisionToken> expected_revision) {
  auto payload = encode_snapshot(snapshot);
  if (!payload.has_value()) {
    return {.status = StateCommitStatus::failed,
            .error = "emergency withdrawal state exceeds persistence limits"};
  }
  auto state = empty_state(std::move(*payload));
  if (!expected_revision.has_value()) {
    return states_.initialize(state_key(), std::move(state));
  }
  return states_.commit({.key = state_key(),
                         .expected_revision = *expected_revision,
                         .state = std::move(state)});
}

bool EmergencyWithdrawalService::can_persist(
    EmergencyWithdrawalSnapshot const& snapshot) const {
  return encode_snapshot(snapshot).has_value();
}

bool EmergencyWithdrawalService::has_version_scoped_withdrawal(
    EmergencyWithdrawalSnapshot const& snapshot,
    withdrawal::OperationTarget const& target) const noexcept {
  return std::ranges::any_of(snapshot.notices, [&](auto const& notice) {
    return std::ranges::any_of(notice.entries, [&](auto const& entry) {
      return entry.action == withdrawal::NoticeAction::withdraw &&
             entry.stable_id == target.stable_id &&
             entry.category == target.category &&
             (!entry.affected_versions.minimum.empty() ||
              !entry.affected_versions.maximum.empty());
    });
  });
}

EmergencyWithdrawalSnapshot EmergencyWithdrawalService::with_transient_safety(
    EmergencyWithdrawalSnapshot snapshot) const {
  if (unpersisted_withdrawals_.empty()) {
    return snapshot;
  }
  snapshot.state = EmergencyWithdrawalServiceState::unpersisted;
  snapshot.possibly_stale = true;
  snapshot.persistence_pending = true;
  snapshot.error = unpersisted_error_;
  if (!snapshot.last_observed_at.has_value()) {
    snapshot.last_observed_at = unpersisted_withdrawals_.front().observed_at;
  }
  return snapshot;
}

void EmergencyWithdrawalService::retain_unpersisted_withdrawals(
    withdrawal::EmergencyWithdrawalNotice const& notice, std::string error) {
  auto const observed_at = clock_.now();
  for (auto const& entry : notice.entries) {
    if (entry.action == withdrawal::NoticeAction::withdraw) {
      auto const known = std::ranges::any_of(
          unpersisted_withdrawals_, [&](auto const& existing) {
            return existing.source == notice.source &&
                   existing.revision == notice.revision &&
                   existing.entry == entry;
          });
      if (!known) {
        unpersisted_withdrawals_.push_back(
            {.entry = entry,
             .source = notice.source,
             .revision = notice.revision,
             .observed_at = observed_at});
      }
    }
  }
  unpersisted_error_ = std::move(error);
}

void EmergencyWithdrawalService::clear_unpersisted_withdrawals(
    withdrawal::EmergencyWithdrawalNotice const& notice) {
  auto const latest = std::ranges::max_element(
      unpersisted_withdrawals_, {}, [&](auto const& withdrawal) {
        return withdrawal.source == notice.source ? withdrawal.revision : 0;
      });
  if (latest == unpersisted_withdrawals_.end() ||
      latest->source != notice.source || latest->revision > notice.revision) {
    return;
  }
  if (latest->revision == notice.revision) {
    auto const contains_every_withdrawal = std::ranges::all_of(
        unpersisted_withdrawals_, [&](auto const& withdrawal) {
          if (withdrawal.source != notice.source ||
              withdrawal.revision != notice.revision) {
            return true;
          }
          return std::ranges::any_of(notice.entries, [&](auto const& entry) {
            return entry == withdrawal.entry;
          });
        });
    if (!contains_every_withdrawal) {
      return;
    }
  }
  std::erase_if(unpersisted_withdrawals_, [&](auto const& withdrawal) {
    return withdrawal.source == notice.source;
  });
  if (unpersisted_withdrawals_.empty()) {
    unpersisted_error_.clear();
  }
}

std::optional<EmergencyWithdrawalService::TransientWithdrawal>
EmergencyWithdrawalService::matching_unpersisted_withdrawal(
    withdrawal::OperationTarget const& target) const {
  auto const found = std::ranges::find_if(
      unpersisted_withdrawals_, [&](auto const& withdrawal) {
        return entry_matches(withdrawal.entry, target);
      });
  if (found == unpersisted_withdrawals_.end()) {
    return std::nullopt;
  }
  return *found;
}

std::optional<EmergencyWithdrawalService::TransientWithdrawal>
EmergencyWithdrawalService::version_scoped_unpersisted_withdrawal(
    withdrawal::OperationTarget const& target) const {
  auto const found = std::ranges::find_if(
      unpersisted_withdrawals_, [&](auto const& withdrawal) {
        return withdrawal.entry.stable_id == target.stable_id &&
               withdrawal.entry.category == target.category &&
               (!withdrawal.entry.affected_versions.minimum.empty() ||
                !withdrawal.entry.affected_versions.maximum.empty());
      });
  if (found == unpersisted_withdrawals_.end()) {
    return std::nullopt;
  }
  return *found;
}

EmergencyWithdrawalCheckResult EmergencyWithdrawalService::persist_notice(
    domain::emergency_withdrawal::EmergencyWithdrawalNotice notice) {
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    auto candidate = notice;
    StateRead read;
    auto current = load_snapshot(&read);
    auto const authoritative = current;
    if (current.state == EmergencyWithdrawalServiceState::read_only) {
      retain_unpersisted_withdrawals(
          notice, "emergency withdrawal state is read-only");
      return {.code = EmergencyWithdrawalCheckCode::read_only,
              .snapshot = with_transient_safety(std::move(current)),
              .error = "emergency withdrawal state is read-only"};
    }
    if (current.state == EmergencyWithdrawalServiceState::failed) {
      auto error = current.error;
      retain_unpersisted_withdrawals(notice, error);
      return {.code = EmergencyWithdrawalCheckCode::failed,
              .snapshot = with_transient_safety(std::move(current)),
              .error = std::move(error)};
    }
    auto found = std::ranges::find_if(current.notices, [&](auto const& existing) {
      return existing.source == notice.source;
    });
    auto set_observed_at = [&](EmergencyWithdrawalSnapshot& snapshot) {
      snapshot.last_observed_at = clock_.now();
    };
    if (found != current.notices.end() && notice.revision <= found->revision) {
      if (notice.revision == found->revision && notice != *found) {
        current.state = EmergencyWithdrawalServiceState::cached;
        current.possibly_stale = true;
        current.error =
            "same emergency withdrawal revision has conflicting content";
        auto error = current.error;
        return {.code = EmergencyWithdrawalCheckCode::rejected,
                .snapshot = std::move(current),
                .error = std::move(error)};
      }
      current.state = EmergencyWithdrawalServiceState::fresh;
      current.possibly_stale = false;
      set_observed_at(current);
      if (!can_persist(current)) {
        auto failed = authoritative;
        failed.state = EmergencyWithdrawalServiceState::failed;
        failed.error = "emergency withdrawal state exceeds persistence limits";
        retain_unpersisted_withdrawals(notice, failed.error);
        return {.code = EmergencyWithdrawalCheckCode::failed,
                .snapshot = with_transient_safety(std::move(failed)),
                .error = "emergency withdrawal state exceeds persistence limits"};
      }
      auto committed = persist(current, read.snapshot.has_value()
                                            ? std::optional{read.snapshot->revision}
                                            : std::nullopt);
      if (committed.status == StateCommitStatus::conflict) continue;
      if (committed.status == StateCommitStatus::outcome_unknown) {
        auto confirmed = load_snapshot();
        if (confirmed.notices == current.notices &&
            confirmed.last_observed_at == current.last_observed_at) {
          confirmed.state = EmergencyWithdrawalServiceState::fresh;
          confirmed.possibly_stale = false;
          clear_unpersisted_withdrawals(notice);
          return {.code = EmergencyWithdrawalCheckCode::stale_ignored,
                  .snapshot = with_transient_safety(std::move(confirmed))};
        }
        auto failed = authoritative;
        failed.state = EmergencyWithdrawalServiceState::failed;
        failed.error = committed.error.empty()
                           ? "emergency withdrawal persistence outcome is unknown"
                           : committed.error;
        auto error = failed.error;
        retain_unpersisted_withdrawals(notice, error);
        return {.code = EmergencyWithdrawalCheckCode::outcome_unknown,
                .snapshot = with_transient_safety(std::move(failed)),
                .error = std::move(error)};
      }
      if (committed.status != StateCommitStatus::committed) {
        auto failed = authoritative;
        failed.state = committed.status == StateCommitStatus::read_only
                           ? EmergencyWithdrawalServiceState::read_only
                           : EmergencyWithdrawalServiceState::failed;
        failed.error = committed.error;
        retain_unpersisted_withdrawals(notice, failed.error);
        return {.code = committed.status == StateCommitStatus::read_only
                             ? EmergencyWithdrawalCheckCode::read_only
                             : EmergencyWithdrawalCheckCode::failed,
                .snapshot = with_transient_safety(std::move(failed)),
                .error = committed.error};
      }
      clear_unpersisted_withdrawals(notice);
      return {.code = EmergencyWithdrawalCheckCode::stale_ignored,
              .snapshot = with_transient_safety(std::move(current))};
    }
    if (found == current.notices.end()) {
      current.notices.push_back(std::move(candidate));
    } else {
      // Each source revision is a complete, authoritative source snapshot.
      // Replacing it lets a newer revision safely narrow or release prior rules.
      *found = std::move(candidate);
    }
    current.state = EmergencyWithdrawalServiceState::fresh;
    current.possibly_stale = false;
    set_observed_at(current);
    if (!can_persist(current)) {
      auto failed = authoritative;
      failed.state = EmergencyWithdrawalServiceState::failed;
      failed.error = "emergency withdrawal state exceeds persistence limits";
      retain_unpersisted_withdrawals(notice, failed.error);
      return {.code = EmergencyWithdrawalCheckCode::failed,
              .snapshot = with_transient_safety(std::move(failed)),
              .error = "emergency withdrawal state exceeds persistence limits"};
    }
    auto committed = persist(current, read.snapshot.has_value()
                                         ? std::optional{read.snapshot->revision}
                                         : std::nullopt);
    if (committed.status == StateCommitStatus::conflict) continue;
    if (committed.status == StateCommitStatus::outcome_unknown) {
      auto confirmed = load_snapshot();
      if (confirmed.notices == current.notices &&
          confirmed.last_observed_at == current.last_observed_at) {
        confirmed.state = EmergencyWithdrawalServiceState::fresh;
        confirmed.possibly_stale = false;
        clear_unpersisted_withdrawals(notice);
        return {.code = EmergencyWithdrawalCheckCode::updated,
                .snapshot = with_transient_safety(std::move(confirmed))};
      }
      auto failed = authoritative;
      failed.state = EmergencyWithdrawalServiceState::failed;
      failed.error = committed.error.empty()
                         ? "emergency withdrawal persistence outcome is unknown"
                         : committed.error;
      auto error = failed.error;
      retain_unpersisted_withdrawals(notice, error);
      return {.code = EmergencyWithdrawalCheckCode::outcome_unknown,
              .snapshot = with_transient_safety(std::move(failed)),
              .error = std::move(error)};
    }
    if (committed.status != StateCommitStatus::committed) {
      auto failed = authoritative;
      failed.state = committed.status == StateCommitStatus::read_only
                         ? EmergencyWithdrawalServiceState::read_only
                         : EmergencyWithdrawalServiceState::failed;
      failed.error = committed.error;
      retain_unpersisted_withdrawals(notice, failed.error);
      return {.code = committed.status == StateCommitStatus::read_only
                           ? EmergencyWithdrawalCheckCode::read_only
                           : EmergencyWithdrawalCheckCode::failed,
              .snapshot = with_transient_safety(std::move(failed)),
              .error = committed.error};
    }
    clear_unpersisted_withdrawals(notice);
    return {.code = EmergencyWithdrawalCheckCode::updated,
            .snapshot = with_transient_safety(std::move(current))};
  }
  auto snapshot = load_snapshot();
  snapshot.state = EmergencyWithdrawalServiceState::failed;
  snapshot.error = "emergency withdrawal state changed during notice acceptance";
  retain_unpersisted_withdrawals(notice, snapshot.error);
  return {.code = EmergencyWithdrawalCheckCode::failed,
          .snapshot = with_transient_safety(std::move(snapshot)),
          .error = "emergency withdrawal state changed during notice acceptance"};
}

EmergencyWithdrawalCheckResult EmergencyWithdrawalService::check_locked() {
  StateRead read;
  auto current = load_snapshot(&read);
  if (current.state == EmergencyWithdrawalServiceState::read_only) {
    return {.code = EmergencyWithdrawalCheckCode::read_only,
            .snapshot = with_transient_safety(std::move(current)),
            .error = "emergency withdrawal state is read-only"};
  }
  if (current.state == EmergencyWithdrawalServiceState::failed) {
    auto error = current.error;
    return {.code = EmergencyWithdrawalCheckCode::failed,
            .snapshot = with_transient_safety(std::move(current)),
            .error = std::move(error)};
  }
  auto fetched = source_.fetch();
  if (!fetched.succeeded) {
    if (log_ != nullptr) {
      auto const receipt = log_->append(
          correlation_,
          ExecutionEvent{.kind = ExecutionEventKind::adapter_result,
                         .component = "emergency-withdrawal",
                         .stage = "fetch-notice",
                         .result = ExecutionResult::failed,
                         .error = ExecutionError{.source = "notice-source",
                                                  .message = fetched.error},
                         .fields = {{"notice_check", "failed",
                                     DiagnosticValueDisposition::retain},
                                    {"notice_observed_at_ms",
                                     std::to_string(
                                         clock_.now().time_since_epoch().count()),
                                     DiagnosticValueDisposition::retain}}});
      if (!receipt.persisted) {
        current.error = receipt.error;
      }
    }
    if (!current.notices.empty()) {
      current.state = EmergencyWithdrawalServiceState::cached;
      current.possibly_stale = true;
      current.error = fetched.error;
      return {.code = EmergencyWithdrawalCheckCode::cached,
              .snapshot = with_transient_safety(std::move(current)),
              .error = fetched.error};
    }
    current.state = EmergencyWithdrawalServiceState::unknown;
    current.possibly_stale = true;
    current.error = fetched.error;
    return {.code = EmergencyWithdrawalCheckCode::unknown,
            .snapshot = with_transient_safety(std::move(current)),
            .error = fetched.error};
  }
  auto parsed = domain::emergency_withdrawal::parse_notice(fetched.document);
  if (!parsed.succeeded()) {
    if (log_ != nullptr) {
      (void)log_->append(
          correlation_,
          ExecutionEvent{.kind = ExecutionEventKind::state_transition,
                         .component = "emergency-withdrawal",
                         .stage = "parse-notice",
                         .result = ExecutionResult::failed,
                         .error = ExecutionError{.source = "notice-parser",
                                                  .message = parsed.detail},
                         .fields = {{"notice_check", "rejected",
                                     DiagnosticValueDisposition::retain},
                                    {"notice_observed_at_ms",
                                     std::to_string(
                                         clock_.now().time_since_epoch().count()),
                                     DiagnosticValueDisposition::retain}}});
    }
    current.error = parsed.detail;
    if (!current.notices.empty()) {
      current.state = EmergencyWithdrawalServiceState::cached;
      current.possibly_stale = true;
      return {.code = EmergencyWithdrawalCheckCode::rejected,
              .snapshot = with_transient_safety(std::move(current)),
              .error = parsed.detail};
    }
    current.state = EmergencyWithdrawalServiceState::unknown;
    current.possibly_stale = true;
    return {.code = EmergencyWithdrawalCheckCode::rejected,
            .snapshot = with_transient_safety(std::move(current)),
            .error = parsed.detail};
  }

  auto notice = std::move(*parsed.notice);
  auto result = persist_notice(notice);
  if (log_ != nullptr) {
    auto const receipt = log_->append(
        correlation_,
        ExecutionEvent{.kind = ExecutionEventKind::state_transition,
                       .component = "emergency-withdrawal",
                       .stage = "apply-notice",
                       .result = check_succeeded(result.code)
                                     ? ExecutionResult::succeeded
                                     : ExecutionResult::failed,
                       .error = result.error.empty()
                                    ? std::nullopt
                                    : std::optional<ExecutionError>{
                                          ExecutionError{
                                              .source = "emergency-withdrawal",
                                              .message = result.error}},
                       .fields = notice_fields(
                           notice,
                           result.code == EmergencyWithdrawalCheckCode::updated
                               ? "updated"
                               : result.code == EmergencyWithdrawalCheckCode::stale_ignored
                                     ? "stale_ignored"
                                     : "not-updated",
                           result.snapshot.last_observed_at)});
    if (!receipt.persisted && result.error.empty()) {
      result.error = receipt.error;
    }
  }
  return result;
}

EmergencyWithdrawalCheckResult
EmergencyWithdrawalService::preflight_execution_failure() noexcept {
  EmergencyWithdrawalCheckResult result{
      .code = EmergencyWithdrawalCheckCode::failed,
      .snapshot = {.state = EmergencyWithdrawalServiceState::failed,
                   .possibly_stale = true},
  };
  try {
    result.error = "emergency withdrawal preflight raised an exception";
    result.snapshot.error = result.error;
    if (log_ != nullptr) {
      static_cast<void>(log_->append(
          correlation_,
          ExecutionEvent{.kind = ExecutionEventKind::adapter_result,
                         .component = "emergency-withdrawal",
                         .stage = "preflight-exception",
                         .result = ExecutionResult::failed,
                         .error = ExecutionError{
                             .source = "emergency-withdrawal",
                             .message = result.error},
                         .fields = {{"notice_check", "failed",
                                     DiagnosticValueDisposition::retain}}}));
    }
  } catch (...) {
    // Preserve the typed failure even when diagnostic recording also fails.
  }
  return result;
}

EmergencyWithdrawalCheckResult EmergencyWithdrawalService::preflight_check() {
  try {
    std::scoped_lock lock{mutex_};
    return check_locked();
  } catch (...) {
    return preflight_execution_failure();
  }
}

EmergencyWithdrawalCheckResult
EmergencyWithdrawalService::report_preflight_execution_exception() noexcept {
  return preflight_execution_failure();
}

EmergencyWithdrawalCheckResult EmergencyWithdrawalService::check() {
  std::scoped_lock lock{mutex_};
  return check_locked();
}

EmergencyWithdrawalSnapshot EmergencyWithdrawalService::snapshot() {
  std::scoped_lock lock{mutex_};
  return with_transient_safety(load_snapshot());
}

OperationAuthorizationResult EmergencyWithdrawalService::authorize(
    domain::emergency_withdrawal::OperationTarget const& target) {
  std::scoped_lock lock{mutex_};
  StateRead read;
  auto current = load_snapshot(&read);
  auto record = [&](OperationAuthorizationResult result) {
    if (log_ != nullptr) {
      std::vector<DiagnosticField> fields{
          {"authorization", authorization_name(result.code),
           DiagnosticValueDisposition::retain},
          {"operation_stable_id", target.stable_id,
           DiagnosticValueDisposition::retain},
          {"operation_category", category_name(target.category),
           DiagnosticValueDisposition::retain},
          {"operation_version", target.version.value_or("unknown"),
           DiagnosticValueDisposition::retain},
      };
      if (result.notice_revision != 0) {
        fields.push_back({"notice_revision",
                          std::to_string(result.notice_revision),
                          DiagnosticValueDisposition::retain});
      }
      append_matching_notice_fields(fields, result);
      auto const receipt = log_->append(
          correlation_,
          ExecutionEvent{.kind = ExecutionEventKind::state_transition,
                         .component = "emergency-withdrawal",
                         .stage = "authorize",
                         .result = authorization_execution_result(result.code),
                         .error = result.reason.empty()
                                      ? std::nullopt
                                      : std::optional<ExecutionError>{
                                            ExecutionError{
                                                .source = "emergency-withdrawal",
                                                .message = result.reason}},
                         .fields = std::move(fields)});
      if (!receipt.persisted && result.reason.empty()) {
        result.reason = receipt.error;
      }
    }
    return result;
  };
  if (!target.valid()) {
    return record({.code = OperationAuthorizationCode::invalid_request,
                   .observed_at = current.last_observed_at,
                   .reason = "emergency withdrawal target is invalid",
                   .possibly_stale = current.possibly_stale});
  }
  if (auto transient = matching_unpersisted_withdrawal(target);
      transient.has_value()) {
    return record({.code = OperationAuthorizationCode::blocked,
                   .matching_notice = transient->entry,
                   .notice_source = transient->source,
                   .observed_at = transient->observed_at,
                   .reason = transient->entry.reason,
                   .notice_revision = transient->revision,
                   .possibly_stale = true});
  }
  if (!target.version.has_value()) {
    if (auto transient = version_scoped_unpersisted_withdrawal(target);
        transient.has_value()) {
      return record(
          {.code = OperationAuthorizationCode::blocked,
           .matching_notice = transient->entry,
           .notice_source = transient->source,
           .observed_at = transient->observed_at,
           .reason =
               "operation version is required while a version-scoped emergency withdrawal is active",
           .notice_revision = transient->revision,
           .possibly_stale = true});
    }
  }
  if (current.state == EmergencyWithdrawalServiceState::read_only) {
    return record({.code = OperationAuthorizationCode::read_only,
                   .observed_at = current.last_observed_at,
                   .reason = current.error,
                   .possibly_stale = current.possibly_stale});
  }
  if (current.state == EmergencyWithdrawalServiceState::failed) {
    return record({.code = OperationAuthorizationCode::failed,
                   .observed_at = current.last_observed_at,
                   .reason = current.error,
                   .possibly_stale = current.possibly_stale});
  }
  for (auto const& notice : current.notices) {
    for (auto const& entry : notice.entries) {
      if (entry_matches(entry, target)) {
        return record({.code = OperationAuthorizationCode::blocked,
                       .matching_notice = entry,
                       .notice_source = notice.source,
                       .observed_at = current.last_observed_at,
                       .reason = entry.reason,
                       .notice_revision = notice.revision,
                       .possibly_stale = current.possibly_stale});
      }
    }
  }
  if (!target.version.has_value() &&
      has_version_scoped_withdrawal(current, target)) {
    auto const matching = std::ranges::find_if(
        current.notices, [&](auto const& notice) {
          return std::ranges::any_of(notice.entries, [&](auto const& entry) {
            return entry.action == withdrawal::NoticeAction::withdraw &&
                   entry.stable_id == target.stable_id &&
                   entry.category == target.category &&
                   (!entry.affected_versions.minimum.empty() ||
                    !entry.affected_versions.maximum.empty());
          });
        });
    auto const matching_entry = std::ranges::find_if(
        matching->entries, [&](auto const& entry) {
          return entry.action == withdrawal::NoticeAction::withdraw &&
                 entry.stable_id == target.stable_id &&
                 entry.category == target.category &&
                 (!entry.affected_versions.minimum.empty() ||
                  !entry.affected_versions.maximum.empty());
        });
    return record({.code = OperationAuthorizationCode::blocked,
                   .matching_notice = *matching_entry,
                   .notice_source = matching->source,
                   .observed_at = current.last_observed_at,
                   .reason =
                       "operation version is required while a version-scoped emergency withdrawal is active",
                   .notice_revision = matching->revision,
                   .possibly_stale = current.possibly_stale});
  }
  auto const unknown = current.state == EmergencyWithdrawalServiceState::unknown ||
                       current.notices.empty() ||
                       !unpersisted_withdrawals_.empty();
  return record({.code = unknown ? OperationAuthorizationCode::allowed_unknown
                                 : OperationAuthorizationCode::allowed,
                 .observed_at = current.last_observed_at,
                 .reason = unknown ? "latest emergency withdrawal notice is unknown"
                                   : "no matching emergency withdrawal",
                 .possibly_stale = current.possibly_stale ||
                                   !unpersisted_withdrawals_.empty()});
}

}  // namespace azzs::application
