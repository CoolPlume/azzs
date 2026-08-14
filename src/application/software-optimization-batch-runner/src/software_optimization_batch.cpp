#include "azzs/application/software_optimization_batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/emergency_withdrawal_service.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/sogou_optimization.hpp"
#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/domain/emergency_withdrawal.hpp"

namespace azzs::application::software_optimization_batch {
namespace {

namespace catalog = domain::software_optimization_catalog;
namespace emergency = domain::emergency_withdrawal;

constexpr std::uint32_t k_format_version = 1;
constexpr std::size_t k_max_payload_bytes = 2U * 1024U * 1024U;
constexpr std::size_t k_max_text_bytes = 16U * 1024U;
constexpr std::size_t k_max_schemes = 64;
constexpr std::size_t k_max_options = 128;
constexpr std::size_t k_max_history = 32;

class Writer final {
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
    for (auto character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }
  [[nodiscard]] domain::StateBytes finish() && { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class Reader final {
 public:
  explicit Reader(std::span<std::byte const> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t& value) {
    if (remaining() < 1) return false;
    value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }
  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) return false;
      value |= static_cast<std::uint32_t>(part) << shift;
    }
    return true;
  }
  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) return false;
      value |= static_cast<std::uint64_t>(part) << shift;
    }
    return true;
  }
  [[nodiscard]] bool i64(std::int64_t& value) {
    std::uint64_t raw{};
    return u64(raw) && ((value = static_cast<std::int64_t>(raw)), true);
  }
  [[nodiscard]] bool text(std::string& value,
                          std::size_t maximum = k_max_text_bytes) {
    std::uint32_t size{};
    if (!u32(size) || size > maximum || remaining() < size) return false;
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
      std::uint8_t character{};
      if (!u8(character)) return false;
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

template <typename Enum>
void write_enum(Writer& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum>
[[nodiscard]] bool read_enum(Reader& reader, Enum& value) {
  std::uint8_t raw{};
  if (!reader.u8(raw)) return false;
  value = static_cast<Enum>(raw);
  return true;
}

void write_id(Writer& writer, catalog::StableId const& value) {
  writer.text(value.value);
}

[[nodiscard]] bool read_id(Reader& reader, catalog::StableId& value) {
  return reader.text(value.value, 256) && value.valid();
}

void write_range(Writer& writer, catalog::VersionRange const& value) {
  writer.text(value.minimum);
  writer.text(value.maximum);
}

[[nodiscard]] bool read_range(Reader& reader, catalog::VersionRange& value) {
  return reader.text(value.minimum, 256) && reader.text(value.maximum, 256);
}

void write_rule(Writer& writer, catalog::ControlledRule const& value) {
  write_enum(writer, value.kind);
  write_id(writer, value.definition);
}

[[nodiscard]] bool read_rule(Reader& reader, catalog::ControlledRule& value) {
  return read_enum(reader, value.kind) && read_id(reader, value.definition);
}

void write_ids(Writer& writer, std::vector<catalog::StableId> const& values) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (auto const& value : values) write_id(writer, value);
}

[[nodiscard]] bool read_ids(Reader& reader, std::vector<catalog::StableId>& values,
                             std::size_t maximum = k_max_options) {
  std::uint32_t count{};
  if (!reader.u32(count) || count > maximum) return false;
  values.clear();
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    catalog::StableId value;
    if (!read_id(reader, value)) return false;
    values.push_back(std::move(value));
  }
  return true;
}

void write_texts(Writer& writer, std::vector<std::string> const& values) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (auto const& value : values) writer.text(value);
}

[[nodiscard]] bool read_texts(Reader& reader, std::vector<std::string>& values,
                               std::size_t maximum = k_max_options) {
  std::uint32_t count{};
  if (!reader.u32(count) || count > maximum) return false;
  values.clear();
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string value;
    if (!reader.text(value, 256)) return false;
    values.push_back(std::move(value));
  }
  return true;
}

void write_optional_text(Writer& writer, std::optional<std::string> const& value) {
  writer.u8(value.has_value() ? 1 : 0);
  if (value.has_value()) writer.text(*value);
}

[[nodiscard]] bool read_optional_text(Reader& reader,
                                       std::optional<std::string>& value) {
  std::uint8_t present{};
  if (!reader.u8(present) || present > 1) return false;
  value.reset();
  if (present == 0) return true;
  std::string text;
  if (!reader.text(text, 256)) return false;
  value = std::move(text);
  return true;
}

void write_target(Writer& writer, catalog::TargetSoftware const& value) {
  write_id(writer, value.id);
  write_id(writer, value.identity_anchor);
  writer.u8(value.required_first_release ? 1 : 0);
  write_enum(writer, value.support_mode);
  write_range(writer, value.supported_versions);
  write_rule(writer, value.install_detection);
  write_rule(writer, value.version_detection);
  writer.u8(value.installation_item_id.has_value() ? 1 : 0);
  if (value.installation_item_id.has_value()) write_id(writer, *value.installation_item_id);
  writer.text(value.explanation_source);
}

[[nodiscard]] bool read_target(Reader& reader, catalog::TargetSoftware& value) {
  std::uint8_t required{};
  std::uint8_t has_installation_item{};
  if (!read_id(reader, value.id) || !read_id(reader, value.identity_anchor) ||
      !reader.u8(required) || required > 1 || !read_enum(reader, value.support_mode) ||
      !read_range(reader, value.supported_versions) ||
      !read_rule(reader, value.install_detection) ||
      !read_rule(reader, value.version_detection) ||
      !reader.u8(has_installation_item) || has_installation_item > 1) {
    return false;
  }
  value.required_first_release = required != 0;
  value.installation_item_id.reset();
  if (has_installation_item != 0) {
    catalog::StableId item;
    if (!read_id(reader, item)) return false;
    value.installation_item_id = std::move(item);
  }
  return reader.text(value.explanation_source, 4096);
}

void write_scheme(Writer& writer, catalog::SoftwareOptimizationScheme const& value) {
  write_id(writer, value.id);
  write_id(writer, value.target_id);
  writer.u8(value.required_first_release ? 1 : 0);
  write_enum(writer, value.automation);
  write_range(writer, value.supported_versions);
  writer.text(value.impact);
  write_enum(writer, value.risk);
  write_enum(writer, value.exit_requirement);
  write_enum(writer, value.restart_requirement);
  write_ids(writer, value.required_scheme_ids);
  write_ids(writer, value.conflicting_scheme_ids);
  writer.text(value.explanation_source);
  writer.text(value.manual_emergency_explanation);
  write_enum(writer, value.availability);
}

[[nodiscard]] bool read_scheme(Reader& reader,
                               catalog::SoftwareOptimizationScheme& value) {
  std::uint8_t required{};
  if (!read_id(reader, value.id) || !read_id(reader, value.target_id) ||
      !reader.u8(required) || required > 1 || !read_enum(reader, value.automation) ||
      !read_range(reader, value.supported_versions) || !reader.text(value.impact, 4096) ||
      !read_enum(reader, value.risk) || !read_enum(reader, value.exit_requirement) ||
      !read_enum(reader, value.restart_requirement) ||
      !read_ids(reader, value.required_scheme_ids) ||
      !read_ids(reader, value.conflicting_scheme_ids) ||
      !reader.text(value.explanation_source, 4096) ||
      !reader.text(value.manual_emergency_explanation, 4096) ||
      !read_enum(reader, value.availability)) {
    return false;
  }
  value.required_first_release = required != 0;
  value.options.clear();
  value.configuration_issues.clear();
  return true;
}

void write_option(Writer& writer, catalog::SoftwareOptimizationOption const& value) {
  write_id(writer, value.id);
  write_id(writer, value.scheme_id);
  write_range(writer, value.supported_versions);
  writer.text(value.impact);
  writer.u8(value.default_selected ? 1 : 0);
  writer.u8(value.required ? 1 : 0);
  write_enum(writer, value.automation);
  write_rule(writer, value.execution);
  write_rule(writer, value.state_detection);
  write_ids(writer, value.required_option_ids);
  write_ids(writer, value.conflicting_option_ids);
  write_texts(writer, value.allowed_values);
  write_optional_text(writer, value.default_value);
  writer.text(value.explanation_source);
}

[[nodiscard]] bool read_option(Reader& reader,
                               catalog::SoftwareOptimizationOption& value) {
  std::uint8_t default_selected{};
  std::uint8_t required{};
  if (!read_id(reader, value.id) || !read_id(reader, value.scheme_id) ||
      !read_range(reader, value.supported_versions) || !reader.text(value.impact, 4096) ||
      !reader.u8(default_selected) || default_selected > 1 || !reader.u8(required) ||
      required > 1 || !read_enum(reader, value.automation) ||
      !read_rule(reader, value.execution) || !read_rule(reader, value.state_detection) ||
      !read_ids(reader, value.required_option_ids) ||
      !read_ids(reader, value.conflicting_option_ids) ||
      !read_texts(reader, value.allowed_values) ||
      !read_optional_text(reader, value.default_value) ||
      !reader.text(value.explanation_source, 4096)) {
    return false;
  }
  value.default_selected = default_selected != 0;
  value.required = required != 0;
  return true;
}

void write_frozen_option(Writer& writer,
                         batch_domain::FrozenOptimizationOption const& value) {
  write_option(writer, value.option);
  write_optional_text(writer, value.selected_value);
}

[[nodiscard]] bool read_frozen_option(
    Reader& reader, batch_domain::FrozenOptimizationOption& value) {
  return read_option(reader, value.option) &&
         read_optional_text(reader, value.selected_value) && value.valid();
}

void write_frozen_scheme(Writer& writer,
                         batch_domain::FrozenOptimizationScheme const& value) {
  write_target(writer, value.target);
  write_scheme(writer, value.scheme);
  writer.text(value.detected_version);
  writer.text(value.risk_confirmation_id);
  writer.u8(value.forced_version_execution ? 1 : 0);
  writer.text(value.force_risk_version);
  writer.u8(value.force_version_confirmed ? 1 : 0);
  write_optional_text(writer, value.force_version_confirmation_id);
  writer.u32(static_cast<std::uint32_t>(value.selected_options.size()));
  for (auto const& option : value.selected_options) write_frozen_option(writer, option);
}

[[nodiscard]] bool read_frozen_scheme(
    Reader& reader, batch_domain::FrozenOptimizationScheme& value) {
  std::uint8_t forced{};
  std::uint8_t confirmed{};
  std::uint32_t option_count{};
  if (!read_target(reader, value.target) || !read_scheme(reader, value.scheme) ||
      !reader.text(value.detected_version, 256) ||
      !reader.text(value.risk_confirmation_id, 256) || !reader.u8(forced) ||
      forced > 1 || !reader.text(value.force_risk_version, 256) ||
      !reader.u8(confirmed) || confirmed > 1 ||
      !read_optional_text(reader, value.force_version_confirmation_id) ||
      !reader.u32(option_count) ||
      option_count == 0 || option_count > k_max_options) {
    return false;
  }
  value.forced_version_execution = forced != 0;
  value.force_version_confirmed = confirmed != 0;
  value.selected_options.clear();
  value.selected_options.reserve(option_count);
  for (std::uint32_t index = 0; index < option_count; ++index) {
    batch_domain::FrozenOptimizationOption option;
    if (!read_frozen_option(reader, option)) return false;
    value.selected_options.push_back(std::move(option));
  }
  return value.valid();
}

void write_plan(Writer& writer, batch_domain::FrozenOptimizationBatchPlan const& value) {
  writer.text(value.batch_id);
  writer.text(value.correlation_id);
  write_optional_text(writer, value.retry_of_batch_id);
  writer.u64(value.catalog_revision);
  writer.u64(value.emergency_notice_revision);
  writer.i64(value.frozen_at_milliseconds);
  writer.u32(static_cast<std::uint32_t>(value.schemes.size()));
  for (auto const& scheme : value.schemes) write_frozen_scheme(writer, scheme);
}

[[nodiscard]] bool read_plan(Reader& reader,
                             batch_domain::FrozenOptimizationBatchPlan& value) {
  std::uint32_t scheme_count{};
  if (!reader.text(value.batch_id, 256) || !reader.text(value.correlation_id, 256) ||
      !read_optional_text(reader, value.retry_of_batch_id) ||
      !reader.u64(value.catalog_revision) || !reader.u64(value.emergency_notice_revision) ||
      !reader.i64(value.frozen_at_milliseconds) || !reader.u32(scheme_count) ||
      scheme_count == 0 || scheme_count > k_max_schemes) {
    return false;
  }
  value.schemes.clear();
  value.schemes.reserve(scheme_count);
  for (std::uint32_t index = 0; index < scheme_count; ++index) {
    batch_domain::FrozenOptimizationScheme scheme;
    if (!read_frozen_scheme(reader, scheme)) return false;
    value.schemes.push_back(std::move(scheme));
  }
  return value.valid();
}

void write_progress(Writer& writer,
                    batch_domain::OptimizationStepProgress const& value) {
  writer.text(value.scheme_id);
  writer.text(value.option_id);
  write_enum(writer, value.state);
  writer.u32(value.attempt);
  writer.u8(value.execution_started ? 1 : 0);
  writer.u8(value.target_exit_confirmed ? 1 : 0);
  writer.u8(value.force_close_confirmation_requested ? 1 : 0);
  writer.u8(value.force_close_completed ? 1 : 0);
  writer.u64(value.emergency_notice_revision);
  writer.text(value.detail);
}

[[nodiscard]] bool read_progress(Reader& reader,
                                  batch_domain::OptimizationStepProgress& value) {
  std::uint8_t started{};
  std::uint8_t target_exit_confirmed{};
  std::uint8_t close_requested{};
  std::uint8_t close_completed{};
  if (!reader.text(value.scheme_id, 256) || !reader.text(value.option_id, 256) ||
      !read_enum(reader, value.state) || !reader.u32(value.attempt) ||
      !reader.u8(started) || started > 1 ||
      !reader.u8(target_exit_confirmed) || target_exit_confirmed > 1 ||
      !reader.u8(close_requested) ||
      close_requested > 1 || !reader.u8(close_completed) || close_completed > 1 ||
      !reader.u64(value.emergency_notice_revision) || !reader.text(value.detail, 4096)) {
    return false;
  }
  value.execution_started = started != 0;
  value.target_exit_confirmed = target_exit_confirmed != 0;
  value.force_close_confirmation_requested = close_requested != 0;
  value.force_close_completed = close_completed != 0;
  return value.valid();
}

void write_lease(Writer& writer, batch_domain::DurableLeaseBinding const& value) {
  writer.text(value.kind);
  writer.text(value.operation_id);
  writer.text(value.correlation_id);
  writer.text(value.lease_token_fingerprint);
  writer.u64(value.occupancy_revision);
}

[[nodiscard]] bool read_lease(Reader& reader,
                               batch_domain::DurableLeaseBinding& value) {
  return reader.text(value.kind, 64) && reader.text(value.operation_id, 256) &&
         reader.text(value.correlation_id, 256) &&
         reader.text(value.lease_token_fingerprint, 256) &&
         reader.u64(value.occupancy_revision) && value.valid();
}

void write_transition(Writer& writer,
                      batch_domain::LastDurableTransition const& value) {
  writer.u64(value.generation);
  writer.text(value.scheme_id);
  writer.text(value.option_id);
  write_enum(writer, value.step_state);
  write_enum(writer, value.outcome);
  writer.u8(value.coverage_gap ? 1 : 0);
}

[[nodiscard]] bool read_transition(Reader& reader,
                                    batch_domain::LastDurableTransition& value) {
  std::uint8_t coverage_gap{};
  return reader.u64(value.generation) && reader.text(value.scheme_id, 256) &&
         reader.text(value.option_id, 256) && read_enum(reader, value.step_state) &&
         read_enum(reader, value.outcome) && reader.u8(coverage_gap) &&
         coverage_gap <= 1 && ((value.coverage_gap = coverage_gap != 0), true) &&
         value.valid();
}

void write_record(Writer& writer, batch_domain::OptimizationBatchRecord const& value) {
  write_plan(writer, value.plan);
  write_enum(writer, value.state);
  writer.u8(value.close_requested ? 1 : 0);
  writer.u8(value.stop_requested ? 1 : 0);
  writer.u32(static_cast<std::uint32_t>(value.steps.size()));
  for (auto const& step : value.steps) write_progress(writer, step);
  writer.u64(value.generation);
  writer.u8(value.active_lease.has_value() ? 1 : 0);
  if (value.active_lease.has_value()) write_lease(writer, *value.active_lease);
  write_transition(writer, value.last_transition);
}

[[nodiscard]] bool read_record(Reader& reader,
                                batch_domain::OptimizationBatchRecord& value) {
  std::uint8_t close{};
  std::uint8_t stop{};
  std::uint8_t has_lease{};
  std::uint32_t step_count{};
  if (!read_plan(reader, value.plan) || !read_enum(reader, value.state) ||
      !reader.u8(close) || close > 1 || !reader.u8(stop) || stop > 1 ||
      !reader.u32(step_count) || step_count == 0 || step_count > k_max_options) {
    return false;
  }
  value.close_requested = close != 0;
  value.stop_requested = stop != 0;
  value.steps.clear();
  value.steps.reserve(step_count);
  for (std::uint32_t index = 0; index < step_count; ++index) {
    batch_domain::OptimizationStepProgress step;
    if (!read_progress(reader, step)) return false;
    value.steps.push_back(std::move(step));
  }
  if (!reader.u64(value.generation) || !reader.u8(has_lease) || has_lease > 1) {
    return false;
  }
  value.active_lease.reset();
  if (has_lease != 0) {
    batch_domain::DurableLeaseBinding lease;
    if (!read_lease(reader, lease)) return false;
    value.active_lease = std::move(lease);
  }
  return read_transition(reader, value.last_transition) && value.valid();
}

void write_history(Writer& writer, batch_domain::OptimizationBatchHistory const& value) {
  write_plan(writer, value.plan);
  write_enum(writer, value.final_state);
  writer.u32(static_cast<std::uint32_t>(value.steps.size()));
  for (auto const& step : value.steps) write_progress(writer, step);
  writer.text(value.reason);
}

[[nodiscard]] bool read_history(Reader& reader,
                                 batch_domain::OptimizationBatchHistory& value) {
  std::uint32_t step_count{};
  if (!read_plan(reader, value.plan) || !read_enum(reader, value.final_state) ||
      !reader.u32(step_count) || step_count == 0 || step_count > k_max_options) {
    return false;
  }
  value.steps.clear();
  value.steps.reserve(step_count);
  for (std::uint32_t index = 0; index < step_count; ++index) {
    batch_domain::OptimizationStepProgress step;
    if (!read_progress(reader, step)) return false;
    value.steps.push_back(std::move(step));
  }
  return reader.text(value.reason, 4096) && value.valid();
}

struct PersistedState final {
  std::optional<batch_domain::OptimizationBatchRecord> active;
  std::vector<batch_domain::OptimizationBatchHistory> history;
};

[[nodiscard]] std::optional<domain::StateBytes> encode(PersistedState const& state) {
  Writer writer;
  writer.u32(k_format_version);
  writer.u8(state.active.has_value() ? 1 : 0);
  if (state.active.has_value()) {
    if (!state.active->valid()) return std::nullopt;
    write_record(writer, *state.active);
  }
  if (state.history.size() > k_max_history ||
      !std::ranges::all_of(state.history,
                           &batch_domain::OptimizationBatchHistory::valid)) {
    return std::nullopt;
  }
  writer.u32(static_cast<std::uint32_t>(state.history.size()));
  for (auto const& entry : state.history) write_history(writer, entry);
  auto bytes = std::move(writer).finish();
  return bytes.size() <= k_max_payload_bytes ? std::optional{std::move(bytes)}
                                               : std::nullopt;
}

[[nodiscard]] std::optional<PersistedState> decode(domain::StateBytes const& bytes) {
  if (bytes.size() > k_max_payload_bytes) return std::nullopt;
  Reader reader{bytes};
  std::uint32_t version{};
  std::uint8_t has_active{};
  if (!reader.u32(version) || version != k_format_version ||
      !reader.u8(has_active) || has_active > 1) {
    return std::nullopt;
  }
  PersistedState state;
  if (has_active != 0) {
    batch_domain::OptimizationBatchRecord active;
    if (!read_record(reader, active)) return std::nullopt;
    state.active = std::move(active);
  }
  std::uint32_t history_count{};
  if (!reader.u32(history_count) || history_count > k_max_history) {
    return std::nullopt;
  }
  state.history.reserve(history_count);
  for (std::uint32_t index = 0; index < history_count; ++index) {
    batch_domain::OptimizationBatchHistory entry;
    if (!read_history(reader, entry)) return std::nullopt;
    state.history.push_back(std::move(entry));
  }
  return reader.remaining() == 0 ? std::optional{std::move(state)} : std::nullopt;
}

[[nodiscard]] std::string lease_token_fingerprint(std::string_view token) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (auto const byte : token) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return "lease-" + std::to_string(hash);
}

[[nodiscard]] bool terminal_batch_state(batch_domain::OptimizationBatchState state) {
  return state == batch_domain::OptimizationBatchState::stopped ||
         state == batch_domain::OptimizationBatchState::completed;
}

[[nodiscard]] bool execution_succeeded(StepExecutionCode code) noexcept {
  return code == StepExecutionCode::applied ||
         code == StepExecutionCode::already_effective;
}

[[nodiscard]] ExecutionResult execution_result(
    batch_domain::OptimizationStepState state) noexcept {
  switch (state) {
    case batch_domain::OptimizationStepState::optimized:
      return ExecutionResult::succeeded;
    case batch_domain::OptimizationStepState::failed:
    case batch_domain::OptimizationStepState::blocked_by_withdrawal:
      return ExecutionResult::failed;
    case batch_domain::OptimizationStepState::not_executed:
      return ExecutionResult::cancelled;
    case batch_domain::OptimizationStepState::pending:
    case batch_domain::OptimizationStepState::executing:
    case batch_domain::OptimizationStepState::awaiting_target_exit:
    case batch_domain::OptimizationStepState::force_close_confirmation_pending:
    case batch_domain::OptimizationStepState::result_confirmation_pending:
    case batch_domain::OptimizationStepState::waiting_restart:
      return ExecutionResult::unknown;
  }
  return ExecutionResult::unknown;
}

[[nodiscard]] bool valid_confirmation(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 256 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return character >= 0x20 && character < 0x7f;
         });
}

[[nodiscard]] bool is_sogou_controlled_option(
    batch_domain::FrozenOptimizationOption const& option) noexcept {
  auto const& declared = option.option;
  return declared.scheme_id.value == "sogou-input-recommended-v1" &&
         declared.id.value.starts_with("sogou-input-") &&
         declared.execution.kind == catalog::RuleKind::built_in_definition &&
         declared.state_detection.kind == catalog::RuleKind::built_in_definition &&
         declared.execution.definition.value.starts_with("sogou.optimize.execute.") &&
         declared.state_detection.definition.value.starts_with("sogou.optimize.detect.");
}

[[nodiscard]] bool is_sogou_controlled_scheme(
    batch_domain::FrozenOptimizationScheme const& scheme) noexcept {
  return scheme.target.id.value == "sogou-input-target-v1" &&
         scheme.target.identity_anchor.value == "vendor.sogou.input.windows" &&
         scheme.scheme.id.value == "sogou-input-recommended-v1";
}

[[nodiscard]] bool selected_option_matches(
    discovery_app::SoftwareOptimizationSubmissionRequest const& submission,
    std::string_view scheme_id, std::string_view option_id) {
  return std::ranges::any_of(submission.selected_options,
                             [&](auto const& selected) {
                               return selected.scheme_id.value == scheme_id &&
                                      selected.option_id.value == option_id;
                             });
}

[[nodiscard]] discovery_app::discovery_domain::SelectedOption const*
find_selected(discovery_app::SoftwareOptimizationSubmissionRequest const& submission,
              std::string_view scheme_id, std::string_view option_id) {
  auto found = std::ranges::find_if(submission.selected_options,
                                    [&](auto const& selected) {
                                      return selected.scheme_id.value == scheme_id &&
                                             selected.option_id.value == option_id;
                                    });
  return found == submission.selected_options.end() ? nullptr : &*found;
}

[[nodiscard]] SchemeExecutionConfirmation const* find_confirmation(
    std::vector<SchemeExecutionConfirmation> const& confirmations,
    std::string_view scheme_id) {
  auto found = std::ranges::find(confirmations, scheme_id,
                                 &SchemeExecutionConfirmation::scheme_id);
  return found == confirmations.end() ? nullptr : &*found;
}

[[nodiscard]] bool contains_confirmation_for_only_selected_schemes(
    OptimizationBatchStartRequest const& request,
    std::vector<std::string> const& selected_scheme_ids) {
  if (request.confirmations.size() != selected_scheme_ids.size()) return false;
  for (auto const& confirmation : request.confirmations) {
    if (!valid_confirmation(confirmation.scheme_id) ||
        !valid_confirmation(confirmation.risk_confirmation_id) ||
        std::ranges::find(selected_scheme_ids, confirmation.scheme_id) ==
            selected_scheme_ids.end()) {
      return false;
    }
    if (confirmation.forced_version_execution &&
        (!valid_confirmation(confirmation.force_risk_version) ||
         !confirmation.force_version_confirmation_id.has_value() ||
         !valid_confirmation(*confirmation.force_version_confirmation_id))) {
      return false;
    }
    if (!confirmation.forced_version_execution &&
        (!confirmation.force_risk_version.empty() ||
         confirmation.force_version_confirmation_id.has_value())) {
      return false;
    }
  }
  return true;
}

}  // namespace

SogouOptimizationBatchExecutor::SogouOptimizationBatchExecutor(
    sogou_optimization::SogouOptimizationService& service) noexcept
    : service_(service) {}

StepExecutionObservation SogouOptimizationBatchExecutor::execute(
    batch_domain::FrozenOptimizationOption const& option) {
  if (!is_sogou_controlled_option(option)) {
    return {.code = StepExecutionCode::unsupported,
            .detail = "frozen option is not a registered Sogou optimization identity"};
  }
  auto observed = service_.execute_option(
      option.option, option.selected_value.has_value()
                         ? std::optional<std::string_view>{*option.selected_value}
                         : std::nullopt);
  using Status = sogou_optimization::SogouOptimizationStatus;
  switch (observed.status) {
    case Status::succeeded: return {.code = StepExecutionCode::applied,
                                    .detail = std::move(observed.detail)};
    case Status::already_effective: return {.code = StepExecutionCode::already_effective,
                                            .detail = std::move(observed.detail)};
    case Status::pending_confirmation: return {.code = StepExecutionCode::confirmation_required,
                                               .detail = std::move(observed.detail)};
    case Status::version_not_supported:
    case Status::not_installed:
      return {.code = StepExecutionCode::version_not_supported,
              .detail = std::move(observed.detail)};
    case Status::unsupported: return {.code = StepExecutionCode::unsupported,
                                      .detail = std::move(observed.detail)};
    case Status::failed:
    case Status::invalid_request:
    case Status::cancelled:
      return {.code = StepExecutionCode::failed, .detail = std::move(observed.detail)};
  }
  return {.code = StepExecutionCode::failed,
          .detail = "the Sogou optimization adapter returned an unknown result"};
}

StepVerificationObservation SogouOptimizationBatchExecutor::verify(
    batch_domain::FrozenOptimizationOption const& option) {
  if (!is_sogou_controlled_option(option)) {
    return {.code = StepVerificationCode::failed,
            .detail = "frozen option is not a registered Sogou optimization identity"};
  }
  auto observed = service_.detect_option(
      option.option, option.selected_value.has_value()
                         ? std::optional<std::string_view>{*option.selected_value}
                         : std::nullopt);
  using Status = sogou_optimization::SogouOptimizationStatus;
  switch (observed.status) {
    case Status::already_effective: return {.code = StepVerificationCode::optimized,
                                            .detail = std::move(observed.detail)};
    case Status::succeeded: return {.code = StepVerificationCode::not_optimized,
                                    .detail = std::move(observed.detail)};
    case Status::pending_confirmation: return {.code = StepVerificationCode::confirmation_required,
                                               .detail = std::move(observed.detail)};
    case Status::version_not_supported:
    case Status::not_installed:
      return {.code = StepVerificationCode::version_not_supported,
              .detail = std::move(observed.detail)};
    case Status::failed:
    case Status::invalid_request:
    case Status::unsupported:
    case Status::cancelled:
      return {.code = StepVerificationCode::failed, .detail = std::move(observed.detail)};
  }
  return {.code = StepVerificationCode::failed,
          .detail = "the Sogou optimization detector returned an unknown result"};
}

TargetExitObservation SogouOptimizationBatchExecutor::observe_target_exit(
    batch_domain::FrozenOptimizationScheme const& scheme) {
  if (!is_sogou_controlled_scheme(scheme)) {
    return {.detail = "frozen target is not the registered Sogou identity"};
  }
  // The present Windows Sogou adapter has no verified process/UI identity
  // contract. It therefore cannot turn a user acknowledgement into a claim
  // that the target has safely exited.
  return {.detail = "no verified Sogou target-exit observer is registered"};
}

EmergencyWithdrawalOptimizationAuthorization::
    EmergencyWithdrawalOptimizationAuthorization(
        EmergencyWithdrawalService& withdrawals) noexcept
    : withdrawals_(withdrawals) {}

WithdrawalAuthorization EmergencyWithdrawalOptimizationAuthorization::authorize(
    batch_domain::FrozenOptimizationScheme const& scheme,
    batch_domain::FrozenOptimizationOption const& option) {
  auto authorize = [&](std::string const& stable_id) {
    return withdrawals_.authorize({.stable_id = stable_id,
                                  .category = emergency::OperationCategory::software_optimization,
                                  .version = scheme.detected_version});
  };
  auto scheme_result = authorize(scheme.scheme.id.value);
  if (scheme_result.code == OperationAuthorizationCode::blocked) {
    return {.code = WithdrawalAuthorizationCode::blocked,
            .notice_revision = scheme_result.notice_revision,
            .reason = scheme_result.reason};
  }
  if (scheme_result.code != OperationAuthorizationCode::allowed &&
      scheme_result.code != OperationAuthorizationCode::allowed_unknown) {
    return {.code = WithdrawalAuthorizationCode::unavailable,
            .notice_revision = scheme_result.notice_revision,
            .reason = scheme_result.reason.empty()
                          ? "software optimization withdrawal state is not authoritative"
                          : scheme_result.reason};
  }
  auto option_result = authorize(option.option.id.value);
  if (option_result.code == OperationAuthorizationCode::blocked) {
    return {.code = WithdrawalAuthorizationCode::blocked,
            .notice_revision = option_result.notice_revision,
            .reason = option_result.reason};
  }
  if (option_result.code != OperationAuthorizationCode::allowed &&
      option_result.code != OperationAuthorizationCode::allowed_unknown) {
    return {.code = WithdrawalAuthorizationCode::unavailable,
            .notice_revision = option_result.notice_revision,
            .reason = option_result.reason.empty()
                          ? "software optimization withdrawal state is not authoritative"
                          : option_result.reason};
  }
  return {.code = WithdrawalAuthorizationCode::allowed,
          .notice_revision = std::max(scheme_result.notice_revision,
                                      option_result.notice_revision)};
}

DiscoveryOptimizationBatchPlanSource::DiscoveryOptimizationBatchPlanSource(
    SoftwareOptimizationCatalogLifecycle& catalogs,
    discovery_app::SoftwareOptimizationDiscoveryService& discovery) noexcept
    : catalogs_(catalogs), discovery_(discovery) {}

FrozenPlanAdmission DiscoveryOptimizationBatchPlanSource::freeze(
    OptimizationBatchStartRequest const& request) {
  if (!valid_confirmation(request.batch_id) || !valid_confirmation(request.correlation_id) ||
      request.frozen_at_milliseconds < 0 || request.submission.empty()) {
    return {.detail = "optimization batch start request is incomplete"};
  }
  auto catalog_snapshot = catalogs_.snapshot();
  auto discovery_snapshot = discovery_.snapshot();
  if (!catalog_snapshot.current.has_value() || !discovery_snapshot.has_current_catalog ||
      request.submission.catalog_revision == 0 ||
      request.submission.catalog_revision != catalog_snapshot.current->revision ||
      request.submission.catalog_revision != discovery_snapshot.discovery.catalog_revision) {
    return {.detail = "optimization selection is stale relative to the current catalog"};
  }

  std::vector<std::string> selected_scheme_ids;
  for (auto const& selected : request.submission.selected_options) {
    if (!selected.scheme_id.valid() || !selected.option_id.valid()) {
      return {.detail = "optimization selection contains an invalid stable identifier"};
    }
    if (std::ranges::find(selected_scheme_ids, selected.scheme_id.value) ==
        selected_scheme_ids.end()) {
      selected_scheme_ids.push_back(selected.scheme_id.value);
    }
  }
  if (!contains_confirmation_for_only_selected_schemes(request, selected_scheme_ids)) {
    return {.detail = "every selected scheme needs exactly one bounded risk confirmation"};
  }

  batch_domain::FrozenOptimizationBatchPlan plan{
      .batch_id = request.batch_id,
      .correlation_id = request.correlation_id,
      .catalog_revision = catalog_snapshot.current->revision,
      .frozen_at_milliseconds = request.frozen_at_milliseconds,
  };
  std::size_t frozen_count{};
  for (auto const& target_view : discovery_snapshot.discovery.targets) {
    if (target_view.presence != discovery_app::discovery_domain::TargetPresence::detected ||
        !target_view.installed_version.has_value()) {
      continue;
    }
    for (auto const& scheme_view : target_view.schemes) {
      auto const* confirmation = find_confirmation(request.confirmations,
                                                   scheme_view.scheme.id.value);
      if (confirmation == nullptr) continue;
      auto const regular = scheme_view.state ==
                           discovery_app::discovery_domain::SchemeState::can_optimize;
      auto const forced = confirmation->forced_version_execution &&
                          scheme_view.state == discovery_app::discovery_domain::
                                                   SchemeState::version_not_applicable;
      if (!regular && !forced) {
        return {.detail = "the selected scheme is not executable in the current discovery state"};
      }
      batch_domain::FrozenOptimizationScheme frozen_scheme{
          .target = target_view.target,
          .scheme = scheme_view.scheme,
          .detected_version = *target_view.installed_version,
          .risk_confirmation_id = confirmation->risk_confirmation_id,
          .forced_version_execution = forced,
          .force_risk_version = forced ? confirmation->force_risk_version : std::string{},
          .force_version_confirmed = forced,
          .force_version_confirmation_id =
              forced ? confirmation->force_version_confirmation_id : std::nullopt,
      };
      for (auto const& option_view : scheme_view.options) {
        auto const* selected = find_selected(request.submission,
                                             scheme_view.scheme.id.value,
                                             option_view.option.id.value);
        if (selected == nullptr) continue;
        if ((!forced && option_view.state !=
                            discovery_app::discovery_domain::OptionState::needs_optimization) ||
            (forced && option_view.state !=
                           discovery_app::discovery_domain::OptionState::version_not_applicable &&
             option_view.state !=
                 discovery_app::discovery_domain::OptionState::needs_optimization)) {
          return {.detail = "a selected option is not executable in the current discovery state"};
        }
        auto value = selected->value.has_value() ? selected->value
                                                 : option_view.option.default_value;
        frozen_scheme.selected_options.push_back(
            {.option = option_view.option, .selected_value = std::move(value)});
        ++frozen_count;
      }
      if (!frozen_scheme.selected_options.empty()) {
        if (!frozen_scheme.valid()) {
          return {.detail = "the frozen optimization scheme violates its closed contract"};
        }
        plan.schemes.push_back(std::move(frozen_scheme));
      }
    }
  }
  if (frozen_count != request.submission.selected_options.size() || !plan.valid()) {
    return {.detail = "the submitted selection no longer matches the executable catalog projection"};
  }
  return {.accepted = true, .plan = std::move(plan)};
}

class SoftwareOptimizationBatchService::Impl final {
 public:
  Impl(DeviceStateStore& states, SharedOperationOccupancy& occupancy,
       ExecutionLog& log, SoftwareOptimizationBatchPlanSource& plans,
       SoftwareOptimizationStepExecutor& executor,
       SoftwareOptimizationWithdrawalAuthorization& withdrawals)
      : states_(states), occupancy_(occupancy), log_(log), plans_(plans),
        executor_(executor), withdrawals_(withdrawals) {}

  [[nodiscard]] OptimizationBatchActionResult restore() {
    std::scoped_lock lock{mutex_};
    auto read = states_.inspect(key_);
    if (read.mode == StateReadMode::uninitialized) {
      writable_ = true;
      error_.clear();
      return result_locked(OptimizationBatchActionCode::succeeded);
    }
    if ((read.mode != StateReadMode::writable &&
         read.mode != StateReadMode::recovered_previous) ||
        !read.snapshot.has_value()) {
      writable_ = false;
      error_ = read.error.empty() ? "software optimization batch state is not writable"
                                  : std::move(read.error);
      return result_locked(OptimizationBatchActionCode::read_only);
    }
    auto decoded = decode(read.snapshot->state.value.payload);
    if (!decoded.has_value()) {
      writable_ = false;
      error_ = "software optimization batch persistence format is unknown or corrupt";
      return result_locked(OptimizationBatchActionCode::read_only);
    }
    active_ = std::move(decoded->active);
    history_ = std::move(decoded->history);
    revision_ = read.snapshot->revision;
    writable_ = true;
    error_.clear();
    if (active_.has_value() && !terminal_batch_state(active_->state)) {
      // No prior process is allowed to infer that an effect did not happen.
      // Recovery may observe only; it cannot advance pending work automatically.
      for (auto& progress : active_->steps) {
        if (progress.state == batch_domain::OptimizationStepState::executing) {
          progress.state = batch_domain::OptimizationStepState::result_confirmation_pending;
          progress.detail = "the prior process ended after an optimization effect was requested";
        }
      }
      active_->state = batch_domain::OptimizationBatchState::recovery_required;
    }
    return result_locked(OptimizationBatchActionCode::succeeded);
  }

  [[nodiscard]] batch_domain::OptimizationBatchSnapshot snapshot() const {
    std::scoped_lock lock{mutex_};
    return snapshot_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult create(
      OptimizationBatchStartRequest const& request) {
    std::scoped_lock lock{mutex_};
    if (!writable_) return result_locked(OptimizationBatchActionCode::not_restored);
    auto admitted = plans_.freeze(request);
    if (!admitted.accepted || !admitted.plan.has_value() || !admitted.plan->valid()) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           admitted.detail.empty()
                               ? "optimization plan admission was rejected"
                               : std::move(admitted.detail));
    }
    auto plan = std::move(*admitted.plan);
    for (auto const& scheme : plan.schemes) {
      for (auto const& option : scheme.selected_options) {
        auto authorization = authorize_locked(scheme, option);
        plan.emergency_notice_revision = std::max(
            plan.emergency_notice_revision, authorization.notice_revision);
        if (authorization.code == WithdrawalAuthorizationCode::blocked) {
          return result_locked(
              OptimizationBatchActionCode::blocked,
              authorization.reason.empty()
                  ? "the new optimization batch is blocked by an emergency withdrawal"
                  : std::move(authorization.reason));
        }
        if (authorization.code != WithdrawalAuthorizationCode::allowed) {
          return result_locked(
              OptimizationBatchActionCode::blocked,
              authorization.reason.empty()
                  ? "the current emergency withdrawal state cannot authorize a new batch"
                  : std::move(authorization.reason));
        }
      }
    }
    return create_plan_locked(std::move(plan));
  }

  [[nodiscard]] OptimizationBatchActionResult advance() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(active_.has_value() ? OptimizationBatchActionCode::recovery_required
                                               : OptimizationBatchActionCode::no_active_batch);
    }
    if (active_->state == batch_domain::OptimizationBatchState::failed_closed ||
        active_->state == batch_domain::OptimizationBatchState::recovery_required ||
        active_->state == batch_domain::OptimizationBatchState::closing) {
      return result_locked(OptimizationBatchActionCode::recovery_required,
                           "the batch cannot continue after a close or unknown outcome");
    }
    if (active_->state == batch_domain::OptimizationBatchState::waiting_restart) {
      return result_locked(OptimizationBatchActionCode::blocked,
                           "the batch is handed over to the restart barrier");
    }

    auto index = current_index_locked();
    if (!index.has_value()) {
      active_->state = active_->stop_requested
                           ? batch_domain::OptimizationBatchState::stopped
                           : batch_domain::OptimizationBatchState::completed;
      auto lease = acquire_and_bind_locked();
      return lease.has_value()
                 ? commit_transition_locked(*lease, active_->steps.front(),
                                            "batch-completed")
                 : lease_failure_result_locked();
    }

    auto& progress = active_->steps[*index];
    auto step = step_at_locked(*index);
    if (step.scheme == nullptr || step.option == nullptr) {
      return fail_closed_result_locked(progress,
                                       "frozen optimization step cannot be resolved");
    }
    if (active_->stop_requested) {
      if (progress.state == batch_domain::OptimizationStepState::pending ||
          progress.state == batch_domain::OptimizationStepState::awaiting_target_exit ||
          progress.state == batch_domain::OptimizationStepState::force_close_confirmation_pending) {
        progress.state = batch_domain::OptimizationStepState::not_executed;
        progress.detail = "not started because the optimization batch was stopped";
        mark_later_pending_not_executed_locked(*index + 1);
        active_->state = batch_domain::OptimizationBatchState::stopped;
        auto lease = acquire_and_bind_locked();
        return lease.has_value()
                   ? commit_transition_locked(*lease, progress, "normal-stop")
                   : lease_failure_result_locked();
      }
      active_->state = batch_domain::OptimizationBatchState::stopping;
      return result_locked(OptimizationBatchActionCode::blocked,
                           "the current optimization requires a safe result boundary");
    }
    if (progress.state != batch_domain::OptimizationStepState::pending) {
      return result_locked(OptimizationBatchActionCode::blocked,
                           "the current optimization requires explicit confirmation or restart");
    }
    if (step.scheme->scheme.exit_requirement == catalog::ExitRequirement::graceful_exit &&
        !progress.target_exit_confirmed) {
      progress.state = batch_domain::OptimizationStepState::awaiting_target_exit;
      active_->state = batch_domain::OptimizationBatchState::awaiting_user;
      auto lease = acquire_and_bind_locked();
      return lease.has_value()
                 ? commit_transition_locked(*lease, progress, "await-target-exit")
                 : lease_failure_result_locked();
    }

    auto authorization = authorize_locked(*step.scheme, *step.option);
    progress.emergency_notice_revision = authorization.notice_revision;
    active_->plan.emergency_notice_revision = std::max(
        active_->plan.emergency_notice_revision, authorization.notice_revision);
    if (authorization.code == WithdrawalAuthorizationCode::blocked) {
      progress.state = batch_domain::OptimizationStepState::blocked_by_withdrawal;
      progress.detail = authorization.reason.empty() ? "blocked by emergency withdrawal"
                                                      : std::move(authorization.reason);
      active_->state = batch_domain::OptimizationBatchState::running;
      auto lease = acquire_and_bind_locked();
      return lease.has_value()
                 ? commit_transition_locked(*lease, progress, "withdrawal-blocked")
                 : lease_failure_result_locked();
    }
    if (authorization.code != WithdrawalAuthorizationCode::allowed) {
      progress.state = batch_domain::OptimizationStepState::failed;
      progress.detail = authorization.reason.empty()
                            ? "emergency withdrawal state is unavailable"
                            : std::move(authorization.reason);
      active_->state = batch_domain::OptimizationBatchState::running;
      auto lease = acquire_and_bind_locked();
      return lease.has_value()
                 ? commit_transition_locked(*lease, progress, "withdrawal-unavailable")
                 : lease_failure_result_locked();
    }

    progress.state = batch_domain::OptimizationStepState::executing;
    progress.execution_started = true;
    ++progress.attempt;
    active_->state = batch_domain::OptimizationBatchState::running;
    auto lease = acquire_and_bind_locked();
    if (!lease.has_value()) return lease_failure_result_locked();
    auto prepared = commit_transition_locked(*lease, progress, "execution-requested");
    if (!prepared.succeeded()) return prepared;

    auto executed = execute_locked(*step.option);
    apply_execution_locked(progress, *step.scheme, *step.option, executed);
    if (active_->close_requested) {
      active_->state = batch_domain::OptimizationBatchState::closing;
    }
    return commit_transition_locked(*lease, progress, "execution-observed");
  }

  [[nodiscard]] OptimizationBatchActionResult confirm_current_target_exit() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    auto index = current_index_locked();
    if (!index.has_value() ||
        active_->steps[*index].state != batch_domain::OptimizationStepState::awaiting_target_exit) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "the current optimization is not waiting for target exit");
    }
    auto step = step_at_locked(*index);
    if (step.scheme == nullptr) {
      return fail_closed_result_locked(active_->steps[*index],
                                       "frozen target-exit step cannot be resolved");
    }
    TargetExitObservation observed;
    try {
      observed = executor_.observe_target_exit(*step.scheme);
    } catch (...) {
      observed.detail = "the controlled target-exit observer threw";
    }
    if (!observed.known || !observed.exited) {
      active_->steps[*index].detail = observed.detail.empty()
                                          ? "the target exit cannot be verified"
                                          : std::move(observed.detail);
      return result_locked(OptimizationBatchActionCode::blocked,
                           "the target exit remains unverified; no configuration was modified");
    }
    auto& progress = active_->steps[*index];
    progress.target_exit_confirmed = true;
    progress.state = batch_domain::OptimizationStepState::pending;
    progress.detail = std::move(observed.detail);
    active_->state = batch_domain::OptimizationBatchState::running;
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress, "target-exit-observed")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult confirm_current_complete() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    auto index = current_index_locked();
    if (!index.has_value() || active_->steps[*index].state !=
                             batch_domain::OptimizationStepState::result_confirmation_pending) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "the current optimization has no pending result confirmation");
    }
    auto step = step_at_locked(*index);
    if (step.scheme == nullptr) {
      return fail_closed_result_locked(active_->steps[*index],
                                       "frozen optimization confirmation step cannot be resolved");
    }
    auto& progress = active_->steps[*index];
    progress.detail = "the user confirmed the optimization result after inspecting the target software";
    if (step.scheme->scheme.restart_requirement != catalog::RestartRequirement::none) {
      progress.state = batch_domain::OptimizationStepState::waiting_restart;
      active_->state = batch_domain::OptimizationBatchState::waiting_restart;
    } else {
      progress.state = batch_domain::OptimizationStepState::optimized;
      active_->state = batch_domain::OptimizationBatchState::running;
    }
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress, "user-result-confirmation")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult stop_current() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    auto index = current_index_locked();
    if (!index.has_value()) {
      active_->state = batch_domain::OptimizationBatchState::stopped;
      auto lease = acquire_and_bind_locked();
      return lease.has_value()
                 ? commit_transition_locked(*lease, active_->steps.front(), "normal-stop")
                 : lease_failure_result_locked();
    }
    auto& progress = active_->steps[*index];
    active_->stop_requested = true;
    mark_later_pending_not_executed_locked(*index + 1);
    if (progress.state == batch_domain::OptimizationStepState::pending ||
        progress.state == batch_domain::OptimizationStepState::awaiting_target_exit ||
        progress.state == batch_domain::OptimizationStepState::force_close_confirmation_pending) {
      progress.state = batch_domain::OptimizationStepState::not_executed;
      progress.detail = "not started because the optimization batch was stopped";
      active_->state = batch_domain::OptimizationBatchState::stopped;
    } else {
      active_->state = batch_domain::OptimizationBatchState::stopping;
    }
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress, "normal-stop-requested")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult request_force_close() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    auto index = current_index_locked();
    if (!index.has_value() || active_->steps[*index].state !=
                             batch_domain::OptimizationStepState::awaiting_target_exit) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "force close is only available while awaiting target exit");
    }
    auto& progress = active_->steps[*index];
    progress.force_close_confirmation_requested = true;
    progress.state = batch_domain::OptimizationStepState::force_close_confirmation_pending;
    active_->state = batch_domain::OptimizationBatchState::awaiting_user;
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress,
                                          "force-close-confirmation-requested")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult confirm_force_close() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    auto index = current_index_locked();
    if (!index.has_value() || active_->steps[*index].state !=
                             batch_domain::OptimizationStepState::force_close_confirmation_pending) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "there is no pending force-close confirmation");
    }
    auto step = step_at_locked(*index);
    if (step.scheme == nullptr) {
      return fail_closed_result_locked(active_->steps[*index],
                                       "frozen force-close step cannot be resolved");
    }
    TargetExitObservation observed;
    try {
      observed = executor_.force_close_target(*step.scheme);
    } catch (...) {
      observed.detail = "the controlled force-close adapter threw";
    }
    auto& progress = active_->steps[*index];
    progress.detail = observed.detail.empty()
                          ? "the target force-close result cannot be verified"
                          : std::move(observed.detail);
    if (observed.known && observed.exited) {
      progress.force_close_completed = true;
      progress.target_exit_confirmed = true;
      progress.state = batch_domain::OptimizationStepState::pending;
      active_->state = batch_domain::OptimizationBatchState::running;
    } else {
      // A confirmation click is not proof of process control. Keep the step
      // before its effect boundary and require an independently observed exit.
      progress.state = batch_domain::OptimizationStepState::awaiting_target_exit;
      active_->state = batch_domain::OptimizationBatchState::awaiting_user;
    }
    auto lease = acquire_and_bind_locked();
    if (!lease.has_value()) return lease_failure_result_locked();
    auto persisted = commit_transition_locked(*lease, progress, "force-close-observed");
    if (!persisted.succeeded()) return persisted;
    return observed.known && observed.exited
               ? persisted
               : result_locked(OptimizationBatchActionCode::unsupported,
                               "the target force-close result is not verified");
  }

  [[nodiscard]] OptimizationBatchActionResult cancel_force_close() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    auto index = current_index_locked();
    if (!index.has_value() || active_->steps[*index].state !=
                             batch_domain::OptimizationStepState::force_close_confirmation_pending) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "there is no pending force-close confirmation");
    }
    auto& progress = active_->steps[*index];
    progress.force_close_confirmation_requested = false;
    progress.state = batch_domain::OptimizationStepState::awaiting_target_exit;
    active_->state = batch_domain::OptimizationBatchState::awaiting_user;
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress,
                                          "force-close-confirmation-cancelled")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult request_close() {
    std::scoped_lock lock{mutex_};
    if (!active_.has_value()) return result_locked(OptimizationBatchActionCode::no_active_batch);
    if (terminal_batch_state(active_->state)) {
      return result_locked(OptimizationBatchActionCode::succeeded);
    }
    active_->close_requested = true;
    for (auto& progress : active_->steps) {
      if (progress.state == batch_domain::OptimizationStepState::executing) {
        progress.state = batch_domain::OptimizationStepState::result_confirmation_pending;
        progress.detail = "normal close occurred after an optimization effect was requested";
      }
    }
    active_->state = batch_domain::OptimizationBatchState::closing;
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, active_->steps.front(), "normal-close")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult recover_read_only() {
    std::scoped_lock lock{mutex_};
    if (!ready_for_command_locked()) {
      return result_locked(active_.has_value() ? OptimizationBatchActionCode::recovery_required
                                               : OptimizationBatchActionCode::no_active_batch);
    }
    if (active_->state != batch_domain::OptimizationBatchState::recovery_required &&
        active_->state != batch_domain::OptimizationBatchState::closing) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "read-only recovery is not required for this batch");
    }
    auto index = current_index_locked();
    if (!index.has_value()) {
      return result_locked(OptimizationBatchActionCode::blocked,
                           "no uncertain optimization result is available for recovery");
    }
    auto& progress = active_->steps[*index];
    if (!progress.execution_started) {
      return result_locked(OptimizationBatchActionCode::blocked,
                           "recovery cannot start a pending optimization step");
    }
    auto step = step_at_locked(*index);
    if (step.option == nullptr || step.scheme == nullptr) {
      return fail_closed_result_locked(progress,
                                       "frozen recovery step cannot be resolved");
    }
    auto observed = verify_locked(*step.option);
    apply_verification_locked(progress, *step.scheme, observed);
    // Even a positive observation cannot auto-start later work after a restart.
    active_->state = batch_domain::OptimizationBatchState::recovery_required;
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress, "recovery-observed",
                                          false)
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult continue_after_recovery() {
    std::scoped_lock lock{mutex_};
    if (!writable_ || !active_.has_value()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    if (active_->state != batch_domain::OptimizationBatchState::recovery_required) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "only an open recovered batch may be continued");
    }
    auto index = current_index_locked();
    if (index.has_value() && batch_domain::blocks_batch(active_->steps[*index].state)) {
      return result_locked(OptimizationBatchActionCode::recovery_required,
                           "the recovered optimization still requires verification or handling");
    }
    active_->state = index.has_value() ? batch_domain::OptimizationBatchState::ready
                                        : (active_->stop_requested
                                               ? batch_domain::OptimizationBatchState::stopped
                                               : batch_domain::OptimizationBatchState::completed);
    auto const& progress = index.has_value() ? active_->steps[*index]
                                               : active_->steps.front();
    auto lease = acquire_and_bind_locked();
    return lease.has_value()
               ? commit_transition_locked(*lease, progress, "recovery-continued")
               : lease_failure_result_locked();
  }

  [[nodiscard]] OptimizationBatchActionResult retry_current(
      OptimizationBatchStartRequest const& request) {
    std::scoped_lock lock{mutex_};
    if (!writable_ || !active_.has_value()) {
      return result_locked(OptimizationBatchActionCode::no_active_batch);
    }
    if (active_->state == batch_domain::OptimizationBatchState::failed_closed ||
        active_->state == batch_domain::OptimizationBatchState::recovery_required ||
        active_->state == batch_domain::OptimizationBatchState::closing ||
        active_->state == batch_domain::OptimizationBatchState::waiting_restart) {
      return result_locked(OptimizationBatchActionCode::recovery_required,
                           "the batch must be recovered before it can be retried");
    }
    auto index = retry_index_locked();
    if (!index.has_value()) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "only a failed or unconfirmed optimization may be retried");
    }
    auto step = step_at_locked(*index);
    if (step.scheme == nullptr || step.option == nullptr) {
      return fail_closed_result_locked(active_->steps[*index],
                                       "frozen retry step cannot be resolved");
    }
    StepVerificationObservation verified;
    try {
      verified = executor_.verify(*step.option);
    } catch (...) {
      verified = {.code = StepVerificationCode::failed,
                  .detail = "the controlled optimization verifier threw"};
    }
    if (verified.code != StepVerificationCode::not_optimized) {
      return result_locked(OptimizationBatchActionCode::blocked,
                           "current verification does not permit a new optimization retry");
    }
    WithdrawalAuthorization authorized;
    try {
      authorized = withdrawals_.authorize(*step.scheme, *step.option);
    } catch (...) {
      authorized = {.code = WithdrawalAuthorizationCode::unavailable,
                    .reason = "emergency withdrawal authorization threw"};
    }
    if (authorized.code != WithdrawalAuthorizationCode::allowed) {
      return result_locked(OptimizationBatchActionCode::blocked,
                           authorized.reason.empty()
                               ? "the retry is blocked by current emergency withdrawal state"
                               : std::move(authorized.reason));
    }
    auto admitted = plans_.freeze(request);
    if (!admitted.accepted || !admitted.plan.has_value()) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           admitted.detail.empty()
                               ? "the fresh retry plan was rejected"
                               : std::move(admitted.detail));
    }
    admitted.plan->retry_of_batch_id = active_->plan.batch_id;
    if (!is_single_retry_for_locked(*admitted.plan, *index) ||
        !admitted.plan->valid()) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "the fresh retry plan does not match the failed optimization");
    }
    if (history_.size() >= k_max_history) {
      return result_locked(OptimizationBatchActionCode::persistence_failed,
                           "optimization history capacity is exhausted");
    }
    auto old_batch_id = active_->plan.batch_id;
    active_->state = batch_domain::OptimizationBatchState::stopped;
    mark_later_pending_not_executed_locked(*index + 1);
    auto lease = acquire_and_bind_locked();
    if (!lease.has_value()) return lease_failure_result_locked();
    auto stopped = commit_transition_locked(*lease, active_->steps[*index], "retry-requested");
    if (!stopped.succeeded()) return stopped;
    history_.push_back({.plan = active_->plan,
                         .final_state = batch_domain::OptimizationBatchState::stopped,
                         .steps = active_->steps,
                         .reason = "superseded by fresh retry batch " + request.batch_id});
    auto retained = active_;
    active_.reset();
    if (persist_locked() != PersistOutcome::committed) {
      active_ = std::move(retained);
      history_.pop_back();
      return result_locked(OptimizationBatchActionCode::persistence_failed,
                           "the original retry history could not be persisted");
    }
    auto created = create_plan_locked(std::move(*admitted.plan));
    if (!created.succeeded()) {
      return created;
    }
    return result_locked(OptimizationBatchActionCode::succeeded,
                         "retry batch replaced " + old_batch_id);
  }

 private:
  struct StepReference final {
    batch_domain::FrozenOptimizationScheme const* scheme{};
    batch_domain::FrozenOptimizationOption const* option{};
  };

  enum class PersistOutcome { committed, failed, outcome_unknown };

  [[nodiscard]] StepExecutionObservation execute_locked(
      batch_domain::FrozenOptimizationOption const& option) {
    try {
      return executor_.execute(option);
    } catch (...) {
      return {.code = StepExecutionCode::failed,
              .detail = "the controlled optimization executor threw"};
    }
  }

  [[nodiscard]] StepVerificationObservation verify_locked(
      batch_domain::FrozenOptimizationOption const& option) {
    try {
      return executor_.verify(option);
    } catch (...) {
      return {.code = StepVerificationCode::failed,
              .detail = "the controlled optimization verifier threw"};
    }
  }

  [[nodiscard]] WithdrawalAuthorization authorize_locked(
      batch_domain::FrozenOptimizationScheme const& scheme,
      batch_domain::FrozenOptimizationOption const& option) {
    try {
      return withdrawals_.authorize(scheme, option);
    } catch (...) {
      return {.code = WithdrawalAuthorizationCode::unavailable,
              .reason = "emergency withdrawal authorization threw"};
    }
  }

  [[nodiscard]] batch_domain::OptimizationBatchSnapshot snapshot_locked() const {
    return {.active = active_, .history = history_, .writable = writable_, .error = error_};
  }

  [[nodiscard]] OptimizationBatchActionResult result_locked(
      OptimizationBatchActionCode code, std::string message = {}) const {
    return {.code = code, .snapshot = snapshot_locked(), .message = std::move(message)};
  }

  [[nodiscard]] bool ready_for_command_locked() const noexcept {
    return writable_ && active_.has_value() && !terminal_batch_state(active_->state);
  }

  [[nodiscard]] std::optional<std::size_t> current_index_locked() const {
    if (!active_.has_value()) return std::nullopt;
    for (std::size_t index = 0; index < active_->steps.size(); ++index) {
      if (!batch_domain::is_terminal(active_->steps[index].state)) return index;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> retry_index_locked() const {
    if (!active_.has_value()) return std::nullopt;
    if (terminal_batch_state(active_->state)) {
      for (std::size_t index = 0; index < active_->steps.size(); ++index) {
        if (batch_domain::requires_fresh_retry_snapshot(active_->steps[index].state)) {
          return index;
        }
      }
      return std::nullopt;
    }
    auto current = current_index_locked();
    return current.has_value() && retryable_locked(active_->steps[*current])
               ? current
               : std::nullopt;
  }

  [[nodiscard]] bool is_single_retry_for_locked(
      batch_domain::FrozenOptimizationBatchPlan const& plan,
      std::size_t failed_index) const noexcept {
    if (!active_.has_value() || failed_index >= active_->steps.size()) return false;
    std::size_t count{};
    batch_domain::FrozenOptimizationOption const* retry_option{};
    batch_domain::FrozenOptimizationScheme const* retry_scheme{};
    for (auto const& scheme : plan.schemes) {
      for (auto const& option : scheme.selected_options) {
        ++count;
        retry_scheme = &scheme;
        retry_option = &option;
      }
    }
    return count == 1 && retry_scheme != nullptr && retry_option != nullptr &&
           retry_scheme->scheme.id.value == active_->steps[failed_index].scheme_id &&
           retry_option->option.id.value == active_->steps[failed_index].option_id;
  }

  [[nodiscard]] StepReference step_at_locked(std::size_t flat_index) const {
    if (!active_.has_value()) return {};
    std::size_t cursor{};
    for (auto const& scheme : active_->plan.schemes) {
      for (auto const& option : scheme.selected_options) {
        if (cursor++ == flat_index) return {.scheme = &scheme, .option = &option};
      }
    }
    return {};
  }

  void mark_later_pending_not_executed_locked(std::size_t first) {
    for (std::size_t index = first; index < active_->steps.size(); ++index) {
      auto& progress = active_->steps[index];
      if (progress.state == batch_domain::OptimizationStepState::pending ||
          progress.state == batch_domain::OptimizationStepState::awaiting_target_exit ||
          progress.state == batch_domain::OptimizationStepState::force_close_confirmation_pending) {
        progress.state = batch_domain::OptimizationStepState::not_executed;
        progress.detail = "not started because the optimization batch was stopped";
      }
    }
  }

  [[nodiscard]] bool retryable_locked(
      batch_domain::OptimizationStepProgress const& progress) const noexcept {
    return progress.state == batch_domain::OptimizationStepState::failed ||
           progress.state == batch_domain::OptimizationStepState::result_confirmation_pending;
  }

  void apply_execution_locked(
      batch_domain::OptimizationStepProgress& progress,
      batch_domain::FrozenOptimizationScheme const& scheme,
      batch_domain::FrozenOptimizationOption const& option,
      StepExecutionObservation const& observed) {
    progress.detail = observed.detail;
    if (execution_succeeded(observed.code)) {
      auto verification = verify_locked(option);
      apply_verification_locked(progress, scheme, verification);
      return;
    }
    switch (observed.code) {
      case StepExecutionCode::confirmation_required:
        progress.state = batch_domain::OptimizationStepState::result_confirmation_pending;
        active_->state = batch_domain::OptimizationBatchState::awaiting_user;
        break;
      case StepExecutionCode::version_not_supported:
      case StepExecutionCode::unsupported:
      case StepExecutionCode::failed:
        progress.state = batch_domain::OptimizationStepState::failed;
        active_->state = batch_domain::OptimizationBatchState::running;
        break;
      case StepExecutionCode::applied:
      case StepExecutionCode::already_effective:
        break;
    }
  }

  void apply_verification_locked(
      batch_domain::OptimizationStepProgress& progress,
      batch_domain::FrozenOptimizationScheme const& scheme,
      StepVerificationObservation const& observed) {
    progress.detail = observed.detail;
    switch (observed.code) {
      case StepVerificationCode::optimized:
        if (scheme.scheme.restart_requirement != catalog::RestartRequirement::none) {
          progress.state = batch_domain::OptimizationStepState::waiting_restart;
          active_->state = batch_domain::OptimizationBatchState::waiting_restart;
        } else {
          progress.state = batch_domain::OptimizationStepState::optimized;
          active_->state = batch_domain::OptimizationBatchState::running;
        }
        break;
      case StepVerificationCode::not_optimized:
      case StepVerificationCode::version_not_supported:
        progress.state = batch_domain::OptimizationStepState::failed;
        active_->state = batch_domain::OptimizationBatchState::running;
        break;
      case StepVerificationCode::confirmation_required:
      case StepVerificationCode::failed:
        progress.state = batch_domain::OptimizationStepState::result_confirmation_pending;
        active_->state = batch_domain::OptimizationBatchState::awaiting_user;
        break;
    }
  }

  [[nodiscard]] OptimizationBatchActionResult create_plan_locked(
      batch_domain::FrozenOptimizationBatchPlan plan) {
    if (active_.has_value()) {
      if (!terminal_batch_state(active_->state)) {
        return result_locked(OptimizationBatchActionCode::blocked,
                             "an existing optimization batch owns the device entry point");
      }
      if (history_.size() >= k_max_history) {
        return result_locked(OptimizationBatchActionCode::persistence_failed,
                             "optimization history capacity is exhausted");
      }
      history_.push_back({.plan = active_->plan,
                          .final_state = active_->state,
                          .steps = active_->steps,
                          .reason = "retained before a later optimization batch"});
      active_.reset();
      if (persist_locked() != PersistOutcome::committed) {
        return result_locked(OptimizationBatchActionCode::persistence_failed,
                             "previous optimization history could not be retained");
      }
    }
    batch_domain::OptimizationBatchRecord record{
        .plan = std::move(plan),
        .state = batch_domain::OptimizationBatchState::ready,
        .generation = 1,
    };
    for (auto const& scheme : record.plan.schemes) {
      for (auto const& option : scheme.selected_options) {
        record.steps.push_back({.scheme_id = scheme.scheme.id.value,
                                .option_id = option.option.id.value});
      }
    }
    record.last_transition = {.generation = record.generation,
                              .scheme_id = record.steps.front().scheme_id,
                              .option_id = record.steps.front().option_id,
                              .step_state = record.steps.front().state,
                              .outcome = batch_domain::DurableTransitionOutcome::committed};
    if (!record.valid()) {
      return result_locked(OptimizationBatchActionCode::rejected,
                           "the frozen optimization record is invalid");
    }
    active_ = std::move(record);
    auto lease = acquire_and_bind_locked();
    if (!lease.has_value()) {
      auto code = last_lease_code_ == OccupancyResultCode::occupied
                      ? OptimizationBatchActionCode::occupied
                      : OptimizationBatchActionCode::persistence_failed;
      // If acquiring the lease succeeded but binding it durably did not, the
      // effect boundary is uncertain. Keep that record fail-closed instead of
      // discarding the only reconciliation evidence.
      if (active_->state != batch_domain::OptimizationBatchState::failed_closed) {
        active_.reset();
      }
      return result_locked(code, error_);
    }
    return commit_transition_locked(*lease, active_->steps.front(), "batch-created");
  }

  [[nodiscard]] std::optional<OperationLease> acquire_and_bind_locked() {
    if (!active_.has_value()) return std::nullopt;
    if (active_->active_lease.has_value()) {
      auto const& bound = *active_->active_lease;
      auto observed = occupancy_.inspect();
      if (observed.code == OccupancyResultCode::observed && observed.current.has_value() &&
          observed.current->identity.kind == bound.kind &&
          observed.current->identity.operation_id == bound.operation_id &&
          observed.current->identity.correlation_id == bound.correlation_id &&
          lease_token_fingerprint(observed.current->lease_token) ==
              bound.lease_token_fingerprint &&
          observed.revision == bound.occupancy_revision) {
        return OperationLease{.identity = observed.current->identity,
                              .lease_token = observed.current->lease_token,
                              .revision = bound.occupancy_revision};
      }
      error_ = observed.detail.empty()
                   ? "the durable optimization occupancy lease cannot be reconciled"
                   : std::move(observed.detail);
      mark_outcome_unknown_locked(active_->steps.front());
      return std::nullopt;
    }
    auto acquired = occupancy_.try_acquire({.kind = "software-optimization-batch",
                                             .operation_id = active_->plan.batch_id,
                                             .correlation_id = active_->plan.correlation_id});
    last_lease_code_ = acquired.code;
    if (acquired.code != OccupancyResultCode::acquired || !acquired.lease.has_value()) {
      error_ = acquired.detail.empty()
                   ? "software optimization occupancy cannot be acquired"
                   : std::move(acquired.detail);
      return std::nullopt;
    }
    auto const& lease = *acquired.lease;
    active_->active_lease = {.kind = lease.identity.kind,
                             .operation_id = lease.identity.operation_id,
                             .correlation_id = lease.identity.correlation_id,
                             .lease_token_fingerprint = lease_token_fingerprint(lease.lease_token),
                             .occupancy_revision = lease.revision};
    if (persist_locked() != PersistOutcome::committed) {
      mark_outcome_unknown_locked(active_->steps.front());
      return std::nullopt;
    }
    return lease;
  }

  [[nodiscard]] OptimizationBatchActionResult commit_transition_locked(
      OperationLease const& lease,
      batch_domain::OptimizationStepProgress const& progress,
      std::string_view action, bool complete_when_terminal = true) {
    ++active_->generation;
    active_->last_transition = {
        .generation = active_->generation,
        .scheme_id = progress.scheme_id,
        .option_id = progress.option_id,
        .step_state = progress.state,
        .outcome = batch_domain::DurableTransitionOutcome::committed,
    };
    if (complete_when_terminal && all_steps_terminal_locked()) {
      active_->state = active_->stop_requested
                           ? batch_domain::OptimizationBatchState::stopped
                           : batch_domain::OptimizationBatchState::completed;
    }
    auto persisted = persist_locked();
    if (persisted != PersistOutcome::committed) {
      mark_outcome_unknown_locked(progress);
      return result_locked(persisted == PersistOutcome::outcome_unknown
                               ? OptimizationBatchActionCode::outcome_unknown
                               : OptimizationBatchActionCode::persistence_failed);
    }
    if (!append_locked(progress, action)) {
      mark_outcome_unknown_locked(progress);
      return result_locked(OptimizationBatchActionCode::outcome_unknown,
                           "optimization record logging failed after a state transition");
    }
    if (!terminal_batch_state(active_->state)) {
      return result_locked(OptimizationBatchActionCode::succeeded);
    }
    auto released = occupancy_.release(lease);
    if (released.code != OccupancyResultCode::released) {
      error_ = released.detail.empty() ? "optimization occupancy release is unknown"
                                       : std::move(released.detail);
      mark_outcome_unknown_locked(progress);
      return result_locked(OptimizationBatchActionCode::outcome_unknown);
    }
    active_->active_lease.reset();
    auto cleared = persist_locked();
    if (cleared != PersistOutcome::committed) {
      error_ = "optimization occupancy was released but terminal state cleanup is unknown";
      mark_outcome_unknown_locked(progress);
      return result_locked(cleared == PersistOutcome::outcome_unknown
                               ? OptimizationBatchActionCode::outcome_unknown
                               : OptimizationBatchActionCode::persistence_failed);
    }
    return result_locked(OptimizationBatchActionCode::succeeded);
  }

  [[nodiscard]] bool all_steps_terminal_locked() const noexcept {
    return std::ranges::all_of(active_->steps, [](auto const& progress) {
      return batch_domain::is_terminal(progress.state);
    });
  }

  [[nodiscard]] bool append_locked(
      batch_domain::OptimizationStepProgress const& progress,
      std::string_view action) {
    try {
      auto receipt = log_.append(
          {.value = active_->plan.correlation_id},
          {.kind = ExecutionEventKind::state_transition,
           .component = "software-optimization-batch-runner",
           .stage = std::string{action},
           .result = execution_result(progress.state),
           .fields = {{"batch_id", active_->plan.batch_id,
                       DiagnosticValueDisposition::retain},
                      {"scheme_id", progress.scheme_id,
                       DiagnosticValueDisposition::retain},
                      {"option_id", progress.option_id,
                       DiagnosticValueDisposition::retain},
                      {"step_state", batch_domain::to_string(progress.state),
                       DiagnosticValueDisposition::retain},
                      {"catalog_revision", std::to_string(active_->plan.catalog_revision),
                       DiagnosticValueDisposition::retain},
                      {"emergency_notice_revision",
                       std::to_string(progress.emergency_notice_revision),
                       DiagnosticValueDisposition::retain}}});
      if (receipt.persisted) return true;
      error_ = receipt.error.empty() ? "optimization execution log persistence failed"
                                     : std::move(receipt.error);
    } catch (...) {
      error_ = "optimization execution log threw during durable transition recording";
    }
    return false;
  }

  void mark_outcome_unknown_locked(
      batch_domain::OptimizationStepProgress const& progress) {
    if (!active_.has_value()) return;
    active_->state = batch_domain::OptimizationBatchState::failed_closed;
    ++active_->generation;
    active_->last_transition = {
        .generation = active_->generation,
        .scheme_id = progress.scheme_id,
        .option_id = progress.option_id,
        .step_state = progress.state,
        .outcome = batch_domain::DurableTransitionOutcome::outcome_unknown,
        .coverage_gap = true,
    };
    static_cast<void>(persist_locked());
    try {
      static_cast<void>(log_.append(
          {.value = active_->plan.correlation_id},
          {.kind = ExecutionEventKind::coverage_gap,
           .component = "software-optimization-batch-runner",
           .stage = "durable-outcome-unknown",
           .result = ExecutionResult::unknown,
           .coverage_gap = std::optional<CoverageGap>{CoverageGap{
               .kind = CoverageGapKind::unknown_after_last_persisted,
               .reason = "software optimization outcome requires read-only recovery"}},
           .fields = {{"batch_id", active_->plan.batch_id,
                       DiagnosticValueDisposition::retain}}}));
    } catch (...) {
      // The state record, not a best-effort coverage event, decides safety.
    }
  }

  [[nodiscard]] OptimizationBatchActionResult fail_closed_result_locked(
      batch_domain::OptimizationStepProgress const& progress, std::string detail) {
    error_ = std::move(detail);
    mark_outcome_unknown_locked(progress);
    return result_locked(OptimizationBatchActionCode::outcome_unknown, error_);
  }

  [[nodiscard]] OptimizationBatchActionResult lease_failure_result_locked() const {
    return result_locked(last_lease_code_ == OccupancyResultCode::occupied
                             ? OptimizationBatchActionCode::occupied
                             : OptimizationBatchActionCode::persistence_failed,
                         error_);
  }

  [[nodiscard]] PersistOutcome persist_locked() {
    auto bytes = encode({.active = active_, .history = history_});
    if (!bytes.has_value()) {
      error_ = "software optimization batch state exceeds its closed persistence limits";
      return PersistOutcome::failed;
    }
    domain::DeviceState state{
        .value = {.schema = 2,
                  .minimum_reader = 1,
                  .minimum_writer = 2,
                  .payload = std::move(*bytes)},
    };
    StateCommitResult committed = revision_.has_value()
        ? states_.commit({.key = key_, .expected_revision = *revision_, .state = std::move(state)})
        : states_.initialize(key_, std::move(state));
    if (committed.status == StateCommitStatus::committed && committed.snapshot.has_value()) {
      revision_ = committed.snapshot->revision;
      return PersistOutcome::committed;
    }
    error_ = committed.error.empty() ? "software optimization batch state commit failed"
                                     : std::move(committed.error);
    return committed.status == StateCommitStatus::outcome_unknown
               ? PersistOutcome::outcome_unknown
               : PersistOutcome::failed;
  }

  DeviceStateStore& states_;
  SharedOperationOccupancy& occupancy_;
  ExecutionLog& log_;
  SoftwareOptimizationBatchPlanSource& plans_;
  SoftwareOptimizationStepExecutor& executor_;
  SoftwareOptimizationWithdrawalAuthorization& withdrawals_;
  domain::StateKey key_ = domain::StateKey::machine(
      domain::AggregateId{"software-optimization-batch"});
  std::optional<domain::RevisionToken> revision_;
  std::optional<batch_domain::OptimizationBatchRecord> active_;
  std::vector<batch_domain::OptimizationBatchHistory> history_;
  bool writable_{false};
  std::string error_;
  OccupancyResultCode last_lease_code_{OccupancyResultCode::storage_error};
  mutable std::mutex mutex_;
};

SoftwareOptimizationBatchService::SoftwareOptimizationBatchService(
    DeviceStateStore& states, SharedOperationOccupancy& occupancy,
    ExecutionLog& log, SoftwareOptimizationBatchPlanSource& plans,
    SoftwareOptimizationStepExecutor& executor,
    SoftwareOptimizationWithdrawalAuthorization& withdrawals)
    : impl_(std::make_unique<Impl>(states, occupancy, log, plans, executor,
                                   withdrawals)) {}

SoftwareOptimizationBatchService::~SoftwareOptimizationBatchService() = default;

OptimizationBatchActionResult SoftwareOptimizationBatchService::restore() {
  return impl_->restore();
}

batch_domain::OptimizationBatchSnapshot SoftwareOptimizationBatchService::snapshot() const {
  return impl_->snapshot();
}

OptimizationBatchActionResult SoftwareOptimizationBatchService::create(
    OptimizationBatchStartRequest const& request) {
  return impl_->create(request);
}

OptimizationBatchActionResult SoftwareOptimizationBatchService::advance() {
  return impl_->advance();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::confirm_current_target_exit() {
  return impl_->confirm_current_target_exit();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::confirm_current_complete() {
  return impl_->confirm_current_complete();
}

OptimizationBatchActionResult SoftwareOptimizationBatchService::stop_current() {
  return impl_->stop_current();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::request_force_close() {
  return impl_->request_force_close();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::confirm_force_close() {
  return impl_->confirm_force_close();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::cancel_force_close() {
  return impl_->cancel_force_close();
}

OptimizationBatchActionResult SoftwareOptimizationBatchService::request_close() {
  return impl_->request_close();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::recover_read_only() {
  return impl_->recover_read_only();
}

OptimizationBatchActionResult
SoftwareOptimizationBatchService::continue_after_recovery() {
  return impl_->continue_after_recovery();
}

OptimizationBatchActionResult SoftwareOptimizationBatchService::retry_current(
    OptimizationBatchStartRequest const& request) {
  return impl_->retry_current(request);
}

char const* to_string(OptimizationBatchActionCode value) noexcept {
  switch (value) {
    case OptimizationBatchActionCode::succeeded: return "succeeded";
    case OptimizationBatchActionCode::not_restored: return "not-restored";
    case OptimizationBatchActionCode::rejected: return "rejected";
    case OptimizationBatchActionCode::occupied: return "occupied";
    case OptimizationBatchActionCode::read_only: return "read-only";
    case OptimizationBatchActionCode::persistence_failed: return "persistence-failed";
    case OptimizationBatchActionCode::outcome_unknown: return "outcome-unknown";
    case OptimizationBatchActionCode::no_active_batch: return "no-active-batch";
    case OptimizationBatchActionCode::blocked: return "blocked";
    case OptimizationBatchActionCode::recovery_required: return "recovery-required";
    case OptimizationBatchActionCode::confirmation_required:
      return "confirmation-required";
    case OptimizationBatchActionCode::unsupported: return "unsupported";
  }
  return "rejected";
}

}  // namespace azzs::application::software_optimization_batch
