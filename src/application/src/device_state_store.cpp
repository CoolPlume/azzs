#include "azzs/application/device_state_store.hpp"

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

namespace azzs::application {
namespace {

constexpr std::uint32_t k_envelope_version = 1;
constexpr std::uint32_t k_checkpoint_format_version = 2;
constexpr std::uint32_t k_current_schema = 2;
constexpr std::uint32_t k_previous_schema = 1;
constexpr std::size_t k_max_state_bytes = 64U * 1024U * 1024U;
constexpr std::array<std::byte, 8> k_state_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'S'}, std::byte{'T'}, std::byte{'0'}, std::byte{'2'},
};
constexpr std::array<std::byte, 8> k_intent_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'I'}, std::byte{'N'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::array<std::byte, 8> k_checkpoint_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'C'}, std::byte{'P'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::array<std::byte, 8> k_consumed_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'C'}, std::byte{'X'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::array<std::byte, 8> k_archive_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'A'}, std::byte{'R'}, std::byte{'0'}, std::byte{'1'},
};

[[nodiscard]] std::uint64_t digest(std::span<std::byte const> bytes) noexcept {
  // CRC-64/ECMA-182 detects accidental corruption. It is an integrity check,
  // not an authenticity or hostile-tampering boundary.
  std::uint64_t value{};
  for (auto const byte : bytes) {
    value ^= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(byte))
             << 56;
    for (unsigned bit = 0; bit < 8; ++bit) {
      value = (value & 0x8000000000000000ULL) != 0
                  ? (value << 1) ^ 0x42f0e1eba9ea3693ULL
                  : value << 1;
    }
  }
  return value;
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

  void raw(std::span<std::byte const> value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  template <std::size_t Size>
  void raw(std::array<std::byte, Size> const& value) {
    raw(std::span<std::byte const>{value});
  }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (auto const character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }

  [[nodiscard]] domain::StateBytes finish_with_digest() {
    u64(digest(bytes_));
    return std::move(bytes_);
  }

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

  [[nodiscard]] bool i64(std::int64_t& value) {
    std::uint64_t encoded{};
    if (!u64(encoded)) {
      return false;
    }
    value = static_cast<std::int64_t>(encoded);
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

  [[nodiscard]] bool bytes(
      std::size_t size, domain::StateBytes& value,
      std::size_t maximum = k_max_state_bytes) {
    if (size > maximum || remaining() < size) {
      return false;
    }
    value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                 bytes_.begin() +
                     static_cast<std::ptrdiff_t>(position_ + size));
    position_ += size;
    return true;
  }

  [[nodiscard]] bool text(std::string& value) {
    std::uint32_t size{};
    if (!u32(size) || size > k_max_state_bytes || remaining() < size) {
      return false;
    }
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
      std::uint8_t character{};
      if (!u8(character)) {
        return false;
      }
      value.push_back(static_cast<char>(character));
    }
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

 private:
  std::span<std::byte const> bytes_;
  std::size_t position_{};
};

[[nodiscard]] bool has_valid_digest(std::span<std::byte const> bytes) {
  if (bytes.size() < sizeof(std::uint64_t)) {
    return false;
  }
  Decoder tail{bytes.last(sizeof(std::uint64_t))};
  std::uint64_t stored{};
  return tail.u64(stored) &&
         stored == digest(bytes.first(bytes.size() - sizeof(std::uint64_t)));
}

[[nodiscard]] bool read_magic(Decoder& decoder,
                              std::span<std::byte const> expected) {
  domain::StateBytes actual(expected.size());
  return decoder.raw(actual) &&
         std::equal(actual.begin(), actual.end(), expected.begin(),
                    expected.end());
}

void encode_revision(Encoder& encoder, domain::RevisionToken const& revision) {
  encoder.raw(revision.epoch);
  encoder.u64(revision.generation);
  encoder.u64(revision.content_digest);
}

[[nodiscard]] bool decode_revision(Decoder& decoder,
                                   domain::RevisionToken& revision) {
  return decoder.raw(revision.epoch) && decoder.u64(revision.generation) &&
         decoder.u64(revision.content_digest);
}

enum class DecodeState {
  supported,
  future,
  invalid,
};

struct DecodedState final {
  DecodeState state{DecodeState::invalid};
  std::optional<domain::DeviceStateSnapshot> snapshot;
  std::string error;
};

[[nodiscard]] domain::StateBytes encode_state(
    domain::DeviceStateSnapshot snapshot) {
  Encoder encoder;
  encoder.raw(k_state_magic);
  encoder.u32(k_envelope_version);
  encoder.u8(static_cast<std::uint8_t>(snapshot.key.partition));
  encoder.text(snapshot.key.aggregate.value);
  encoder.text(snapshot.key.subject.has_value() ? snapshot.key.subject->value
                                                : std::string_view{});
  encoder.u32(snapshot.state.value.schema);
  encoder.u32(snapshot.state.value.minimum_reader);
  encoder.u32(snapshot.state.value.minimum_writer);
  encoder.raw(snapshot.revision.epoch);
  encoder.u64(snapshot.revision.generation);
  encoder.u32(
      static_cast<std::uint32_t>(snapshot.state.reinitializations.size()));
  for (auto const& audit : snapshot.state.reinitializations) {
    encoder.i64(audit.confirmed_at_milliseconds);
    encoder.text(audit.confirmation_reference);
    encoder.u32(
        static_cast<std::uint32_t>(audit.affected_categories.size()));
    for (auto const category : audit.affected_categories) {
      encoder.u8(static_cast<std::uint8_t>(category));
    }
  }
  encoder.u64(snapshot.state.value.payload.size());
  encoder.raw(snapshot.state.value.payload);
  auto bytes = encoder.finish_with_digest();
  snapshot.revision.content_digest = digest(
      std::span<std::byte const>{bytes}.first(bytes.size() - sizeof(std::uint64_t)));
  return bytes;
}

[[nodiscard]] DecodedState decode_state(
    std::span<std::byte const> bytes, domain::StateKey const& expected_key) {
  if (bytes.size() > k_max_state_bytes || !has_valid_digest(bytes)) {
    return {.error = "state envelope integrity check failed"};
  }

  Decoder decoder{bytes.first(bytes.size() - sizeof(std::uint64_t))};
  std::uint32_t envelope_version{};
  std::uint8_t partition{};
  std::string aggregate;
  std::string subject;
  std::uint32_t schema{};
  std::uint32_t minimum_reader{};
  std::uint32_t minimum_writer{};
  domain::RevisionToken revision;
  std::uint32_t audit_count{};
  if (!read_magic(decoder, k_state_magic) ||
      !decoder.u32(envelope_version)) {
    return {.error = "state envelope header is invalid"};
  }
  if (envelope_version > k_envelope_version) {
    return {.state = DecodeState::future,
            .error = "state envelope version is newer than this workbench"};
  }
  if (envelope_version != k_envelope_version || !decoder.u8(partition) ||
      !decoder.text(aggregate) || !decoder.text(subject) ||
      !decoder.u32(schema) || !decoder.u32(minimum_reader) ||
      !decoder.u32(minimum_writer) || !decoder.raw(revision.epoch) ||
      !decoder.u64(revision.generation) || !decoder.u32(audit_count)) {
    return {.error = "state envelope fields are truncated"};
  }

  domain::StateKey decoded_key{
      .partition = static_cast<domain::StatePartition>(partition),
      .aggregate = domain::AggregateId{std::move(aggregate)},
  };
  if (!subject.empty()) {
    decoded_key.subject = domain::StateSubject{std::move(subject)};
  }
  if (!decoded_key.valid() || decoded_key != expected_key ||
      revision.generation == 0) {
    return {.error = "state envelope identity is invalid"};
  }

  domain::DeviceState state{
      .value = {.schema = schema,
                .minimum_reader = minimum_reader,
                .minimum_writer = minimum_writer},
  };
  if (audit_count > 1024) {
    return {.error = "state audit count exceeds the supported limit"};
  }
  state.reinitializations.reserve(audit_count);
  for (std::uint32_t index = 0; index < audit_count; ++index) {
    domain::ReinitializationAudit audit;
    std::uint32_t category_count{};
    if (!decoder.i64(audit.confirmed_at_milliseconds) ||
        !decoder.text(audit.confirmation_reference) ||
        audit.confirmation_reference.empty() ||
        audit.confirmation_reference.size() > 256 ||
        !std::ranges::all_of(
            audit.confirmation_reference, [](unsigned char byte) {
              return byte >= 0x20 && byte < 0x7f;
            }) ||
        !decoder.u32(category_count) || category_count == 0 ||
        category_count > 64) {
      return {.error = "state reinitialization audit is invalid"};
    }
    audit.affected_categories.reserve(category_count);
    for (std::uint32_t category = 0; category < category_count; ++category) {
      std::uint8_t value{};
      if (!decoder.u8(value)) {
        return {.error = "state reinitialization category is truncated"};
      }
      if (value < static_cast<std::uint8_t>(
                      domain::DataImpactCategory::recovery_records) ||
          value > static_cast<std::uint8_t>(
                      domain::DataImpactCategory::emergency_withdrawals)) {
        return {.error = "state reinitialization category is unknown"};
      }
      audit.affected_categories.push_back(
          static_cast<domain::DataImpactCategory>(value));
    }
    state.reinitializations.push_back(std::move(audit));
  }

  std::uint64_t payload_size{};
  if (!decoder.u64(payload_size) || payload_size > k_max_state_bytes ||
      !decoder.bytes(static_cast<std::size_t>(payload_size),
                     state.value.payload) ||
      decoder.remaining() != 0) {
    return {.error = "state payload length is invalid"};
  }

  revision.content_digest =
      digest(bytes.first(bytes.size() - sizeof(std::uint64_t)));
  domain::DeviceStateSnapshot snapshot{
      .key = std::move(decoded_key),
      .revision = revision,
      .state = std::move(state),
  };
  if (schema > k_current_schema || minimum_reader > k_current_schema ||
      minimum_writer > k_current_schema) {
    return {.state = DecodeState::future,
            .snapshot = std::move(snapshot),
            .error = "state schema requires a newer workbench"};
  }
  if (schema < k_previous_schema || schema > k_current_schema) {
    return {.error = "state schema is outside the N/N-1 window"};
  }
  return {.state = DecodeState::supported,
          .snapshot = std::move(snapshot)};
}

[[nodiscard]] bool is_immediately_previous(
    domain::RevisionToken const& current,
    domain::RevisionToken const& previous) noexcept {
  return current.generation > 1 && current.epoch == previous.epoch &&
         previous.generation == current.generation - 1;
}

struct Intent final {
  std::optional<domain::RevisionToken> expected;
  domain::RevisionToken candidate;
};

[[nodiscard]] domain::StateBytes encode_intent(Intent const& intent) {
  Encoder encoder;
  encoder.raw(k_intent_magic);
  encoder.u32(k_envelope_version);
  encoder.u8(intent.expected.has_value() ? 1 : 0);
  if (intent.expected.has_value()) {
    encode_revision(encoder, *intent.expected);
  }
  encode_revision(encoder, intent.candidate);
  return encoder.finish_with_digest();
}

[[nodiscard]] std::optional<Intent> decode_intent(
    std::span<std::byte const> bytes) {
  if (!has_valid_digest(bytes)) {
    return std::nullopt;
  }
  Decoder decoder{bytes.first(bytes.size() - sizeof(std::uint64_t))};
  std::uint32_t version{};
  std::uint8_t has_expected{};
  Intent intent;
  if (!read_magic(decoder, k_intent_magic) || !decoder.u32(version) ||
      version != k_envelope_version || !decoder.u8(has_expected) ||
      has_expected > 1) {
    return std::nullopt;
  }
  if (has_expected == 1) {
    domain::RevisionToken expected;
    if (!decode_revision(decoder, expected)) {
      return std::nullopt;
    }
    intent.expected = expected;
  }
  if (!decode_revision(decoder, intent.candidate) ||
      decoder.remaining() != 0) {
    return std::nullopt;
  }
  return intent;
}

[[nodiscard]] std::array<std::byte, 16> make_epoch(
    domain::StateKey const& key, WallClockTime time,
    std::uint64_t entropy) noexcept {
  std::string identity = key.aggregate.value;
  identity.push_back('/');
  if (key.subject.has_value()) {
    identity += key.subject->value;
  }
  auto const milliseconds =
      static_cast<std::uint64_t>(time.time_since_epoch().count());
  auto first = digest(std::as_bytes(std::span{identity}));
  first ^= milliseconds;
  first *= 1099511628211ULL;
  first ^= entropy;
  auto second = first ^ 0x9e3779b97f4a7c15ULL;
  second *= 1099511628211ULL;

  std::array<std::byte, 16> epoch{};
  for (unsigned shift = 0; shift < 64; shift += 8) {
    epoch[shift / 8] = std::byte{static_cast<std::uint8_t>(first >> shift)};
    epoch[8 + shift / 8] =
        std::byte{static_cast<std::uint8_t>(second >> shift)};
  }
  return epoch;
}

[[nodiscard]] domain::RevisionToken revision_for(
    domain::StateBytes const& encoded, std::array<std::byte, 16> epoch,
    std::uint64_t generation) {
  return domain::RevisionToken{
      .epoch = epoch,
      .generation = generation,
      .content_digest = digest(std::span<std::byte const>{encoded}.first(
          encoded.size() - sizeof(std::uint64_t))),
  };
}

[[nodiscard]] domain::StateBytes encode_with_revision(
    domain::StateKey const& key, domain::DeviceState const& state,
    std::array<std::byte, 16> epoch, std::uint64_t generation,
    domain::RevisionToken& revision) {
  domain::DeviceStateSnapshot provisional{
      .key = key,
      .revision = {.epoch = epoch, .generation = generation},
      .state = state,
  };
  auto bytes = encode_state(provisional);
  revision = revision_for(bytes, epoch, generation);
  return bytes;
}

[[nodiscard]] StateFileRead read_file(StateFileSystem& files,
                                      domain::StateKey const& key,
                                      StateFileSlot slot) {
  return files.read(key, slot);
}

[[nodiscard]] bool cleanup_transaction(StateFileSystem& files,
                                       domain::StateKey const& key) {
  auto const intent = files.remove(key, StateFileSlot::intent);
  auto const candidate = files.remove(key, StateFileSlot::candidate);
  auto const staging = files.remove(key, StateFileSlot::previous_staging);
  return intent.succeeded() && candidate.succeeded() && staging.succeeded();
}

struct LoadedState final {
  StateRead result;
  std::optional<domain::StateBytes> authoritative_bytes;
};

[[nodiscard]] LoadedState load_locked(StateFileSystem& files,
                                      domain::StateKey const& key,
                                      bool recover_transaction);

enum class TransactionCompletion {
  completed,
  published_needs_cleanup,
  failed,
};

[[nodiscard]] TransactionCompletion complete_transaction(
    StateFileSystem& files, domain::StateKey const& key, Intent const& intent,
    domain::StateBytes const& candidate_bytes,
    std::optional<domain::StateBytes> const& expected_bytes) {
  if (expected_bytes.has_value()) {
    auto result = files.write(key, StateFileSlot::previous_staging,
                              *expected_bytes);
    if (!result.succeeded()) {
      return TransactionCompletion::failed;
    }
    result = files.flush(key, StateFileSlot::previous_staging);
    if (!result.succeeded()) {
      return TransactionCompletion::failed;
    }
    result = files.replace(key, StateFileSlot::previous_staging,
                           StateFileSlot::previous);
    if (!result.succeeded()) {
      return TransactionCompletion::failed;
    }
    result = files.flush_volume(key);
    if (!result.succeeded()) {
      return TransactionCompletion::failed;
    }
    auto previous_reread = files.read(key, StateFileSlot::previous);
    if (!intent.expected.has_value() ||
        previous_reread.status != StateIoStatus::succeeded ||
        previous_reread.bytes != *expected_bytes) {
      return TransactionCompletion::failed;
    }
    auto previous_decoded = decode_state(previous_reread.bytes, key);
    if (previous_decoded.state != DecodeState::supported ||
        !previous_decoded.snapshot.has_value() ||
        previous_decoded.snapshot->revision != *intent.expected) {
      return TransactionCompletion::failed;
    }
  }

  auto result = files.replace(key, StateFileSlot::candidate,
                              StateFileSlot::current);
  if (!result.succeeded()) {
    return TransactionCompletion::failed;
  }
  result = files.flush_volume(key);
  if (!result.succeeded()) {
    return TransactionCompletion::failed;
  }
  auto reread = files.read(key, StateFileSlot::current);
  if (reread.status != StateIoStatus::succeeded ||
      reread.bytes != candidate_bytes) {
    return TransactionCompletion::failed;
  }
  auto decoded = decode_state(reread.bytes, key);
  if (decoded.state != DecodeState::supported ||
      !decoded.snapshot.has_value() ||
      decoded.snapshot->revision != intent.candidate) {
    return TransactionCompletion::failed;
  }
  return cleanup_transaction(files, key) ? TransactionCompletion::completed
                                         : TransactionCompletion::published_needs_cleanup;
}

[[nodiscard]] LoadedState load_locked(StateFileSystem& files,
                                      domain::StateKey const& key,
                                      bool recover_transaction) {
  if (!key.valid()) {
    return {.result = {.mode = StateReadMode::failed,
                       .error = "state key is invalid"}};
  }

  auto current_file = read_file(files, key, StateFileSlot::current);
  auto previous_file = read_file(files, key, StateFileSlot::previous);
  auto candidate_file = read_file(files, key, StateFileSlot::candidate);
  auto intent_file = read_file(files, key, StateFileSlot::intent);
  auto staging_file = read_file(files, key, StateFileSlot::previous_staging);
  for (auto const* file : {&current_file, &previous_file, &candidate_file,
                           &intent_file, &staging_file}) {
    if (file->status == StateIoStatus::failed) {
      return {.result = {.mode = StateReadMode::failed,
                         .error = file->error}};
    }
  }

  auto current = current_file.status == StateIoStatus::succeeded
                     ? decode_state(current_file.bytes, key)
                     : DecodedState{};
  auto previous = previous_file.status == StateIoStatus::succeeded
                      ? decode_state(previous_file.bytes, key)
                      : DecodedState{};

  if (current.state == DecodeState::future) {
    return {.result = {
                .mode = StateReadMode::read_only_future,
                .evidence = {{.kind = StateEvidenceKind::future_current,
                              .slot = StateFileSlot::current,
                              .raw_bytes = current_file.bytes,
                              .detail = current.error}},
            }};
  }
  if (previous.state == DecodeState::future) {
    StateRead result{
        .mode = StateReadMode::read_only_future,
        .evidence = {{.kind = StateEvidenceKind::future_previous,
                      .slot = StateFileSlot::previous,
                      .raw_bytes = previous_file.bytes,
                      .detail = previous.error}},
    };
    if (current_file.status == StateIoStatus::succeeded &&
        current.state == DecodeState::invalid) {
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_current,
           .slot = StateFileSlot::current,
           .raw_bytes = current_file.bytes,
           .detail = current.error});
    }
    return {.result = std::move(result)};
  }

  bool const has_residual =
      candidate_file.status == StateIoStatus::succeeded ||
      intent_file.status == StateIoStatus::succeeded ||
      staging_file.status == StateIoStatus::succeeded;
  auto const intent = intent_file.status == StateIoStatus::succeeded
                          ? decode_intent(intent_file.bytes)
                          : std::nullopt;
  bool const current_is_intended_candidate =
      intent.has_value() && current.state == DecodeState::supported &&
      current.snapshot.has_value() &&
      current.snapshot->revision == intent->candidate;
  bool const intended_rollback_matches =
      intent.has_value() &&
      ((!intent->expected.has_value() &&
        previous_file.status == StateIoStatus::not_found) ||
       (intent->expected.has_value() &&
        previous.state == DecodeState::supported &&
        previous.snapshot.has_value() &&
        previous.snapshot->revision == *intent->expected));
  if (recover_transaction && current_is_intended_candidate &&
      intended_rollback_matches) {
    if (!cleanup_transaction(files, key)) {
      return {.result = {.mode = StateReadMode::failed,
                         .error = "completed state transaction cleanup failed; retry"}};
    }
    return load_locked(files, key, false);
  }
  if (recover_transaction && intent.has_value() &&
      candidate_file.status == StateIoStatus::succeeded) {
    auto const candidate = decode_state(candidate_file.bytes, key);
    if (candidate.state == DecodeState::supported &&
        candidate.snapshot.has_value() &&
        candidate.snapshot->revision == intent->candidate) {
      bool const expected_matches =
          (!intent->expected.has_value() &&
           current_file.status == StateIoStatus::not_found &&
           previous_file.status == StateIoStatus::not_found) ||
          (intent->expected.has_value() &&
           current.state == DecodeState::supported &&
           current.snapshot.has_value() &&
           current.snapshot->revision == *intent->expected);
      if (expected_matches) {
        std::optional<domain::StateBytes> expected_bytes;
        if (intent->expected.has_value()) {
          expected_bytes = current_file.bytes;
        }
        auto const completion = complete_transaction(
            files, key, *intent, candidate_file.bytes, expected_bytes);
        if (completion == TransactionCompletion::completed) {
          return load_locked(files, key, false);
        }
        if (completion == TransactionCompletion::published_needs_cleanup) {
          return load_locked(files, key, true);
        }
      }
    }
  }

  StateRead result;
  if (has_residual) {
    auto append_residual = [&](StateFileRead const& file, StateFileSlot slot) {
      if (file.status == StateIoStatus::succeeded) {
        result.evidence.push_back(
            {.kind = StateEvidenceKind::transaction_residual,
             .slot = slot,
             .raw_bytes = file.bytes,
             .detail = "uncommitted transaction residual retained"});
      }
    };
    append_residual(candidate_file, StateFileSlot::candidate);
    append_residual(intent_file, StateFileSlot::intent);
    append_residual(staging_file, StateFileSlot::previous_staging);
  }

  bool const null_intent_conflicts_with_existing_generation =
      recover_transaction && intent.has_value() &&
      !intent->expected.has_value() &&
      (current_file.status == StateIoStatus::succeeded ||
       previous_file.status == StateIoStatus::succeeded);
  bool const completed_candidate_lacks_trusted_rollback =
      recover_transaction && current_is_intended_candidate &&
      !intended_rollback_matches;
  if (null_intent_conflicts_with_existing_generation ||
      completed_candidate_lacks_trusted_rollback) {
    result.mode = StateReadMode::read_only_corrupt;
    result.error =
        "state transaction conflicts with its required generation layout";
    if (current_file.status == StateIoStatus::succeeded) {
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_current,
           .slot = StateFileSlot::current,
           .raw_bytes = current_file.bytes,
           .detail = result.error});
    }
    if (previous_file.status == StateIoStatus::succeeded) {
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_previous,
           .slot = StateFileSlot::previous,
           .raw_bytes = previous_file.bytes,
           .detail = result.error});
    }
    return {.result = std::move(result)};
  }

  if (has_residual) {
    result.mode = StateReadMode::read_only_corrupt;
    result.error = "state transaction residual cannot be recovered exactly";
    if (current_file.status == StateIoStatus::succeeded &&
        current.state == DecodeState::invalid) {
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_current,
           .slot = StateFileSlot::current,
           .raw_bytes = current_file.bytes,
           .detail = current.error});
    }
    if (previous_file.status == StateIoStatus::succeeded &&
        previous.state == DecodeState::invalid) {
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_previous,
           .slot = StateFileSlot::previous,
           .raw_bytes = previous_file.bytes,
           .detail = previous.error});
    }
    return {.result = std::move(result)};
  }

  if (current.state == DecodeState::supported &&
      current.snapshot.has_value()) {
    bool const previous_layout_matches =
        current.snapshot->revision.generation == 1
            ? previous_file.status == StateIoStatus::not_found
            : previous.state == DecodeState::supported &&
                  previous.snapshot.has_value() &&
                  is_immediately_previous(current.snapshot->revision,
                                          previous.snapshot->revision);
    if (!previous_layout_matches) {
      result.mode = StateReadMode::read_only_corrupt;
      result.error = "state generations violate N/N-1 ordering";
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_current,
           .slot = StateFileSlot::current,
           .raw_bytes = current_file.bytes,
           .detail = result.error});
      if (previous_file.status == StateIoStatus::succeeded) {
        result.evidence.push_back(
            {.kind = StateEvidenceKind::invalid_previous,
             .slot = StateFileSlot::previous,
             .raw_bytes = previous_file.bytes,
             .detail = result.error});
      }
      return {.result = std::move(result)};
    }
    result.mode = StateReadMode::writable;
    result.snapshot = std::move(current.snapshot);
    return {.result = std::move(result),
            .authoritative_bytes = std::move(current_file.bytes)};
  }

  if (previous.state == DecodeState::supported &&
      previous.snapshot.has_value()) {
    result.mode = StateReadMode::recovered_previous;
    result.snapshot = std::move(previous.snapshot);
    if (current_file.status == StateIoStatus::succeeded) {
      result.evidence.push_back(
          {.kind = StateEvidenceKind::invalid_current,
           .slot = StateFileSlot::current,
           .raw_bytes = current_file.bytes,
           .detail = current.error});
    }
    return {.result = std::move(result),
            .authoritative_bytes = std::move(previous_file.bytes)};
  }

  bool const current_absent =
      current_file.status == StateIoStatus::not_found;
  bool const previous_absent =
      previous_file.status == StateIoStatus::not_found;
  if (current_absent && previous_absent && !has_residual) {
    result.mode = StateReadMode::uninitialized;
    return {.result = std::move(result)};
  }

  result.mode = StateReadMode::read_only_corrupt;
  result.error = "no authoritative or last trusted generation is valid";
  if (current_file.status == StateIoStatus::succeeded) {
    result.evidence.push_back(
        {.kind = StateEvidenceKind::invalid_current,
         .slot = StateFileSlot::current,
         .raw_bytes = current_file.bytes,
         .detail = current.error});
  }
  if (previous_file.status == StateIoStatus::succeeded) {
    result.evidence.push_back(
        {.kind = StateEvidenceKind::invalid_previous,
         .slot = StateFileSlot::previous,
         .raw_bytes = previous_file.bytes,
         .detail = previous.error});
  }
  return {.result = std::move(result)};
}

[[nodiscard]] bool writable_value(domain::VersionedStateValue const& value) {
  return value.schema >= k_previous_schema &&
         value.schema <= k_current_schema &&
         value.minimum_reader >= k_previous_schema &&
         value.minimum_reader <= k_current_schema &&
         value.minimum_writer >= k_previous_schema &&
         value.minimum_writer <= k_current_schema &&
         value.payload.size() <= k_max_state_bytes;
}

[[nodiscard]] bool valid_impact_category(
    domain::DataImpactCategory category) noexcept {
  auto const value = static_cast<std::uint8_t>(category);
  return value >= static_cast<std::uint8_t>(
                      domain::DataImpactCategory::recovery_records) &&
         value <= static_cast<std::uint8_t>(
                      domain::DataImpactCategory::emergency_withdrawals);
}

[[nodiscard]] bool valid_audit_text(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 256 &&
         std::ranges::all_of(value, [](unsigned char byte) {
           return byte >= 0x20 && byte < 0x7f;
         });
}

[[nodiscard]] bool writable_state(domain::DeviceState const& state) {
  if (!writable_value(state.value) || state.reinitializations.size() > 1024) {
    return false;
  }
  return std::ranges::all_of(
      state.reinitializations, [](domain::ReinitializationAudit const& audit) {
        return valid_audit_text(audit.confirmation_reference) &&
               !audit.affected_categories.empty() &&
               audit.affected_categories.size() <= 64 &&
               std::ranges::all_of(audit.affected_categories,
                                   valid_impact_category);
      });
}

[[nodiscard]] StateCommitResult publish(
    StateFileSystem& files, domain::StateKey const& key,
    std::optional<domain::RevisionToken> expected,
    std::optional<domain::StateBytes> authoritative_bytes,
    domain::DeviceState state, std::array<std::byte, 16> epoch,
    std::uint64_t generation,
    bool discard_existing_generations = false) {
  domain::RevisionToken revision;
  auto candidate =
      encode_with_revision(key, state, epoch, generation, revision);
  bool generation_purge_started = false;
  auto fail = [&](std::string stage, std::string error,
                  bool may_have_committed = false) {
    may_have_committed =
        may_have_committed || generation_purge_started;
    auto const cleanup_succeeded =
        may_have_committed ? false : cleanup_transaction(files, key);
    auto const outcome_unknown = may_have_committed || !cleanup_succeeded;
    return StateCommitResult{
        .status = outcome_unknown ? StateCommitStatus::outcome_unknown
                                  : StateCommitStatus::failed,
        .failed_stage = std::move(stage),
        .error = std::move(error),
    };
  };

  auto io = files.write(key, StateFileSlot::candidate, candidate);
  if (!io.succeeded()) {
    return fail("candidate-write", io.error);
  }
  io = files.flush(key, StateFileSlot::candidate);
  if (!io.succeeded()) {
    return fail("candidate-flush", io.error);
  }
  auto candidate_reread = files.read(key, StateFileSlot::candidate);
  if (candidate_reread.status != StateIoStatus::succeeded ||
      candidate_reread.bytes != candidate) {
    return fail("candidate-reread",
                candidate_reread.error.empty()
                    ? "candidate reread did not match the staged bytes"
                    : candidate_reread.error);
  }
  auto decoded_candidate = decode_state(candidate_reread.bytes, key);
  if (decoded_candidate.state != DecodeState::supported ||
      !decoded_candidate.snapshot.has_value() ||
      decoded_candidate.snapshot->revision != revision) {
    return fail("candidate-reread", "candidate validation failed");
  }

  auto intent = encode_intent(
      Intent{.expected = expected, .candidate = revision});
  io = files.write(key, StateFileSlot::intent, intent);
  if (!io.succeeded()) {
    return fail("intent-write", io.error);
  }
  io = files.flush(key, StateFileSlot::intent);
  if (!io.succeeded()) {
    return fail("intent-flush", io.error);
  }

  if (discard_existing_generations) {
    generation_purge_started = true;
    io = files.remove(key, StateFileSlot::previous);
    if (!io.succeeded()) {
      return fail("previous-remove", io.error);
    }
    io = files.remove(key, StateFileSlot::current);
    if (!io.succeeded()) {
      return fail("current-remove", io.error);
    }
    io = files.flush_volume(key);
    if (!io.succeeded()) {
      return fail("generation-purge-flush", io.error);
    }
  }

  if (authoritative_bytes.has_value()) {
    io = files.write(key, StateFileSlot::previous_staging,
                     *authoritative_bytes);
    if (!io.succeeded()) {
      return fail("previous-staging-write", io.error);
    }
    io = files.flush(key, StateFileSlot::previous_staging);
    if (!io.succeeded()) {
      return fail("previous-staging-flush", io.error);
    }
    io = files.replace(key, StateFileSlot::previous_staging,
                       StateFileSlot::previous);
    if (!io.succeeded()) {
      return fail("previous-replace", io.error, true);
    }
    io = files.flush_volume(key);
    if (!io.succeeded()) {
      return fail("previous-volume-flush", io.error, true);
    }
    auto previous_reread = files.read(key, StateFileSlot::previous);
    auto previous_decoded =
        previous_reread.status == StateIoStatus::succeeded
            ? decode_state(previous_reread.bytes, key)
            : DecodedState{};
    if (previous_reread.status != StateIoStatus::succeeded ||
        previous_reread.bytes != *authoritative_bytes ||
        previous_decoded.state != DecodeState::supported ||
        !previous_decoded.snapshot.has_value() ||
        previous_decoded.snapshot->revision != expected) {
      return fail("previous-reread",
                  previous_reread.error.empty()
                      ? "last trusted generation did not validate"
                      : previous_reread.error,
                  true);
    }
  }

  io = files.replace(key, StateFileSlot::candidate,
                     StateFileSlot::current);
  if (!io.succeeded()) {
    return fail("current-replace", io.error, true);
  }
  io = files.flush_volume(key);
  if (!io.succeeded()) {
    return fail("volume-flush", io.error, true);
  }
  auto current_reread = files.read(key, StateFileSlot::current);
  if (current_reread.status != StateIoStatus::succeeded ||
      current_reread.bytes != candidate) {
    return fail("current-reread",
                current_reread.error.empty()
                    ? "authoritative reread did not match the candidate"
                    : current_reread.error,
                true);
  }
  auto current_decoded = decode_state(current_reread.bytes, key);
  if (current_decoded.state != DecodeState::supported ||
      !current_decoded.snapshot.has_value() ||
      current_decoded.snapshot->revision != revision) {
    return fail("current-reread", "authoritative validation failed", true);
  }

  static_cast<void>(cleanup_transaction(files, key));
  return {.status = StateCommitStatus::committed,
          .snapshot = std::move(current_decoded.snapshot)};
}

[[nodiscard]] domain::StateBytes encode_archive(
    std::vector<StateEvidence> const& evidence) {
  Encoder encoder;
  encoder.raw(k_archive_magic);
  encoder.u32(k_envelope_version);
  encoder.u64(evidence.size());
  for (auto const& item : evidence) {
    encoder.u8(static_cast<std::uint8_t>(item.kind));
    encoder.u8(static_cast<std::uint8_t>(item.slot));
    encoder.u64(item.raw_bytes.size());
    encoder.raw(item.raw_bytes);
    encoder.text(item.detail);
  }
  return encoder.finish_with_digest();
}

[[nodiscard]] std::optional<std::vector<StateEvidence>> decode_archive(
    std::span<std::byte const> bytes) {
  if (!has_valid_digest(bytes)) {
    return std::nullopt;
  }
  Decoder decoder{bytes.first(bytes.size() - sizeof(std::uint64_t))};
  std::uint32_t version{};
  std::uint64_t record_count{};
  if (!read_magic(decoder, k_archive_magic) || !decoder.u32(version) ||
      version != k_envelope_version || !decoder.u64(record_count) ||
      record_count > decoder.remaining() ||
      record_count > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }

  std::vector<StateEvidence> evidence;
  evidence.reserve(static_cast<std::size_t>(record_count));
  for (std::uint64_t index = 0; index < record_count; ++index) {
    std::uint8_t kind{};
    std::uint8_t slot{};
    std::uint64_t raw_size{};
    StateEvidence item;
    if (!decoder.u8(kind) ||
        kind > static_cast<std::uint8_t>(
                   StateEvidenceKind::transaction_residual) ||
        !decoder.u8(slot) ||
        slot > static_cast<std::uint8_t>(
                   StateFileSlot::corrupt_previous_staging) ||
        !decoder.u64(raw_size) ||
        raw_size > std::numeric_limits<std::size_t>::max() ||
        !decoder.bytes(static_cast<std::size_t>(raw_size), item.raw_bytes,
                       static_cast<std::size_t>(raw_size)) ||
        !decoder.text(item.detail)) {
      return std::nullopt;
    }
    item.kind = static_cast<StateEvidenceKind>(kind);
    item.slot = static_cast<StateFileSlot>(slot);
    evidence.push_back(std::move(item));
  }
  if (decoder.remaining() != 0) {
    return std::nullopt;
  }
  return evidence;
}

struct ArchiveWriteResult final {
  bool succeeded{};
  std::string stage;
  std::string error;
};

[[nodiscard]] ArchiveWriteResult append_archive(
    StateFileSystem& files, domain::StateKey const& key,
    std::vector<StateEvidence> const& additions) {
  std::vector<StateEvidence> evidence;
  auto existing = files.read(key, StateFileSlot::corrupt_archive);
  if (existing.status == StateIoStatus::failed) {
    return {.stage = "corrupt-archive-read", .error = existing.error};
  }
  if (existing.status == StateIoStatus::succeeded) {
    auto decoded = decode_archive(existing.bytes);
    if (!decoded.has_value()) {
      return {.stage = "corrupt-archive-read",
              .error = "existing corruption archive failed validation"};
    }
    evidence = std::move(*decoded);
  }
  evidence.insert(evidence.end(), additions.begin(), additions.end());
  auto encoded = encode_archive(evidence);

  auto io = files.write(key, StateFileSlot::corrupt_archive_staging, encoded);
  if (!io.succeeded()) {
    return {.stage = "corrupt-archive-write", .error = io.error};
  }
  io = files.flush(key, StateFileSlot::corrupt_archive_staging);
  if (!io.succeeded()) {
    return {.stage = "corrupt-archive-flush", .error = io.error};
  }
  io = files.replace(key, StateFileSlot::corrupt_archive_staging,
                     StateFileSlot::corrupt_archive);
  if (!io.succeeded()) {
    return {.stage = "corrupt-archive-replace", .error = io.error};
  }
  io = files.flush_volume(key);
  if (!io.succeeded()) {
    return {.stage = "corrupt-archive-volume-flush", .error = io.error};
  }
  auto reread = files.read(key, StateFileSlot::corrupt_archive);
  if (reread.status != StateIoStatus::succeeded || reread.bytes != encoded ||
      !decode_archive(reread.bytes).has_value()) {
    return {.stage = "corrupt-archive-reread",
            .error = reread.error.empty()
                         ? "corruption archive reread did not validate"
                         : reread.error};
  }
  return {.succeeded = true};
}

[[nodiscard]] std::uint64_t raw_evidence_digest(StateRead const& read) {
  std::uint64_t value = 14695981039346656037ULL;
  for (auto const& evidence : read.evidence) {
    value ^= digest(evidence.raw_bytes);
    value *= 1099511628211ULL;
  }
  return value;
}

struct DecodedCheckpoint final {
  StateCheckpoint checkpoint;
  std::uint64_t serial{};
};

struct ConsumedCheckpoint final {
  std::uint64_t serial{};
  std::uint64_t checkpoint_id{};
};

[[nodiscard]] domain::StateBytes encode_checkpoint(
    StateCheckpoint const& checkpoint, std::uint64_t serial) {
  Encoder encoder;
  encoder.raw(k_checkpoint_magic);
  encoder.u32(k_checkpoint_format_version);
  encoder.u64(serial);
  encode_revision(encoder, checkpoint.base_revision);
  encoder.u64(checkpoint.payload.size());
  encoder.raw(checkpoint.payload);
  return encoder.finish_with_digest();
}

[[nodiscard]] std::optional<DecodedCheckpoint> decode_checkpoint(
    std::span<std::byte const> bytes) {
  if (!has_valid_digest(bytes)) {
    return std::nullopt;
  }
  Decoder decoder{bytes.first(bytes.size() - sizeof(std::uint64_t))};
  std::uint32_t version{};
  DecodedCheckpoint decoded;
  std::uint64_t size{};
  if (!read_magic(decoder, k_checkpoint_magic) || !decoder.u32(version) ||
      (version != k_envelope_version &&
       version != k_checkpoint_format_version)) {
    return std::nullopt;
  }
  if (version == k_checkpoint_format_version &&
      (!decoder.u64(decoded.serial) || decoded.serial == 0)) {
    return std::nullopt;
  }
  if (!decode_revision(decoder, decoded.checkpoint.base_revision) ||
      !decoder.u64(size) || size > k_max_state_bytes ||
      !decoder.bytes(static_cast<std::size_t>(size),
                     decoded.checkpoint.payload) ||
      decoder.remaining() != 0) {
    return std::nullopt;
  }
  return decoded;
}

[[nodiscard]] domain::StateBytes encode_consumed(
    ConsumedCheckpoint const& consumed) {
  Encoder encoder;
  encoder.raw(k_consumed_magic);
  encoder.u32(k_checkpoint_format_version);
  encoder.u64(consumed.serial);
  encoder.u64(consumed.checkpoint_id);
  return encoder.finish_with_digest();
}

[[nodiscard]] std::optional<ConsumedCheckpoint> decode_consumed(
    std::span<std::byte const> bytes) {
  if (!has_valid_digest(bytes)) {
    return std::nullopt;
  }
  Decoder decoder{bytes.first(bytes.size() - sizeof(std::uint64_t))};
  std::uint32_t version{};
  ConsumedCheckpoint consumed;
  if (!read_magic(decoder, k_consumed_magic) || !decoder.u32(version) ||
      (version != k_envelope_version &&
       version != k_checkpoint_format_version)) {
    return std::nullopt;
  }
  if (version == k_checkpoint_format_version &&
      !decoder.u64(consumed.serial)) {
    return std::nullopt;
  }
  if (!decoder.u64(consumed.checkpoint_id) ||
      decoder.remaining() != 0) {
    return std::nullopt;
  }
  return consumed;
}

}  // namespace

DeviceStateStore::DeviceStateStore(StateFileSystem& files,
                                   Clock const& clock) noexcept
    : files_(files), clock_(clock) {}

StateRead DeviceStateStore::inspect(domain::StateKey const& key) {
  auto lock_result = files_.try_lock(key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.mode = StateReadMode::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.mode = StateReadMode::failed,
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  return load_locked(files_, key, true).result;
}

StateCommitResult DeviceStateStore::initialize(domain::StateKey const& key,
                                               domain::DeviceState state) {
  auto lock_result = files_.try_lock(key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.status = StateCommitStatus::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.status = StateCommitStatus::failed,
            .failed_stage = "lock",
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  if (!key.valid() || !writable_state(state) ||
      !state.reinitializations.empty()) {
    return {.status = StateCommitStatus::failed,
            .error = "state key or value is invalid"};
  }
  auto loaded = load_locked(files_, key, true);
  if (loaded.result.mode != StateReadMode::uninitialized) {
    return {.status = loaded.result.mode == StateReadMode::writable ||
                             loaded.result.mode ==
                                 StateReadMode::recovered_previous
                         ? StateCommitStatus::conflict
                         : StateCommitStatus::read_only,
            .error = "only a completely empty aggregate may be initialized"};
  }
  auto const epoch =
      make_epoch(key, clock_.now(), digest(state.value.payload));
  return publish(files_, key, std::nullopt, std::nullopt, std::move(state),
                 epoch, 1);
}

StateCommitResult DeviceStateStore::commit(StateCommitRequest request) {
  auto lock_result = files_.try_lock(request.key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.status = StateCommitStatus::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.status = StateCommitStatus::failed,
            .failed_stage = "lock",
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  if (!request.key.valid() || !writable_state(request.state)) {
    return {.status = StateCommitStatus::failed,
            .error = "state key or value is invalid"};
  }

  auto loaded = load_locked(files_, request.key, true);
  if (loaded.result.mode == StateReadMode::read_only_corrupt ||
      loaded.result.mode == StateReadMode::read_only_future ||
      loaded.result.mode == StateReadMode::uninitialized) {
    return {.status = StateCommitStatus::read_only,
            .error = "state aggregate is not writable"};
  }
  if (loaded.result.mode == StateReadMode::failed) {
    return {.status = StateCommitStatus::failed,
            .error = loaded.result.error};
  }
  if (!loaded.result.snapshot.has_value() ||
      loaded.result.snapshot->revision != request.expected_revision) {
    return {.status = StateCommitStatus::conflict,
            .snapshot = loaded.result.snapshot,
            .error = "the authoritative revision changed"};
  }
  if (request.state.reinitializations !=
      loaded.result.snapshot->state.reinitializations) {
    return {.status = StateCommitStatus::failed,
            .error = "ordinary commits must preserve reinitialization audit facts"};
  }

  if (loaded.result.mode == StateReadMode::recovered_previous) {
    auto archive = append_archive(files_, request.key, loaded.result.evidence);
    if (!archive.succeeded) {
      return {.status = StateCommitStatus::failed,
              .failed_stage = std::move(archive.stage),
              .error = std::move(archive.error)};
    }
  }

  auto const generation = request.expected_revision.generation + 1;
  if (generation == 0) {
    return {.status = StateCommitStatus::failed,
            .error = "state generation exhausted"};
  }
  return publish(files_, request.key, request.expected_revision,
                 std::move(loaded.authoritative_bytes),
                 std::move(request.state), request.expected_revision.epoch,
                 generation);
}

StateCommitResult DeviceStateStore::reinitialize(
    StateReinitializeRequest request) {
  auto lock_result = files_.try_lock(request.key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.status = StateCommitStatus::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.status = StateCommitStatus::failed,
            .failed_stage = "lock",
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  if (!request.key.valid() || !writable_value(request.replacement) ||
      request.affected_categories.empty() ||
      request.affected_categories.size() > 64 ||
      !std::ranges::all_of(request.affected_categories,
                          valid_impact_category) ||
      !valid_audit_text(request.confirmation_reference)) {
    return {.status = StateCommitStatus::failed,
            .error = "reinitialization requires impact and confirmation facts"};
  }

  auto loaded = load_locked(files_, request.key, true);
  if (loaded.result.mode == StateReadMode::read_only_future) {
    return {.status = StateCommitStatus::read_only,
            .error = "future state cannot be reinitialized by an older workbench"};
  }
  if (loaded.result.mode != StateReadMode::read_only_corrupt) {
    return {.status = StateCommitStatus::conflict,
            .error = "reinitialization is only available after dual corruption"};
  }

  auto archive =
      append_archive(files_, request.key, loaded.result.evidence);
  if (!archive.succeeded) {
    return {.status = StateCommitStatus::failed,
            .failed_stage = std::move(archive.stage),
            .error = std::move(archive.error)};
  }

  for (auto const& evidence : loaded.result.evidence) {
    StateFileSlot archive_slot;
    if (evidence.kind == StateEvidenceKind::invalid_current) {
      archive_slot = StateFileSlot::corrupt_current;
    } else if (evidence.kind == StateEvidenceKind::invalid_previous) {
      archive_slot = StateFileSlot::corrupt_previous;
    } else if (evidence.kind == StateEvidenceKind::transaction_residual &&
               evidence.slot == StateFileSlot::candidate) {
      archive_slot = StateFileSlot::corrupt_candidate;
    } else if (evidence.kind == StateEvidenceKind::transaction_residual &&
               evidence.slot == StateFileSlot::intent) {
      archive_slot = StateFileSlot::corrupt_intent;
    } else if (evidence.kind == StateEvidenceKind::transaction_residual &&
               evidence.slot == StateFileSlot::previous_staging) {
      archive_slot = StateFileSlot::corrupt_previous_staging;
    } else {
      continue;
    }
    auto io = files_.write(request.key, archive_slot, evidence.raw_bytes);
    if (!io.succeeded()) {
      return {.status = StateCommitStatus::failed,
              .failed_stage = "corrupt-evidence-archive",
              .error = io.error};
    }
    io = files_.flush(request.key, archive_slot);
    if (!io.succeeded()) {
      return {.status = StateCommitStatus::failed,
              .failed_stage = "corrupt-evidence-flush",
              .error = io.error};
    }
  }

  domain::DeviceState replacement{
      .value = std::move(request.replacement),
      .reinitializations = {{
          .affected_categories = std::move(request.affected_categories),
          .confirmation_reference = std::move(request.confirmation_reference),
          .confirmed_at_milliseconds = clock_.now().time_since_epoch().count(),
      }},
  };
  auto const entropy = raw_evidence_digest(loaded.result);
  auto const epoch = make_epoch(request.key, clock_.now(), entropy);
  return publish(files_, request.key, std::nullopt, std::nullopt,
                 std::move(replacement), epoch, 1, true);
}

CheckpointResult DeviceStateStore::write_checkpoint(
    domain::StateKey const& key, StateCheckpoint checkpoint) {
  auto lock_result = files_.try_lock(key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.status = CheckpointStatus::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.status = CheckpointStatus::failed,
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  if (!key.valid() || key.partition != domain::StatePartition::subject ||
      checkpoint.payload.size() > k_max_state_bytes) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoints require a valid subject partition"};
  }
  auto loaded = load_locked(files_, key, true);
  if (loaded.result.mode == StateReadMode::read_only_corrupt ||
      loaded.result.mode == StateReadMode::read_only_future) {
    return {.status = CheckpointStatus::read_only,
            .error = "state aggregate is read-only"};
  }
  if (!loaded.result.snapshot.has_value() ||
      loaded.result.snapshot->revision != checkpoint.base_revision) {
    return {.status = CheckpointStatus::conflict,
            .error = "checkpoint base revision changed"};
  }

  std::uint64_t latest_serial{};
  auto active = files_.read(key, StateFileSlot::checkpoint);
  if (active.status == StateIoStatus::failed) {
    return {.status = CheckpointStatus::failed, .error = active.error};
  }
  if (active.status == StateIoStatus::succeeded) {
    auto decoded = decode_checkpoint(active.bytes);
    if (!decoded.has_value()) {
      return {.status = CheckpointStatus::failed,
              .error = "existing checkpoint integrity check failed"};
    }
    latest_serial = decoded->serial;
  }
  auto consumed = files_.read(key, StateFileSlot::checkpoint_consumed);
  if (consumed.status == StateIoStatus::failed) {
    return {.status = CheckpointStatus::failed, .error = consumed.error};
  }
  if (consumed.status == StateIoStatus::succeeded) {
    auto decoded = decode_consumed(consumed.bytes);
    if (!decoded.has_value()) {
      return {.status = CheckpointStatus::failed,
              .error = "checkpoint consumption fact is damaged"};
    }
    latest_serial = std::max(latest_serial, decoded->serial);
  }
  if (latest_serial == std::numeric_limits<std::uint64_t>::max()) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoint serial exhausted"};
  }
  auto encoded = encode_checkpoint(checkpoint, latest_serial + 1);
  auto io = files_.write(key, StateFileSlot::checkpoint_staging, encoded);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.flush(key, StateFileSlot::checkpoint_staging);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.replace(key, StateFileSlot::checkpoint_staging,
                      StateFileSlot::checkpoint);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.flush_volume(key);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.remove(key, StateFileSlot::checkpoint_consumed);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.flush_volume(key);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  return {.status = CheckpointStatus::available,
          .checkpoint = std::move(checkpoint)};
}

CheckpointResult DeviceStateStore::read_checkpoint(
    domain::StateKey const& key,
    domain::RevisionToken const& current_revision) {
  auto lock_result = files_.try_lock(key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.status = CheckpointStatus::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.status = CheckpointStatus::failed,
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  if (!key.valid() || key.partition != domain::StatePartition::subject) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoints require a valid subject partition"};
  }
  auto loaded = load_locked(files_, key, true);
  if (loaded.result.mode == StateReadMode::read_only_corrupt ||
      loaded.result.mode == StateReadMode::read_only_future) {
    return {.status = CheckpointStatus::read_only,
            .error = "state aggregate is read-only"};
  }
  if (!loaded.result.snapshot.has_value() ||
      loaded.result.snapshot->revision != current_revision) {
    return {.status = CheckpointStatus::conflict,
            .error = "checkpoint base revision changed"};
  }
  auto active = files_.read(key, StateFileSlot::checkpoint);
  if (active.status == StateIoStatus::not_found) {
    return {.status = CheckpointStatus::absent};
  }
  if (active.status == StateIoStatus::failed) {
    return {.status = CheckpointStatus::failed, .error = active.error};
  }
  auto checkpoint = decode_checkpoint(active.bytes);
  if (!checkpoint.has_value()) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoint integrity check failed"};
  }

  auto consumed = files_.read(key, StateFileSlot::checkpoint_consumed);
  if (consumed.status == StateIoStatus::failed) {
    return {.status = CheckpointStatus::failed, .error = consumed.error};
  }
  auto const checkpoint_id = digest(active.bytes);
  if (consumed.status == StateIoStatus::succeeded) {
    auto consumed_id = decode_consumed(consumed.bytes);
    if (!consumed_id.has_value()) {
      return {.status = CheckpointStatus::conflict,
              .error = "checkpoint consumption fact is damaged; payload will not be revived"};
    }
    if (consumed_id->checkpoint_id == checkpoint_id) {
      return {.status = CheckpointStatus::consumed};
    }
    // A valid tombstone for another payload is stale after an atomic
    // checkpoint replacement. It must not consume the newer checkpoint.
  }
  if (checkpoint->checkpoint.base_revision != current_revision) {
    return {.status = CheckpointStatus::conflict,
            .checkpoint = std::move(checkpoint->checkpoint),
            .error = "checkpoint base revision changed"};
  }
  return {.status = CheckpointStatus::available,
          .checkpoint = std::move(checkpoint->checkpoint)};
}

CheckpointResult DeviceStateStore::consume_checkpoint(
    domain::StateKey const& key,
    domain::RevisionToken const& current_revision) {
  auto lock_result = files_.try_lock(key);
  if (lock_result.status == StateFileLockStatus::busy) {
    return {.status = CheckpointStatus::busy,
            .error = "state aggregate is in use by another instance"};
  }
  if (lock_result.status != StateFileLockStatus::acquired) {
    return {.status = CheckpointStatus::failed,
            .error = std::move(lock_result.error)};
  }
  auto lock = std::move(lock_result.lock);
  if (!key.valid() || key.partition != domain::StatePartition::subject) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoints require a valid subject partition"};
  }
  auto loaded = load_locked(files_, key, true);
  if (loaded.result.mode == StateReadMode::read_only_corrupt ||
      loaded.result.mode == StateReadMode::read_only_future) {
    return {.status = CheckpointStatus::read_only,
            .error = "state aggregate is read-only"};
  }
  if (!loaded.result.snapshot.has_value() ||
      loaded.result.snapshot->revision != current_revision) {
    return {.status = CheckpointStatus::conflict,
            .error = "checkpoint base revision changed"};
  }

  auto active = files_.read(key, StateFileSlot::checkpoint);
  if (active.status == StateIoStatus::not_found) {
    return {.status = CheckpointStatus::absent};
  }
  if (active.status == StateIoStatus::failed) {
    return {.status = CheckpointStatus::failed, .error = active.error};
  }
  auto checkpoint = decode_checkpoint(active.bytes);
  if (!checkpoint.has_value()) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoint integrity check failed"};
  }
  if (checkpoint->checkpoint.base_revision != current_revision) {
    return {.status = CheckpointStatus::conflict,
            .checkpoint = std::move(checkpoint->checkpoint),
            .error = "checkpoint base revision changed"};
  }

  auto tombstone = encode_consumed(
      {.serial = checkpoint->serial,
       .checkpoint_id = digest(active.bytes)});
  auto io = files_.write(key, StateFileSlot::checkpoint_consumed_staging,
                         tombstone);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.flush(key, StateFileSlot::checkpoint_consumed_staging);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.replace(key, StateFileSlot::checkpoint_consumed_staging,
                      StateFileSlot::checkpoint_consumed);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed, .error = io.error};
  }
  io = files_.flush_volume(key);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::failed,
            .error = "checkpoint was consumed but durability is uncertain: " +
                     io.error};
  }
  io = files_.remove(key, StateFileSlot::checkpoint);
  if (!io.succeeded()) {
    return {.status = CheckpointStatus::consumed,
            .error = "checkpoint was consumed but payload cleanup failed"};
  }
  io = files_.flush_volume(key);
  return {.status = CheckpointStatus::consumed,
          .error = io.succeeded()
                       ? std::string{}
                       : "checkpoint was consumed but cleanup durability is uncertain"};
}

}  // namespace azzs::application
