#include "azzs/application/guided_initialization.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "azzs/application/clock.hpp"
#include "azzs/application/device_state_store.hpp"

namespace azzs::application::guided_initialization {
namespace {

constexpr std::array<std::byte, 8> k_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'G'}, std::byte{'I'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t k_format_version = 1;
constexpr std::uint32_t k_state_schema = 1;
constexpr std::size_t k_max_records = 256;
constexpr std::size_t k_max_handoffs_per_record = 512;
constexpr std::size_t k_max_text_bytes = 4096;

[[nodiscard]] domain::StateKey state_key() {
  return domain::StateKey::machine(domain::AggregateId{"guided-initialization"});
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

  template <std::size_t Size>
  void raw(std::array<std::byte, Size> const& value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (auto const character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }

  [[nodiscard]] domain::StateBytes finish() { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class Decoder final {
 public:
  explicit Decoder(std::span<std::byte const> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t& value) {
    if (position_ == bytes_.size()) {
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

  template <std::size_t Size>
  [[nodiscard]] bool raw(std::array<std::byte, Size>& value) {
    if (bytes_.size() - position_ < value.size()) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                value.size(), value.begin());
    position_ += value.size();
    return true;
  }

  [[nodiscard]] bool text(std::string& value) {
    std::uint32_t size{};
    if (!u32(size) || size > k_max_text_bytes ||
        bytes_.size() - position_ < size) {
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

  [[nodiscard]] bool complete() const noexcept { return position_ == bytes_.size(); }

 private:
  std::span<std::byte const> bytes_;
  std::size_t position_{};
};

[[nodiscard]] bool writable(StateReadMode mode) noexcept {
  return mode == StateReadMode::uninitialized ||
         mode == StateReadMode::writable ||
         mode == StateReadMode::recovered_previous;
}

[[nodiscard]] bool readable(StateReadMode mode) noexcept {
  return mode == StateReadMode::writable ||
         mode == StateReadMode::recovered_previous;
}

[[nodiscard]] bool valid_text(std::string_view value, std::size_t maximum = k_max_text_bytes) {
  return value.size() <= maximum;
}

[[nodiscard]] bool valid_stage(Stage value) noexcept {
  return value >= Stage::drivers && value <= Stage::software_optimization;
}

[[nodiscard]] bool valid_stage_state(StageState value) noexcept {
  return value >= StageState::pending &&
         value <= StageState::not_executed;
}

[[nodiscard]] bool valid_flow_state(FlowState value) noexcept {
  return value >= FlowState::active && value <= FlowState::cancelled;
}

[[nodiscard]] bool valid_handoff_state(ExternalHandoffState value) noexcept {
  return value >= ExternalHandoffState::waiting_for_external_install &&
         value <= ExternalHandoffState::skipped;
}

[[nodiscard]] bool terminal_stage(StageState value) noexcept {
  switch (value) {
    case StageState::completed:
    case StageState::skipped:
    case StageState::no_applicable_items:
    case StageState::partial:
    case StageState::not_executed:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool terminal_flow(FlowState value) noexcept {
  return value == FlowState::completed || value == FlowState::cancelled;
}

[[nodiscard]] std::size_t index_for(Stage stage) noexcept {
  return static_cast<std::size_t>(stage);
}

[[nodiscard]] Stage next_stage(Stage stage) noexcept {
  return static_cast<Stage>(static_cast<std::uint8_t>(stage) + 1U);
}

[[nodiscard]] std::array<StageRecord, 4> default_stages() {
  return {{
      {.stage = Stage::drivers},
      {.stage = Stage::system_optimization},
      {.stage = Stage::software_installation},
      {.stage = Stage::software_optimization},
  }};
}

[[nodiscard]] bool record_is_valid(FlowRecord const& record) {
  if (record.id.empty() || !valid_text(record.id, 128) ||
      !valid_flow_state(record.state) || !valid_stage(record.current_stage) ||
      record.created_at_milliseconds < 0 || record.updated_at_milliseconds < 0 ||
      record.updated_at_milliseconds < record.created_at_milliseconds ||
      record.external_handoff_software_ids.size() > k_max_handoffs_per_record) {
    return false;
  }
  for (std::size_t index = 0; index < record.stages.size(); ++index) {
    auto const& stage = record.stages[index];
    if (stage.stage != static_cast<Stage>(index) ||
        !valid_stage_state(stage.state) || !valid_text(stage.detail)) {
      return false;
    }
  }
  for (auto const& id : record.external_handoff_software_ids) {
    if (id.empty() || !valid_text(id, 256) ||
        std::ranges::count(record.external_handoff_software_ids, id) != 1) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool has_handoff(FlowRecord const& record,
                               std::string_view software_id) {
  return std::ranges::find(record.external_handoff_software_ids, software_id) !=
         record.external_handoff_software_ids.end();
}

[[nodiscard]] bool stage_equal(StageRecord const& left,
                               StageRecord const& right) noexcept {
  return left.stage == right.stage && left.state == right.state &&
         left.detail == right.detail;
}

[[nodiscard]] bool record_equal(FlowRecord const& left,
                                FlowRecord const& right) noexcept {
  if (left.id != right.id ||
      left.created_at_milliseconds != right.created_at_milliseconds ||
      left.updated_at_milliseconds != right.updated_at_milliseconds ||
      left.state != right.state || left.current_stage != right.current_stage ||
      left.external_handoff_software_ids != right.external_handoff_software_ids) {
    return false;
  }
  for (std::size_t index = 0; index < left.stages.size(); ++index) {
    if (!stage_equal(left.stages[index], right.stages[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool records_equal(std::vector<FlowRecord> const& left,
                                 std::vector<FlowRecord> const& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!record_equal(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool encode_record(Encoder& encoder, FlowRecord const& record) {
  if (!record_is_valid(record)) {
    return false;
  }
  encoder.text(record.id);
  encoder.i64(record.created_at_milliseconds);
  encoder.i64(record.updated_at_milliseconds);
  encoder.u8(static_cast<std::uint8_t>(record.state));
  encoder.u8(static_cast<std::uint8_t>(record.current_stage));
  for (auto const& stage : record.stages) {
    encoder.u8(static_cast<std::uint8_t>(stage.stage));
    encoder.u8(static_cast<std::uint8_t>(stage.state));
    encoder.text(stage.detail);
  }
  encoder.u32(static_cast<std::uint32_t>(record.external_handoff_software_ids.size()));
  for (auto const& software_id : record.external_handoff_software_ids) {
    encoder.text(software_id);
  }
  return true;
}

[[nodiscard]] bool decode_record(Decoder& decoder, FlowRecord& record) {
  std::uint8_t state{};
  std::uint8_t current_stage{};
  std::uint32_t handoff_count{};
  if (!decoder.text(record.id) || !decoder.i64(record.created_at_milliseconds) ||
      !decoder.i64(record.updated_at_milliseconds) || !decoder.u8(state) ||
      !decoder.u8(current_stage)) {
    return false;
  }
  record.state = static_cast<FlowState>(state);
  record.current_stage = static_cast<Stage>(current_stage);
  for (auto& stage : record.stages) {
    std::uint8_t stage_id{};
    std::uint8_t stage_state{};
    if (!decoder.u8(stage_id) || !decoder.u8(stage_state) ||
        !decoder.text(stage.detail)) {
      return false;
    }
    stage.stage = static_cast<Stage>(stage_id);
    stage.state = static_cast<StageState>(stage_state);
  }
  if (!decoder.u32(handoff_count) || handoff_count > k_max_handoffs_per_record) {
    return false;
  }
  record.external_handoff_software_ids.clear();
  record.external_handoff_software_ids.reserve(handoff_count);
  for (std::uint32_t index = 0; index < handoff_count; ++index) {
    std::string software_id;
    if (!decoder.text(software_id)) {
      return false;
    }
    record.external_handoff_software_ids.push_back(std::move(software_id));
  }
  return record_is_valid(record);
}

[[nodiscard]] std::optional<domain::StateBytes> encode_state(
    std::vector<FlowRecord> const& records, std::optional<std::string> const& active_id,
    std::uint64_t next_sequence) {
  if (records.size() > k_max_records || next_sequence == 0) {
    return std::nullopt;
  }
  if (active_id.has_value() &&
      (!valid_text(*active_id, 128) || active_id->empty())) {
    return std::nullopt;
  }
  Encoder encoder;
  encoder.raw(k_magic);
  encoder.u32(k_format_version);
  encoder.u64(next_sequence);
  encoder.u8(active_id.has_value() ? 1U : 0U);
  if (active_id.has_value()) {
    encoder.text(*active_id);
  }
  encoder.u32(static_cast<std::uint32_t>(records.size()));
  for (auto const& record : records) {
    if (!encode_record(encoder, record)) {
      return std::nullopt;
    }
  }
  return encoder.finish();
}

[[nodiscard]] bool decode_state(std::span<std::byte const> bytes,
                                std::vector<FlowRecord>& records,
                                std::optional<std::string>& active_id,
                                std::uint64_t& next_sequence) {
  Decoder decoder{bytes};
  std::array<std::byte, k_magic.size()> magic{};
  std::uint32_t format{};
  std::uint8_t has_active{};
  std::uint32_t record_count{};
  if (!decoder.raw(magic) || magic != k_magic || !decoder.u32(format) ||
      format != k_format_version || !decoder.u64(next_sequence) ||
      next_sequence == 0 || !decoder.u8(has_active) || has_active > 1) {
    return false;
  }
  active_id.reset();
  if (has_active == 1) {
    std::string value;
    if (!decoder.text(value) || value.empty() || !valid_text(value, 128)) {
      return false;
    }
    active_id = std::move(value);
  }
  if (!decoder.u32(record_count) || record_count > k_max_records) {
    return false;
  }
  records.clear();
  records.reserve(record_count);
  for (std::uint32_t index = 0; index < record_count; ++index) {
    FlowRecord record;
    if (!decode_record(decoder, record) ||
        std::ranges::any_of(records, [&record](FlowRecord const& existing) {
          return existing.id == record.id;
        })) {
      return false;
    }
    records.push_back(std::move(record));
  }
  if (!decoder.complete()) {
    return false;
  }
  auto const active = std::ranges::find_if(
      records, [&active_id](FlowRecord const& record) {
        return active_id.has_value() && record.id == *active_id;
      });
  if (active_id.has_value()) {
    return active != records.end() && !terminal_flow(active->state);
  }
  return std::ranges::none_of(records, [](FlowRecord const& record) {
    return !terminal_flow(record.state);
  });
}

[[nodiscard]] StageState stage_from_handoff_state(
    ExternalHandoffState value) noexcept {
  switch (value) {
    case ExternalHandoffState::waiting_for_external_install:
    case ExternalHandoffState::externally_recognized:
      return StageState::external_handoff;
    case ExternalHandoffState::skipped:
      return StageState::partial;
  }
  return StageState::failed;
}

[[nodiscard]] bool can_replace_from_evidence(StageState current,
                                              StageState observed) noexcept {
  if (observed == StageState::emergency_withdrawn) {
    return true;
  }
  if (current == StageState::completed || current == StageState::skipped ||
      current == StageState::no_applicable_items ||
      current == StageState::not_executed) {
    return false;
  }
  return true;
}

}  // namespace

GuidedInitializationService::GuidedInitializationService(
    DeviceStateStore& states, Clock const& clock, EvidenceSource& evidence_source)
    : states_(states), clock_(clock), evidence_source_(evidence_source) {}

ActionResult GuidedInitializationService::restore() {
  if (mode_ != LifecycleMode::not_restored) {
    return make_result(mode_ == LifecycleMode::read_only ? ActionCode::read_only
                                                           : ActionCode::succeeded);
  }

  auto read = states_.inspect(state_key());
  if (read.mode == StateReadMode::uninitialized) {
    mode_ = LifecycleMode::ready;
    writable_ = true;
    records_.clear();
    active_id_.reset();
    revision_.reset();
    next_sequence_ = 1;
  } else if (!readable(read.mode) || !read.snapshot.has_value() ||
             !decode_state(read.snapshot->state.value.payload, records_, active_id_,
                           next_sequence_)) {
    mode_ = LifecycleMode::read_only;
    writable_ = false;
    revision_.reset();
    error_ = read.error.empty()
                 ? "recommended initialization records are unavailable for safe recovery"
                 : std::move(read.error);
    if (read.snapshot.has_value() && read.error.empty()) {
      error_ = "recommended initialization records have an unsupported or invalid format";
    }
    try {
      evidence_ = evidence_source_.observe();
    } catch (std::exception const& error) {
      evidence_ = {};
      error_ += "; evidence observation failed: ";
      error_ += error.what();
    } catch (...) {
      evidence_ = {};
      error_ += "; evidence observation failed";
    }
    return make_result(ActionCode::read_only, error_);
  } else {
    mode_ = LifecycleMode::ready;
    writable_ = writable(read.mode);
    revision_ = read.snapshot->revision;
    error_.clear();
  }

  auto before_records = records_;
  auto before_active = active_id_;
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed)) {
    mode_ = LifecycleMode::failed;
    writable_ = false;
    error_ = std::move(error);
    return make_result(ActionCode::source_rejected, error_);
  }
  if (changed && !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    mode_ = LifecycleMode::failed;
    writable_ = false;
    error_ = std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

Snapshot GuidedInitializationService::snapshot() const {
  Snapshot result{
      .mode = mode_,
      .writable = writable_,
      .evidence = evidence_,
      .error = error_,
  };
  if (auto const* record = active_record()) {
    result.active = *record;
    result.summary = summarize(*record);
  } else if (!records_.empty()) {
    // Completed and cancelled flows remain immutable history. The latest
    // record still supplies the summary shown at the completion boundary.
    result.summary = summarize(records_.back());
  }
  for (auto const& record : records_) {
    if (!active_id_.has_value() || record.id != *active_id_) {
      result.history.push_back(record);
    }
  }
  return result;
}

ActionResult GuidedInitializationService::refresh() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  auto before_records = records_;
  auto before_active = active_id_;
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed)) {
    error_ = std::move(error);
    return make_result(ActionCode::source_rejected, error_);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only, error_);
  }
  if (changed && !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::start() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  if (active_record() != nullptr) {
    return make_result(ActionCode::rejected,
                       "an existing recommended initialization flow is still active");
  }

  auto before_records = records_;
  auto before_active = active_id_;
  auto const before_sequence = next_sequence_;
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed)) {
    error_ = std::move(error);
    return make_result(ActionCode::source_rejected, error_);
  }
  if (evidence_.restart_gate != RestartGateState::none) {
    return make_result(ActionCode::continuation_required,
                       "the existing restart barrier needs an explicit decision first");
  }
  auto const now = now_milliseconds();
  FlowRecord record{
      .id = "guided-" + std::to_string(next_sequence_++),
      .created_at_milliseconds = now,
      .updated_at_milliseconds = now,
      .state = FlowState::active,
      .current_stage = Stage::drivers,
      .stages = default_stages(),
  };
  records_.push_back(std::move(record));
  active_id_ = records_.back().id;
  if (!synchronize_from_evidence(error, changed) || !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    next_sequence_ = before_sequence;
    error_ = error.empty() ? "recommended initialization flow could not be persisted"
                           : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::mark_driver_completed() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  auto* record = active_record();
  if (record == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  if (record->state != FlowState::active || record->current_stage != Stage::drivers ||
      evidence_.restart_gate != RestartGateState::none) {
    return make_result(ActionCode::continuation_required,
                       "the driver stage is not ready for an explicit completion");
  }

  auto before_records = records_;
  auto before_active = active_id_;
  auto& stage = stage_for(*record);
  stage.state = StageState::completed;
  stage.detail = "driver stage marked complete by the user";
  record->updated_at_milliseconds = now_milliseconds();
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed) || !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = error.empty() ? "driver stage completion could not be persisted"
                           : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::skip_current_stage() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  auto* record = active_record();
  if (record == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  if (record->state != FlowState::active || evidence_.restart_gate != RestartGateState::none) {
    return make_result(ActionCode::continuation_required,
                       "the current stage is paused by a restart barrier");
  }
  auto const current = record->current_stage;
  auto& stage = stage_for(*record);
  if (stage.state == StageState::external_handoff) {
    return make_result(ActionCode::rejected,
                       "external handoffs must be skipped from their owned software record");
  }
  if (terminal_stage(stage.state)) {
    return make_result(ActionCode::rejected, "the current stage is already resolved");
  }

  auto before_records = records_;
  auto before_active = active_id_;
  stage.state = StageState::skipped;
  stage.detail = "stage temporarily skipped by the user";
  record->updated_at_milliseconds = now_milliseconds();
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed) || !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = error.empty() ? "stage skip could not be persisted" : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded,
                     std::string{"temporarily skipped "} + to_string(current));
}

ActionResult GuidedInitializationService::continue_current_stage() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  if (active_record() == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  auto before_records = records_;
  auto before_active = active_id_;
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed)) {
    error_ = std::move(error);
    return make_result(ActionCode::source_rejected, error_);
  }
  auto const progressed = !records_equal(before_records, records_) ||
                          before_active != active_id_;
  if (!progressed) {
    return make_result(ActionCode::continuation_required,
                       "the current stage still needs an explicit result or continuation");
  }
  if (!persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = error.empty() ? "stage continuation could not be persisted"
                           : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::continue_external_handoff(
    std::string_view software_id) {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  auto* record = active_record();
  if (record == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  if (record->state != FlowState::active ||
      record->current_stage != Stage::software_installation ||
      !has_handoff(*record, software_id)) {
    return make_result(ActionCode::rejected,
                       "the requested external handoff is not part of the current stage");
  }
  auto const handoff = std::ranges::find_if(
      evidence_.external_handoffs, [software_id](ExternalHandoffEvidence const& value) {
        return value.software_id == software_id;
      });
  if (handoff == evidence_.external_handoffs.end() ||
      handoff->state != ExternalHandoffState::externally_recognized) {
    return make_result(ActionCode::continuation_required,
                       "external installation must be recognized before continuing");
  }

  std::string source_error;
  if (!evidence_source_.continue_external_handoff(software_id, source_error)) {
    return make_result(ActionCode::source_rejected,
                       source_error.empty()
                           ? "the software selection service rejected the continuation"
                           : std::move(source_error));
  }
  auto before_records = records_;
  auto before_active = active_id_;
  record->updated_at_milliseconds = now_milliseconds();
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed) || !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = error.empty() ? "external handoff continuation could not be persisted"
                           : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::continue_after_restart() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  auto* record = active_record();
  if (record == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  if (record->state != FlowState::awaiting_restart_continue ||
      evidence_.restart_gate != RestartGateState::awaiting_user_continue) {
    return make_result(ActionCode::continuation_required,
                       "restart recovery has not reached the explicit continue boundary");
  }

  std::string source_error;
  if (!evidence_source_.continue_after_restart(source_error)) {
    return make_result(ActionCode::source_rejected,
                       source_error.empty()
                           ? "the restart-resume service rejected the continuation"
                           : std::move(source_error));
  }
  auto before_records = records_;
  auto before_active = active_id_;
  record->updated_at_milliseconds = now_milliseconds();
  std::string error;
  bool changed{};
  if (!synchronize_from_evidence(error, changed) || !persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = error.empty() ? "restart continuation could not be persisted"
                           : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::retry_current_stage() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  auto* record = active_record();
  if (record == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  auto const& stage = stage_for(*record);
  if (stage.state != StageState::failed) {
    return make_result(ActionCode::rejected,
                       "only a current failed stage can be retried from the workflow");
  }
  // Retrying belongs to the stage owner. This receipt only records the user's
  // intent boundary so a page can navigate to that owner without fabricating a
  // new settings run or batch.
  auto before_records = records_;
  record->updated_at_milliseconds = now_milliseconds();
  std::string error;
  if (!persist(error)) {
    records_ = std::move(before_records);
    error_ = error.empty() ? "retry intent could not be persisted" : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded,
                     "retry the failed stage from its dedicated page");
}

ActionResult GuidedInitializationService::cancel() {
  if (mode_ == LifecycleMode::not_restored) {
    return make_result(ActionCode::not_restored);
  }
  if (!writable_) {
    return make_result(ActionCode::read_only);
  }
  auto* record = active_record();
  if (record == nullptr) {
    return make_result(ActionCode::no_active_flow);
  }
  auto before_records = records_;
  auto before_active = active_id_;
  record->state = FlowState::cancelled;
  record->updated_at_milliseconds = now_milliseconds();
  active_id_.reset();
  std::string error;
  if (!persist(error)) {
    records_ = std::move(before_records);
    active_id_ = std::move(before_active);
    error_ = error.empty() ? "flow cancellation could not be persisted"
                           : std::move(error);
    return make_result(ActionCode::persistence_failed, error_);
  }
  return make_result(ActionCode::succeeded);
}

ActionResult GuidedInitializationService::make_result(ActionCode code,
                                                       std::string message) const {
  return {.code = code, .snapshot = snapshot(), .message = std::move(message)};
}

bool GuidedInitializationService::persist(std::string& error) {
  auto payload = encode_state(records_, active_id_, next_sequence_);
  if (!payload.has_value()) {
    error = "recommended initialization records exceed their closed persistence format";
    return false;
  }
  domain::DeviceState state{
      .value = {.schema = k_state_schema,
                .minimum_reader = k_state_schema,
                .minimum_writer = k_state_schema,
                .payload = std::move(*payload)},
  };
  auto committed = revision_.has_value()
                       ? states_.commit({.key = state_key(),
                                         .expected_revision = *revision_,
                                         .state = std::move(state)})
                       : states_.initialize(state_key(), std::move(state));
  if (committed.status == StateCommitStatus::committed &&
      committed.snapshot.has_value()) {
    revision_ = committed.snapshot->revision;
    error_.clear();
    return true;
  }
  error = committed.error.empty() ? "recommended initialization records could not be persisted"
                                  : std::move(committed.error);
  return false;
}

bool GuidedInitializationService::synchronize_from_evidence(std::string& error,
                                                              bool& changed) {
  changed = false;
  try {
    evidence_ = evidence_source_.observe();
  } catch (std::exception const& observed) {
    error = "recommended initialization evidence could not be observed: ";
    error += observed.what();
    return false;
  } catch (...) {
    error = "recommended initialization evidence could not be observed";
    return false;
  }
  for (auto const& handoff : evidence_.external_handoffs) {
    if (handoff.software_id.empty() || !valid_text(handoff.software_id, 256) ||
        !valid_handoff_state(handoff.state)) {
      error = "recommended initialization evidence contains an invalid external handoff";
      return false;
    }
  }

  auto* record = active_record();
  if (record == nullptr || !writable_) {
    return true;
  }
  auto const before = *record;
  auto const before_active = active_id_;

  switch (evidence_.restart_gate) {
    case RestartGateState::waiting_for_windows_restart:
    case RestartGateState::awaiting_read_only_verification:
      record->state = FlowState::waiting_for_restart;
      stage_for(*record).state = StageState::waiting_for_restart;
      stage_for(*record).detail = "waiting for the shared Windows restart barrier";
      break;
    case RestartGateState::awaiting_user_continue:
      record->state = FlowState::awaiting_restart_continue;
      stage_for(*record).state = StageState::waiting_for_restart;
      stage_for(*record).detail = "restart verification is complete; explicit continuation is required";
      break;
    case RestartGateState::read_only:
      record->state = FlowState::waiting_for_restart;
      stage_for(*record).state = StageState::waiting_for_restart;
      stage_for(*record).detail = "restart barrier is available only for read-only recovery";
      break;
    case RestartGateState::none:
      if (record->state == FlowState::waiting_for_restart ||
          record->state == FlowState::awaiting_restart_continue) {
        record->state = FlowState::active;
      }
      break;
  }

  if (record->state == FlowState::active) {
    bool keep_advancing = true;
    while (keep_advancing) {
      auto& stage = stage_for(*record);
      auto const& observed = evidence_for(record->current_stage);
      if (!valid_stage_state(observed.state) || !valid_text(observed.detail)) {
        error = "recommended initialization evidence contains an invalid stage state";
        return false;
      }
      if (can_replace_from_evidence(stage.state, observed.state) &&
          apply_stage_evidence(*record, observed)) {
        changed = true;
      }
      if (record->current_stage == Stage::software_installation) {
        for (auto const& handoff : evidence_.external_handoffs) {
          if (!has_handoff(*record, handoff.software_id)) {
            record->external_handoff_software_ids.push_back(handoff.software_id);
            changed = true;
          }
        }
        auto const unresolved = std::ranges::find_if(
            evidence_.external_handoffs,
            [](ExternalHandoffEvidence const& handoff) {
              return stage_from_handoff_state(handoff.state) ==
                     StageState::external_handoff;
            });
        if (unresolved != evidence_.external_handoffs.end() &&
            stage.state != StageState::skipped) {
          if (stage.state != StageState::external_handoff) {
            stage.state = StageState::external_handoff;
            stage.detail = "waiting for an external installation handoff to continue";
            changed = true;
          }
        }
      }
      if (!terminal_stage(stage.state)) {
        break;
      }
      if (record->current_stage == Stage::software_optimization) {
        record->state = FlowState::completed;
        active_id_.reset();
        changed = true;
        keep_advancing = false;
      } else {
        record->current_stage = next_stage(record->current_stage);
        auto& next = stage_for(*record);
        if (next.state == StageState::pending) {
          next.detail.clear();
        }
        changed = true;
      }
    }
  }

  if (!record_equal(before, *record) || before_active != active_id_) {
    record->updated_at_milliseconds = now_milliseconds();
    changed = true;
  }
  return true;
}

bool GuidedInitializationService::apply_stage_evidence(FlowRecord& record,
                                                        StageEvidence const& evidence) {
  auto& stage = stage_for(record);
  if (stage.state == evidence.state && stage.detail == evidence.detail) {
    return false;
  }
  stage.state = evidence.state;
  stage.detail = evidence.detail;
  return true;
}

bool GuidedInitializationService::advance(FlowRecord& record) {
  if (!terminal_stage(stage_for(record).state)) {
    return false;
  }
  if (record.current_stage == Stage::software_optimization) {
    record.state = FlowState::completed;
    active_id_.reset();
    return true;
  }
  record.current_stage = next_stage(record.current_stage);
  return true;
}

StageEvidence const& GuidedInitializationService::evidence_for(Stage stage) const noexcept {
  switch (stage) {
    case Stage::drivers:
      return evidence_.drivers;
    case Stage::system_optimization:
      return evidence_.system_optimization;
    case Stage::software_installation:
      return evidence_.software_installation;
    case Stage::software_optimization:
      return evidence_.software_optimization;
  }
  return evidence_.drivers;
}

StageRecord& GuidedInitializationService::stage_for(FlowRecord& record) noexcept {
  return record.stages[index_for(record.current_stage)];
}

StageRecord const& GuidedInitializationService::stage_for(
    FlowRecord const& record) const noexcept {
  return record.stages[index_for(record.current_stage)];
}

FlowRecord* GuidedInitializationService::active_record() noexcept {
  if (!active_id_.has_value()) {
    return nullptr;
  }
  auto found = std::ranges::find(records_, *active_id_, &FlowRecord::id);
  return found == records_.end() ? nullptr : &*found;
}

FlowRecord const* GuidedInitializationService::active_record() const noexcept {
  if (!active_id_.has_value()) {
    return nullptr;
  }
  auto found = std::ranges::find(records_, *active_id_, &FlowRecord::id);
  return found == records_.end() ? nullptr : &*found;
}

std::int64_t GuidedInitializationService::now_milliseconds() const noexcept {
  return clock_.now().time_since_epoch().count();
}

Summary GuidedInitializationService::summarize(FlowRecord const& record) const {
  Summary result;
  for (auto const& stage : record.stages) {
    switch (stage.state) {
      case StageState::completed:
        ++result.completed;
        break;
      case StageState::partial:
        ++result.partial;
        break;
      case StageState::failed:
        ++result.failed;
        break;
      case StageState::skipped:
        ++result.skipped;
        break;
      case StageState::no_applicable_items:
        ++result.no_applicable_items;
        break;
      case StageState::not_executed:
        ++result.not_executed;
        break;
      case StageState::result_confirmation_pending:
        ++result.result_confirmation_pending;
        break;
      case StageState::waiting_for_restart:
        ++result.waiting_for_restart;
        break;
      case StageState::waiting_explorer_restart:
        ++result.waiting_explorer_restart;
        break;
      case StageState::emergency_withdrawn:
        ++result.emergency_withdrawn;
        break;
      case StageState::pending:
      case StageState::active:
      case StageState::external_handoff:
        break;
    }
  }
  result.externally_recognized = static_cast<std::size_t>(std::ranges::count_if(
      evidence_.external_handoffs, [](ExternalHandoffEvidence const& handoff) {
        return handoff.state == ExternalHandoffState::externally_recognized;
      }));
  result.retry_available = active_id_.has_value() &&
                           stage_for(record).state == StageState::failed;
  return result;
}

char const* to_string(Stage value) noexcept {
  switch (value) {
    case Stage::drivers:
      return "drivers";
    case Stage::system_optimization:
      return "system-optimization";
    case Stage::software_installation:
      return "software-installation";
    case Stage::software_optimization:
      return "software-optimization";
  }
  return "unknown";
}

char const* to_string(StageState value) noexcept {
  switch (value) {
    case StageState::pending:
      return "pending";
    case StageState::active:
      return "active";
    case StageState::completed:
      return "completed";
    case StageState::skipped:
      return "skipped";
    case StageState::no_applicable_items:
      return "no-applicable-items";
    case StageState::partial:
      return "partial";
    case StageState::failed:
      return "failed";
    case StageState::result_confirmation_pending:
      return "result-confirmation-pending";
    case StageState::waiting_explorer_restart:
      return "waiting-explorer-restart";
    case StageState::waiting_for_restart:
      return "waiting-for-restart";
    case StageState::emergency_withdrawn:
      return "emergency-withdrawn";
    case StageState::external_handoff:
      return "external-handoff";
    case StageState::not_executed:
      return "not-executed";
  }
  return "unknown";
}

char const* to_string(FlowState value) noexcept {
  switch (value) {
    case FlowState::active:
      return "active";
    case FlowState::waiting_for_restart:
      return "waiting-for-restart";
    case FlowState::awaiting_restart_continue:
      return "awaiting-restart-continue";
    case FlowState::completed:
      return "completed";
    case FlowState::cancelled:
      return "cancelled";
  }
  return "unknown";
}

char const* to_string(ActionCode value) noexcept {
  switch (value) {
    case ActionCode::succeeded:
      return "succeeded";
    case ActionCode::not_restored:
      return "not-restored";
    case ActionCode::read_only:
      return "read-only";
    case ActionCode::rejected:
      return "rejected";
    case ActionCode::no_active_flow:
      return "no-active-flow";
    case ActionCode::continuation_required:
      return "continuation-required";
    case ActionCode::persistence_failed:
      return "persistence-failed";
    case ActionCode::source_rejected:
      return "source-rejected";
  }
  return "unknown";
}

}  // namespace azzs::application::guided_initialization
