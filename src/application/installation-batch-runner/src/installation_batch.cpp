#include "azzs/application/installation_batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/domain/controlled_install_profiles.hpp"

namespace azzs::application::installation_batch {
namespace {

namespace catalog_app = application::software_catalog;
namespace selection_app = application::software_selection;
namespace catalog = domain::software_catalog;
namespace cache = domain::offline_package_cache;
namespace selection = domain::software_selection;

constexpr std::uint32_t k_format_version = 5;
constexpr std::size_t k_max_payload_bytes = 2U * 1024U * 1024U;
constexpr std::size_t k_max_text_bytes = 1024U * 1024U;
constexpr std::size_t k_max_items = 128;
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
    std::uint64_t raw{};
    return u64(raw) && ((value = static_cast<std::int64_t>(raw)), true);
  }
  [[nodiscard]] bool text(std::string& value,
                          std::size_t maximum = k_max_text_bytes) {
    std::uint32_t size{};
    if (!u32(size) || size > maximum || remaining() < size) {
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

template <typename Enum>
void write_enum(Writer& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum>
[[nodiscard]] bool read_enum(Reader& reader, Enum& value) {
  std::uint8_t raw{};
  if (!reader.u8(raw)) {
    return false;
  }
  value = static_cast<Enum>(raw);
  return true;
}

[[nodiscard]] std::string lease_token_fingerprint(std::string_view token) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (auto const byte : token) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return "lease-" + std::to_string(hash);
}

void write_package(Writer& writer, selection::ResolvedPackage const& value) {
  writer.text(value.candidate.software_id);
  write_enum(writer, value.candidate.architecture);
  writer.text(value.candidate.version);
  writer.text("frozen-package");
  write_enum(writer, value.package_type);
  writer.u8(value.complete_package ? 1 : 0);
  writer.u8(value.network_required ? 1 : 0);
}

[[nodiscard]] bool read_package(Reader& reader, selection::ResolvedPackage& value) {
  std::uint8_t complete{};
  std::uint8_t network{};
  return reader.text(value.candidate.software_id, 256) &&
         read_enum(reader, value.candidate.architecture) &&
         reader.text(value.candidate.version, 256) &&
         reader.text(value.candidate.identity, 256) &&
         read_enum(reader, value.package_type) && reader.u8(complete) &&
         reader.u8(network) && complete <= 1 && network <= 1 &&
         ((value.complete_package = complete != 0), true) &&
         ((value.network_required = network != 0), true);
}

void write_source(Writer& writer, selection::ResolvedSourceSnapshot const& value) {
  writer.text(value.software_id);
  write_enum(writer, value.declared_purpose);
  writer.text("redacted-source");
  writer.text(value.version);
  writer.text("redacted-source");
  writer.text("controlled");
  writer.text("frozen");
  writer.u32(static_cast<std::uint32_t>(value.packages.size()));
  for (auto const& package : value.packages) {
    write_package(writer, package);
  }
  writer.u8(value.network_required ? 1 : 0);
  writer.i64(value.resolved_at_milliseconds);
  writer.text("frozen-resolver");
}

[[nodiscard]] bool read_source(Reader& reader,
                               selection::ResolvedSourceSnapshot& value) {
  std::uint32_t count{};
  std::uint8_t network{};
  if (!reader.text(value.software_id, 256) ||
      !read_enum(reader, value.declared_purpose) ||
      !reader.text(value.declared_address) || !reader.text(value.version, 256) ||
      !reader.text(value.actual_address) ||
      !reader.text(value.hosting_mechanism, 256) ||
      !reader.text(value.branch, 256) || !reader.u32(count) || count > k_max_items) {
    return false;
  }
  value.packages.clear();
  value.packages.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    selection::ResolvedPackage package;
    if (!read_package(reader, package)) {
      return false;
    }
    value.packages.push_back(std::move(package));
  }
  return reader.u8(network) && network <= 1 &&
         ((value.network_required = network != 0), true) &&
         reader.i64(value.resolved_at_milliseconds) &&
         reader.text(value.capability_version, 256);
}

void write_asset(Writer& writer, cache::CacheAsset const& value) {
  writer.text(value.identity.software_id);
  writer.text(value.identity.version);
  write_enum(writer, value.identity.architecture);
  writer.text(value.identity.source_identity);
  write_enum(writer, value.kind);
  writer.u8(value.resume_supported ? 1 : 0);
  writer.u8(value.expected_bytes.has_value() ? 1 : 0);
  if (value.expected_bytes.has_value()) {
    writer.u64(*value.expected_bytes);
  }
}

[[nodiscard]] bool read_asset(Reader& reader, cache::CacheAsset& value) {
  std::uint8_t resume{};
  std::uint8_t expected{};
  if (!reader.text(value.identity.software_id, 256) ||
      !reader.text(value.identity.version, 256) ||
      !read_enum(reader, value.identity.architecture) ||
      !reader.text(value.identity.source_identity, 256) ||
      !read_enum(reader, value.kind) || !reader.u8(resume) || !reader.u8(expected) ||
      resume > 1 || expected > 1) {
    return false;
  }
  value.resume_supported = resume != 0;
  value.expected_bytes.reset();
  if (expected != 0) {
    std::uint64_t bytes{};
    if (!reader.u64(bytes)) {
      return false;
    }
    value.expected_bytes = bytes;
  }
  return true;
}

void write_profile(Writer& writer, batch_domain::FrozenExecutionProfile const& value) {
  writer.text(value.profile_id);
  writer.text(value.baseline.id);
  writer.text(value.baseline.version);
  writer.u32(static_cast<std::uint32_t>(value.choices.size()));
  for (auto const& choice : value.choices) {
    writer.text(choice.preference_id);
    write_enum(writer, choice.value);
  }
  write_enum(writer, value.executor);
  write_enum(writer, value.execution);
  write_enum(writer, value.completion_boundary);
  write_enum(writer, value.post_install);
  write_enum(writer, value.restart);
  write_enum(writer, value.result_detection);
  write_enum(writer, value.interaction_scope);
  write_enum(writer, value.interaction_disposition);
}

[[nodiscard]] bool read_profile(Reader& reader,
                                batch_domain::FrozenExecutionProfile& value) {
  std::uint32_t count{};
  if (!reader.text(value.profile_id, 256) || !reader.text(value.baseline.id, 256) ||
      !reader.text(value.baseline.version, 256) || !reader.u32(count) || count > 64) {
    return false;
  }
  value.choices.clear();
  value.choices.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    batch_domain::FrozenPreferenceChoice choice;
    if (!reader.text(choice.preference_id, 256) || !read_enum(reader, choice.value)) {
      return false;
    }
    value.choices.push_back(std::move(choice));
  }
  return read_enum(reader, value.executor) && read_enum(reader, value.execution) &&
         read_enum(reader, value.completion_boundary) &&
         read_enum(reader, value.post_install) && read_enum(reader, value.restart) &&
         read_enum(reader, value.result_detection) &&
         read_enum(reader, value.interaction_scope) &&
         read_enum(reader, value.interaction_disposition);
}

void write_item(Writer& writer, batch_domain::FrozenInstallationItem const& value) {
  writer.text(value.item_id);
  writer.u32(static_cast<std::uint32_t>(value.dependencies.size()));
  for (auto const& dependency : value.dependencies) {
    writer.text(dependency);
  }
  write_source(writer, value.source);
  write_package(writer, value.selected_package);
  write_profile(writer, value.execution_profile);
  write_enum(writer, value.resource_kind);
  write_asset(writer, value.cache_asset);
  write_enum(writer, value.cache_root.kind);
  writer.text(value.cache_root.id);
}

[[nodiscard]] bool read_item(Reader& reader,
                             batch_domain::FrozenInstallationItem& value) {
  std::uint32_t count{};
  if (!reader.text(value.item_id, 256) || !reader.u32(count) || count > k_max_items) {
    return false;
  }
  value.dependencies.clear();
  value.dependencies.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string dependency;
    if (!reader.text(dependency, 256)) {
      return false;
    }
    value.dependencies.push_back(std::move(dependency));
  }
  return read_source(reader, value.source) && read_package(reader, value.selected_package) &&
         read_profile(reader, value.execution_profile) &&
         read_enum(reader, value.resource_kind) && read_asset(reader, value.cache_asset) &&
         read_enum(reader, value.cache_root.kind) && reader.text(value.cache_root.id, 256);
}

void write_plan(Writer& writer, batch_domain::FrozenBatchPlan const& value) {
  writer.text(value.batch_id);
  writer.text(value.correlation_id);
  writer.u8(value.retry_of_batch_id.has_value() ? 1 : 0);
  if (value.retry_of_batch_id.has_value()) {
    writer.text(*value.retry_of_batch_id);
  }
  writer.text("[frozen-catalog]");
  writer.text("frozen-catalog");
  writer.text("azzs");
  writer.u32(value.catalog.schema_version);
  writer.u64(value.catalog.revision);
  write_enum(writer, value.catalog.release_state);
  writer.u8(value.catalog.local_trial ? 1 : 0);
  writer.u32(static_cast<std::uint32_t>(value.items.size()));
  for (auto const& item : value.items) {
    write_item(writer, item);
  }
  writer.i64(value.frozen_at_milliseconds);
}

[[nodiscard]] bool read_plan(Reader& reader, batch_domain::FrozenBatchPlan& value) {
  std::uint8_t retry{};
  std::uint8_t local_trial{};
  std::uint32_t count{};
  if (!reader.text(value.batch_id, 256) || !reader.text(value.correlation_id, 256) ||
      !reader.u8(retry) || retry > 1) {
    return false;
  }
  value.retry_of_batch_id.reset();
  if (retry != 0) {
    std::string retry_of;
    if (!reader.text(retry_of, 256)) {
      return false;
    }
    value.retry_of_batch_id = std::move(retry_of);
  }
  if (!reader.text(value.catalog.raw_catalog_bytes, k_max_payload_bytes) ||
      !reader.text(value.catalog.content_identity, 256) ||
      !reader.text(value.catalog.application_id, 256) ||
      !reader.u32(value.catalog.schema_version) || !reader.u64(value.catalog.revision) ||
      !read_enum(reader, value.catalog.release_state) || !reader.u8(local_trial) ||
      local_trial > 1 || !reader.u32(count) || count == 0 || count > k_max_items) {
    return false;
  }
  value.catalog.local_trial = local_trial != 0;
  value.items.clear();
  value.items.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    batch_domain::FrozenInstallationItem item;
    if (!read_item(reader, item)) {
      return false;
    }
    value.items.push_back(std::move(item));
  }
  return reader.i64(value.frozen_at_milliseconds);
}

void write_progress(Writer& writer, batch_domain::InstallationItemProgress const& value) {
  writer.text(value.item_id);
  write_enum(writer, value.state);
  writer.u32(value.attempt);
  writer.u8(value.launch_requested ? 1 : 0);
  writer.u8(value.installer_started ? 1 : 0);
  writer.u8(value.force_termination_confirmation_requested ? 1 : 0);
  writer.u8(value.force_termination_completed ? 1 : 0);
  writer.u8(value.post_install_completed ? 1 : 0);
  writer.u8(0);
  writer.text("");
}

[[nodiscard]] bool read_progress(Reader& reader,
                                 batch_domain::InstallationItemProgress& value) {
  std::uint8_t launch_requested{};
  std::uint8_t started{};
  std::uint8_t force_confirmation{};
  std::uint8_t force_terminated{};
  std::uint8_t post_install_completed{};
  std::uint8_t has_handle{};
  if (!reader.text(value.item_id, 256) || !read_enum(reader, value.state) ||
      !reader.u32(value.attempt) || !reader.u8(launch_requested) || !reader.u8(started) ||
      !reader.u8(force_confirmation) || !reader.u8(force_terminated) ||
      !reader.u8(post_install_completed) || !reader.u8(has_handle) ||
      launch_requested > 1 || started > 1 || force_confirmation > 1 ||
      force_terminated > 1 || post_install_completed > 1 || has_handle > 1) {
    return false;
  }
  value.launch_requested = launch_requested != 0;
  value.installer_started = started != 0;
  value.force_termination_confirmation_requested = force_confirmation != 0;
  value.force_termination_completed = force_terminated != 0;
  value.post_install_completed = post_install_completed != 0;
  value.opaque_installer_handle.reset();
  if (has_handle != 0) {
    std::string handle;
    if (!reader.text(handle, 256)) {
      return false;
    }
    value.opaque_installer_handle = std::move(handle);
  }
  return reader.text(value.detail, 1024);
}

void write_transition(Writer& writer,
                      batch_domain::LastDurableTransition const& value) {
  writer.u64(value.generation);
  writer.text(value.item_id);
  write_enum(writer, value.item_state);
  write_enum(writer, value.outcome);
  writer.u8(value.coverage_gap ? 1 : 0);
}

[[nodiscard]] bool read_transition(Reader& reader,
                                   batch_domain::LastDurableTransition& value) {
  std::uint8_t coverage{};
  return reader.u64(value.generation) && reader.text(value.item_id, 256) &&
         read_enum(reader, value.item_state) && read_enum(reader, value.outcome) &&
         reader.u8(coverage) && coverage <= 1 &&
         ((value.coverage_gap = coverage != 0), true);
}

void write_lease(Writer& writer, batch_domain::DurableLeaseBinding const& value) {
  writer.text(value.kind);
  writer.text(value.operation_id);
  writer.text(value.correlation_id);
  writer.text(value.lease_token_fingerprint);
  writer.u64(value.occupancy_revision);
}

[[nodiscard]] bool read_lease(Reader& reader, batch_domain::DurableLeaseBinding& value) {
  return reader.text(value.kind, 256) && reader.text(value.operation_id, 256) &&
         reader.text(value.correlation_id, 256) &&
         reader.text(value.lease_token_fingerprint, 256) &&
         reader.u64(value.occupancy_revision);
}

void write_record(Writer& writer, batch_domain::InstallationBatchRecord const& value) {
  write_plan(writer, value.plan);
  write_enum(writer, value.state);
  writer.u8(value.close_requested ? 1 : 0);
  writer.u8(value.stop_requested ? 1 : 0);
  writer.u32(static_cast<std::uint32_t>(value.items.size()));
  for (auto const& progress : value.items) {
    write_progress(writer, progress);
  }
  writer.u64(value.generation);
  writer.u8(value.active_lease.has_value() ? 1 : 0);
  if (value.active_lease.has_value()) {
    write_lease(writer, *value.active_lease);
  }
  write_transition(writer, value.last_transition);
}

[[nodiscard]] bool read_record(Reader& reader,
                               batch_domain::InstallationBatchRecord& value) {
  std::uint8_t closing{};
  std::uint8_t stopping{};
  std::uint32_t count{};
  std::uint8_t has_lease{};
  if (!read_plan(reader, value.plan) || !read_enum(reader, value.state) ||
      !reader.u8(closing) || !reader.u8(stopping) || closing > 1 || stopping > 1 ||
      !reader.u32(count) || count > k_max_items) {
    return false;
  }
  value.close_requested = closing != 0;
  value.stop_requested = stopping != 0;
  value.items.clear();
  value.items.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    batch_domain::InstallationItemProgress progress;
    if (!read_progress(reader, progress)) {
      return false;
    }
    value.items.push_back(std::move(progress));
  }
  if (!reader.u64(value.generation) || !reader.u8(has_lease) || has_lease > 1) {
    return false;
  }
  value.active_lease.reset();
  if (has_lease != 0) {
    batch_domain::DurableLeaseBinding lease;
    if (!read_lease(reader, lease)) {
      return false;
    }
    value.active_lease = std::move(lease);
  }
  return read_transition(reader, value.last_transition);
}

void write_history(Writer& writer, batch_domain::InstallationBatchHistory const& value) {
  write_plan(writer, value.plan);
  write_enum(writer, value.final_state);
  writer.u32(static_cast<std::uint32_t>(value.items.size()));
  for (auto const& progress : value.items) {
    write_progress(writer, progress);
  }
  writer.text("batch-history-retained");
}

[[nodiscard]] bool read_history(Reader& reader, batch_domain::InstallationBatchHistory& value) {
  std::uint32_t count{};
  if (!read_plan(reader, value.plan) || !read_enum(reader, value.final_state) ||
      !reader.u32(count) || count > k_max_items) {
    return false;
  }
  value.items.clear();
  value.items.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    batch_domain::InstallationItemProgress progress;
    if (!read_progress(reader, progress)) {
      return false;
    }
    value.items.push_back(std::move(progress));
  }
  return reader.text(value.reason, 1024);
}

struct PersistedState final {
  std::optional<batch_domain::InstallationBatchRecord> active;
  std::vector<batch_domain::InstallationBatchHistory> history;
};

[[nodiscard]] std::optional<domain::StateBytes> encode(PersistedState const& value) {
  Writer writer;
  writer.u32(k_format_version);
  writer.u8(value.active.has_value() ? 1 : 0);
  if (value.active.has_value()) {
    if (!value.active->valid()) {
      return std::nullopt;
    }
    write_record(writer, *value.active);
  }
  if (value.history.size() > k_max_history ||
      !std::ranges::all_of(value.history, &batch_domain::InstallationBatchHistory::valid)) {
    return std::nullopt;
  }
  writer.u32(static_cast<std::uint32_t>(value.history.size()));
  for (auto const& history : value.history) {
    write_history(writer, history);
  }
  auto bytes = std::move(writer).finish();
  return bytes.size() <= k_max_payload_bytes ? std::optional{std::move(bytes)}
                                               : std::nullopt;
}

[[nodiscard]] std::optional<PersistedState> decode(domain::StateBytes const& bytes) {
  if (bytes.size() > k_max_payload_bytes) {
    return std::nullopt;
  }
  Reader reader{bytes};
  std::uint32_t version{};
  std::uint8_t has_active{};
  if (!reader.u32(version) || version != k_format_version ||
      !reader.u8(has_active) || has_active > 1) {
    return std::nullopt;
  }
  PersistedState state;
  if (has_active != 0) {
    batch_domain::InstallationBatchRecord record;
    if (!read_record(reader, record) || !record.valid()) {
      return std::nullopt;
    }
    state.active = std::move(record);
  }
  std::uint32_t history_count{};
  if (!reader.u32(history_count) || history_count > k_max_history) {
    return std::nullopt;
  }
  state.history.reserve(history_count);
  for (std::uint32_t index = 0; index < history_count; ++index) {
    batch_domain::InstallationBatchHistory history;
    if (!read_history(reader, history) || !history.valid()) {
      return std::nullopt;
    }
    state.history.push_back(std::move(history));
  }
  return reader.remaining() == 0 ? std::optional{std::move(state)} : std::nullopt;
}

[[nodiscard]] ExecutionResult execution_result(
    batch_domain::InstallationItemState state) noexcept {
  if (state == batch_domain::InstallationItemState::succeeded ||
      state == batch_domain::InstallationItemState::skipped_installed) {
    return ExecutionResult::succeeded;
  }
  if (state == batch_domain::InstallationItemState::failed ||
      state == batch_domain::InstallationItemState::source_invalid) {
    return ExecutionResult::failed;
  }
  return ExecutionResult::unknown;
}

[[nodiscard]] bool bounded_effect_text(std::string const& value) noexcept {
  return !value.empty() && value.size() <= 256U;
}

void redact_package(selection::ResolvedPackage& value) {
  value.candidate.identity = "redacted-package";
}

void redact_plan(batch_domain::FrozenBatchPlan& value) {
  value.catalog.raw_catalog_bytes = "[redacted]";
  value.catalog.content_identity = "redacted-catalog";
  value.catalog.application_id = "azzs";
  for (auto& item : value.items) {
    item.source.declared_address = "redacted-source";
    item.source.actual_address = "redacted-source";
    item.source.hosting_mechanism = "controlled";
    item.source.branch = "frozen";
    item.source.capability_version = "frozen-resolver";
    for (auto& package : item.source.packages) {
      redact_package(package);
    }
    redact_package(item.selected_package);
  }
}

void redact_progress(batch_domain::InstallationItemProgress& value) {
  value.opaque_installer_handle.reset();
  value.detail.clear();
}

void redact_record(batch_domain::InstallationBatchRecord& value) {
  redact_plan(value.plan);
  for (auto& item : value.items) {
    redact_progress(item);
  }
  value.active_lease.reset();
}

void redact_history(batch_domain::InstallationBatchHistory& value) {
  redact_plan(value.plan);
  for (auto& item : value.items) {
    redact_progress(item);
  }
  value.reason = "batch-history-retained";
}

}  // namespace

bool InstallationEffectTarget::valid() const noexcept {
  return bounded_effect_text(item_id) && cache_asset.valid() && cache_root.valid() &&
         execution_profile.valid() && bounded_effect_text(opaque_item_handle);
}

namespace {

[[nodiscard]] char const* fact_action_name(InstallationFactKind kind) noexcept {
  switch (kind) {
    case InstallationFactKind::batch_created:
      return "batch-created";
    case InstallationFactKind::state_persisted:
      return "state-persisted";
    case InstallationFactKind::launch_requested:
      return "launch-requested";
    case InstallationFactKind::verification_observed:
      return "verification-observed";
    case InstallationFactKind::batch_paused:
      return "batch-paused";
    case InstallationFactKind::download_paused:
      return "download-paused";
    case InstallationFactKind::download_resumed:
      return "download-resumed";
    case InstallationFactKind::normal_stop_requested:
      return "normal-stop-requested";
    case InstallationFactKind::forced_termination_confirmation_requested:
      return "forced-termination-confirmation-requested";
    case InstallationFactKind::forced_termination_confirmation_cancelled:
      return "forced-termination-confirmation-cancelled";
    case InstallationFactKind::forced_termination_observed:
      return "forced-termination-observed";
    case InstallationFactKind::normal_close_requested:
      return "normal-close-requested";
    case InstallationFactKind::recovery_continued:
      return "recovery-continued";
    case InstallationFactKind::recovery_observed:
      return "recovery-observed";
    case InstallationFactKind::coverage_gap:
      return "coverage-gap";
  }
  return "unknown";
}

}  // namespace

class InstallationBatchService::Impl final {
 public:
  Impl(DeviceStateStore& states, SharedOperationOccupancy& occupancy,
       ExecutionLog& log, InstallationDownloadPort& download,
       ControlledInstallerExecutor& executor,
       ControlledProfileReadinessPort& readiness,
       InstallResultVerifier& verifier, InstallationFactSink& facts,
       catalog_app::SoftwareCatalogLifecycle const& catalogs,
       selection_app::SoftwareSelectionLifecycle const& selections,
       restart_resume::RestartResumeService* restart_resume)
      : states_(states), occupancy_(occupancy), log_(log), download_(download),
        executor_(executor), readiness_(readiness), verifier_(verifier), facts_(facts),
        catalogs_(&catalogs), selections_(&selections),
        restart_resume_(restart_resume) {}

  Impl(DeviceStateStore& states, SharedOperationOccupancy& occupancy,
       ExecutionLog& log, InstallationDownloadPort& download,
       ControlledInstallerExecutor& executor,
       ControlledProfileReadinessPort& readiness,
       InstallResultVerifier& verifier, InstallationFactSink& facts,
       FrozenBatchPlanAdmissionPort const& admission,
       restart_resume::RestartResumeService* restart_resume)
      : states_(states), occupancy_(occupancy), log_(log), download_(download),
        executor_(executor), readiness_(readiness), verifier_(verifier), facts_(facts),
        admission_(&admission), restart_resume_(restart_resume) {}

  [[nodiscard]] InstallationBatchActionResult restore() {
    auto const read = states_.inspect(key_);
    if (read.mode == StateReadMode::uninitialized) {
      writable_ = true;
      error_.clear();
      return result(InstallationBatchActionCode::succeeded);
    }
    if ((read.mode != StateReadMode::writable &&
         read.mode != StateReadMode::recovered_previous) || !read.snapshot.has_value()) {
      writable_ = false;
      error_ = read.error.empty() ? "installation batch state is not writable" : read.error;
      return result(InstallationBatchActionCode::read_only);
    }
    auto decoded = decode(read.snapshot->state.value.payload);
    if (!decoded.has_value()) {
      writable_ = false;
      error_ = "installation batch persistence format is unknown or corrupt";
      return result(InstallationBatchActionCode::read_only);
    }
    active_ = std::move(decoded->active);
    history_ = std::move(decoded->history);
    revision_ = read.snapshot->revision;
    writable_ = true;
    error_.clear();
    auto const was_waiting_restart =
        active_.has_value() &&
        active_->state == batch_domain::InstallationBatchState::waiting_restart;
    if (active_.has_value() && active_->state != batch_domain::InstallationBatchState::completed &&
        active_->state != batch_domain::InstallationBatchState::stopped) {
      // A restored batch may contain a launcher effect whose durable receipt
      // was interrupted. It can only take the explicit read-only recovery
      // path; advance() must never repeat that effect.
      active_->state = batch_domain::InstallationBatchState::recovery_required;
      if (was_waiting_restart) active_->close_requested = true;
    }
    // Restore is intentionally local-only. A caller must explicitly request
    // recovery, which uses only the verifier and cannot launch another item.
    return result(InstallationBatchActionCode::succeeded);
  }

  [[nodiscard]] batch_domain::InstallationBatchSnapshot snapshot() const {
    auto active = active_;
    if (active.has_value()) {
      redact_record(*active);
    }
    auto history = history_;
    for (auto& entry : history) {
      redact_history(entry);
    }
    return {.active = std::move(active),
            .history = std::move(history),
            .writable = writable_,
            .error = error_.empty() ? "" : "installation batch requires attention"};
  }

  [[nodiscard]] InstallationBatchActionResult create(batch_domain::FrozenBatchPlan plan) {
    if (!writable_) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_.has_value() && active_->state != batch_domain::InstallationBatchState::completed &&
        active_->state != batch_domain::InstallationBatchState::stopped) {
      return result(InstallationBatchActionCode::blocked, "an active installation batch exists");
    }
    auto validation = validate_new_plan(plan);
    if (!validation.empty()) {
      return result(InstallationBatchActionCode::rejected, std::move(validation));
    }
    batch_domain::InstallationBatchRecord record{
        .plan = std::move(plan),
        .state = batch_domain::InstallationBatchState::ready,
        .items = {},
        .generation = 1,
    };
    for (auto const& item : record.plan.items) {
      record.items.push_back({.item_id = item.item_id});
    }
    record.last_transition = {.generation = record.generation,
                              .item_id = record.items.front().item_id,
                              .item_state = record.items.front().state,
                              .outcome = batch_domain::DurableTransitionOutcome::committed};
    auto previous_active = active_;
    active_ = std::move(record);
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      if (last_lease_acquire_code_ == OccupancyResultCode::occupied) {
        active_ = std::move(previous_active);
        return result(InstallationBatchActionCode::occupied,
                      "another controlled operation currently owns the device");
      }
      return result(InstallationBatchActionCode::persistence_failed);
    }
    return commit_and_release(*lease, InstallationFactKind::batch_created,
                              active_->items.front());
  }

  [[nodiscard]] InstallationBatchActionResult advance() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return result(InstallationBatchActionCode::outcome_unknown,
                    "a prior durable transition is unknown; batch is fail-closed");
    }
    if (active_->close_requested || active_->state == batch_domain::InstallationBatchState::closing ||
        active_->state == batch_domain::InstallationBatchState::recovery_required) {
      return result(InstallationBatchActionCode::recovery_required,
                    "closing or recovery state does not auto-continue");
    }
    auto index = next_runnable_index();
    if (!index.has_value()) {
      active_->state = active_->stop_requested
                           ? batch_domain::InstallationBatchState::stopped
                           : batch_domain::InstallationBatchState::completed;
      auto lease = acquire_and_bind();
      return lease.has_value()
                 ? commit_and_release(*lease, InstallationFactKind::state_persisted,
                                      active_->items.front())
                 : result(InstallationBatchActionCode::persistence_failed);
    }
    auto& progress = active_->items[*index];
    auto const& item = active_->plan.items[*index];
    if (dependency_blocked(*index)) {
      progress.state = batch_domain::InstallationItemState::dependency_blocked;
      progress.detail = "a dependency did not complete successfully";
      active_->state = batch_domain::InstallationBatchState::running;
      auto lease = acquire_and_bind();
      return lease.has_value()
                 ? commit_and_release(*lease, InstallationFactKind::state_persisted, progress)
                 : result(InstallationBatchActionCode::persistence_failed);
    }
    if (progress.state == batch_domain::InstallationItemState::result_confirmation_pending &&
        item.execution_profile.post_install ==
            catalog::PostInstallBehavior::controlled_preferences &&
        progress.installer_started && !progress.post_install_completed) {
      // This repeats only the project-owned completion observation. It never
      // relaunches the installer or bypasses a missing post-install fact.
      progress.state = batch_domain::InstallationItemState::installer_running;
      active_->state = batch_domain::InstallationBatchState::running;
    }
    if (batch_domain::blocks_batch(progress.state)) {
      if (progress.state == batch_domain::InstallationItemState::download_paused) {
        active_->state = batch_domain::InstallationBatchState::download_paused;
      } else if (progress.state == batch_domain::InstallationItemState::waiting_restart) {
        active_->state = batch_domain::InstallationBatchState::waiting_restart;
      } else if (!active_->stop_requested) {
        active_->state = batch_domain::InstallationBatchState::awaiting_user;
      }
      return result(InstallationBatchActionCode::blocked, "the current item requires explicit action");
    }
    if (progress.state == batch_domain::InstallationItemState::installer_running) {
      auto lease = acquire_and_bind();
      if (!lease.has_value()) {
        return lease_failure_result();
      }
      if (active_->stop_requested && !progress.installer_started) {
        progress.state = batch_domain::InstallationItemState::stop_pending;
        progress.detail = "the installer launch was not completed before normal stop";
        return commit_and_release(*lease, InstallationFactKind::normal_stop_requested,
                                  progress);
      }
      if (!progress.installer_started) {
        if (!progress.launch_requested) {
          progress.launch_requested = true;
          auto prepared = commit_and_release(*lease, InstallationFactKind::launch_requested,
                                             progress);
          if (!prepared.succeeded()) {
            return prepared;
          }
        }
        auto observed = executor_.launch({.target = effect_target(*index)});
        switch (observed.code) {
          case InstallerLaunchCode::started:
            progress.installer_started = true;
            progress.opaque_installer_handle = observed.opaque_operation_handle;
            break;
          case InstallerLaunchCode::interaction_required:
            progress.state = batch_domain::InstallationItemState::installer_interaction_pending;
            active_->state = batch_domain::InstallationBatchState::awaiting_user;
            break;
          case InstallerLaunchCode::waiting_network:
            progress.state = batch_domain::InstallationItemState::waiting_network;
            break;
          case InstallerLaunchCode::source_invalid:
            progress.state = batch_domain::InstallationItemState::source_invalid;
            break;
          case InstallerLaunchCode::failed:
            progress.state = batch_domain::InstallationItemState::failed;
            break;
        }
        progress.detail = observed.detail;
        return commit_and_release(*lease, InstallationFactKind::launch_requested, progress);
      }
      auto completion = executor_.observe_completion(
          {.target = effect_target(*index),
           .opaque_operation_handle = progress.opaque_installer_handle});
      switch (completion.code) {
        case InstallerCompletionCode::running:
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::state_persisted, progress);
        case InstallerCompletionCode::interaction_required:
          progress.state = batch_domain::InstallationItemState::installer_interaction_pending;
          active_->state = batch_domain::InstallationBatchState::awaiting_user;
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::batch_paused, progress);
        case InstallerCompletionCode::unknown:
          progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
          active_->state = batch_domain::InstallationBatchState::awaiting_user;
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::batch_paused, progress);
        case InstallerCompletionCode::failed:
          progress.state = batch_domain::InstallationItemState::failed;
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::state_persisted, progress);
        case InstallerCompletionCode::completed:
          break;
      }
      if (!post_install_completion_established(
              progress, item.execution_profile, completion,
              batch_domain::InstallationBatchState::awaiting_user)) {
        return commit_and_release(*lease, InstallationFactKind::batch_paused, progress);
      }
      auto observed = verifier_.verify({.target = effect_target(*index),
                                        .phase = InstallVerificationPhase::after_process_exit,
                                        .opaque_operation_handle = progress.opaque_installer_handle});
      apply_verification(progress, item.execution_profile, observed,
                         InstallVerificationPhase::after_process_exit);
      return commit_and_release(*lease, InstallationFactKind::verification_observed, progress);
    }
    if (progress.state == batch_domain::InstallationItemState::pending) {
      auto lease = acquire_and_bind();
      if (!lease.has_value()) {
        return lease_failure_result();
      }
      auto observed = verifier_.verify({.target = effect_target(*index),
                                        .phase = InstallVerificationPhase::before_launch});
      if (observed.code == InstallVerificationCode::installed) {
        progress.state = batch_domain::InstallationItemState::skipped_installed;
        progress.detail = observed.detail;
      } else if (observed.code != InstallVerificationCode::absent) {
        // An unknown precondition must never become a launch authorization.
        progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
        progress.detail = observed.detail;
        active_->state = batch_domain::InstallationBatchState::awaiting_user;
      } else if (item.resource_kind == batch_domain::FrozenResourceKind::controlled_download) {
        progress.state = batch_domain::InstallationItemState::downloading;
        active_->state = batch_domain::InstallationBatchState::running;
      } else {
        progress.state = batch_domain::InstallationItemState::installer_running;
        active_->state = batch_domain::InstallationBatchState::running;
      }
      return commit_and_release(*lease, InstallationFactKind::verification_observed, progress);
    }
    if (progress.state == batch_domain::InstallationItemState::downloading) {
      auto lease = acquire_and_bind();
      if (!lease.has_value()) {
        return lease_failure_result();
      }
      auto observed = download_.advance(effect_target(*index));
      switch (observed.code) {
        case InstallationDownloadCode::cached_ready:
        case InstallationDownloadCode::completed:
          progress.state = batch_domain::InstallationItemState::installer_running;
          break;
        case InstallationDownloadCode::waiting_network:
          progress.state = batch_domain::InstallationItemState::waiting_network;
          break;
        case InstallationDownloadCode::source_invalid:
          progress.state = batch_domain::InstallationItemState::source_invalid;
          break;
        case InstallationDownloadCode::downloading:
          break;
        case InstallationDownloadCode::paused:
          progress.state = batch_domain::InstallationItemState::download_paused;
          active_->state = batch_domain::InstallationBatchState::download_paused;
          break;
        case InstallationDownloadCode::restart_required:
        case InstallationDownloadCode::stopped:
          progress.state = batch_domain::InstallationItemState::failed;
          break;
        case InstallationDownloadCode::failed:
          progress.state = batch_domain::InstallationItemState::failed;
          break;
      }
      progress.detail = observed.detail;
      return commit_and_release(*lease, InstallationFactKind::state_persisted, progress);
    }
    if (progress.state == batch_domain::InstallationItemState::waiting_network) {
      progress.state = batch_domain::InstallationItemState::pending;
      auto lease = acquire_and_bind();
      return lease.has_value()
                 ? commit_and_release(*lease, InstallationFactKind::state_persisted, progress)
                 : lease_failure_result();
    }
    if (progress.state == batch_domain::InstallationItemState::source_invalid ||
        progress.state == batch_domain::InstallationItemState::failed) {
      return result(InstallationBatchActionCode::blocked, "the current item requires retry or stop");
    }
    return result(InstallationBatchActionCode::blocked);
  }

  [[nodiscard]] InstallationBatchActionResult retry_current(
      batch_domain::FrozenBatchPlan retry_plan) {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = current_retryable_index();
    if (!index.has_value()) {
      return result(InstallationBatchActionCode::rejected, "retry is not allowed in the current state");
    }
    auto const old = *active_;
    auto validation = validate_retry_plan(retry_plan, old, *index);
    if (!validation.empty()) {
      return result(InstallationBatchActionCode::rejected, std::move(validation));
    }
    // The failed attempt remains the authoritative historical record. Close
    // its nonterminal lease before allocating a new operation identity for the
    // derivative batch; retaining that lease across the replacement would make
    // the new batch appear externally occupied by itself.
    auto old_lease = acquire_and_bind();
    if (!old_lease.has_value()) {
      return lease_failure_result();
    }
    active_->state = batch_domain::InstallationBatchState::stopped;
    auto closed = commit_and_release(*old_lease, InstallationFactKind::batch_paused,
                                     active_->items[*index]);
    if (!closed.succeeded()) {
      return closed;
    }
    auto previous_active = active_;
    auto const retry_item_id = retry_plan.items.front().item_id;
    active_ = batch_domain::InstallationBatchRecord{
        .plan = std::move(retry_plan),
        .state = batch_domain::InstallationBatchState::ready,
        .items = {{.item_id = retry_item_id,
                   .attempt = old.items[*index].attempt + 1}},
        .generation = old.generation + 1,
        .last_transition = {.generation = old.generation + 1,
                            .item_id = retry_item_id,
                            .item_state = batch_domain::InstallationItemState::pending,
                            .outcome = batch_domain::DurableTransitionOutcome::committed},
    };
    last_lease_acquire_code_ = OccupancyResultCode::storage_error;
    auto lease = acquire_or_recover_bound_lease();
    if (!lease.has_value()) {
      if (last_lease_acquire_code_ == OccupancyResultCode::occupied) {
        active_ = std::move(previous_active);
        return result(InstallationBatchActionCode::occupied,
                      "another controlled operation currently owns the device");
      }
      return lease_failure_result();
    }
    history_.push_back({.plan = old.plan,
                        .final_state = batch_domain::InstallationBatchState::stopped,
                        .items = old.items,
                        .reason = "created immutable single-item retry batch"});
    return commit_and_release(*lease, InstallationFactKind::batch_created,
                              active_->items.front());
  }

  [[nodiscard]] InstallationBatchActionResult confirm_current_complete() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = current_blocked_index();
    if (!index.has_value() || active_->items[*index].state !=
                                  batch_domain::InstallationItemState::result_confirmation_pending) {
      return result(InstallationBatchActionCode::rejected,
                    "only a result-confirmation-pending item may be user-confirmed");
    }
    auto& progress = active_->items[*index];
    auto const& profile = active_->plan.items[*index].execution_profile;
    if (profile.result_detection !=
        catalog::ResultDetectionStrategy::user_confirmation_only) {
      return result(InstallationBatchActionCode::rejected,
                    "the frozen profile requires project-owned result detection");
    }
    if (profile.post_install == catalog::PostInstallBehavior::controlled_preferences &&
        !progress.post_install_completed) {
      return result(InstallationBatchActionCode::rejected,
                    "the controlled post-install completion fact is still required");
    }
    progress.state = batch_domain::InstallationItemState::succeeded;
    progress.detail = "user explicitly confirmed the installation result";
    active_->state = batch_domain::InstallationBatchState::ready;
    auto lease = acquire_and_bind();
    return lease.has_value()
               ? commit_and_release(*lease, InstallationFactKind::state_persisted, progress)
               : lease_failure_result();
  }

  [[nodiscard]] InstallationBatchActionResult complete_current_installer_interaction() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = current_blocked_index();
    if (!index.has_value() || active_->items[*index].state !=
                                  batch_domain::InstallationItemState::installer_interaction_pending) {
      return result(InstallationBatchActionCode::rejected,
                    "only an installer-interaction-pending item may be completed");
    }
    auto& progress = active_->items[*index];
    // This is not a completion claim. It authorizes a post-interaction
    // observation only; the verifier remains the sole completion authority.
    progress.state = batch_domain::InstallationItemState::installer_running;
    progress.launch_requested = true;
    progress.installer_started = true;
    progress.detail = "user completed the permitted installer interaction";
    active_->state = batch_domain::InstallationBatchState::running;
    auto lease = acquire_and_bind();
    return lease.has_value()
               ? commit_and_release(*lease, InstallationFactKind::state_persisted, progress)
               : lease_failure_result();
  }

  [[nodiscard]] InstallationBatchActionResult pause_current_download() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = next_runnable_index();
    if (!index.has_value() || !batch_domain::command_allowed(
                                  active_->items[*index].state,
                                  batch_domain::InstallationItemCommand::pause_download)) {
      return result(InstallationBatchActionCode::rejected,
                    "pause is only allowed while the current item is downloading");
    }
    auto& progress = active_->items[*index];
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    auto observed = download_.pause(effect_target(*index));
    progress.detail = observed.detail;
    InstallationFactKind kind = InstallationFactKind::state_persisted;
    switch (observed.code) {
      case InstallationDownloadCode::paused:
        progress.state = batch_domain::InstallationItemState::download_paused;
        active_->state = batch_domain::InstallationBatchState::download_paused;
        kind = InstallationFactKind::download_paused;
        break;
      case InstallationDownloadCode::cached_ready:
      case InstallationDownloadCode::completed:
        progress.state = batch_domain::InstallationItemState::installer_running;
        active_->state = batch_domain::InstallationBatchState::running;
        break;
      case InstallationDownloadCode::downloading:
        active_->state = batch_domain::InstallationBatchState::running;
        break;
      case InstallationDownloadCode::waiting_network:
        progress.state = batch_domain::InstallationItemState::waiting_network;
        active_->state = batch_domain::InstallationBatchState::running;
        break;
      case InstallationDownloadCode::source_invalid:
        progress.state = batch_domain::InstallationItemState::source_invalid;
        active_->state = batch_domain::InstallationBatchState::running;
        break;
      case InstallationDownloadCode::restart_required:
      case InstallationDownloadCode::stopped:
      case InstallationDownloadCode::failed:
        break;
    }
    auto persisted = commit_and_release(*lease, kind, progress);
    if (!persisted.succeeded()) {
      return persisted;
    }
    return observed.code == InstallationDownloadCode::paused ||
                   observed.code == InstallationDownloadCode::cached_ready ||
                   observed.code == InstallationDownloadCode::completed
               ? persisted
               : result(InstallationBatchActionCode::rejected,
                        "the controlled download could not be paused");
  }

  [[nodiscard]] InstallationBatchActionResult resume_current_download() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = next_runnable_index();
    if (!index.has_value() || !batch_domain::command_allowed(
                                  active_->items[*index].state,
                                  batch_domain::InstallationItemCommand::resume_download)) {
      return result(InstallationBatchActionCode::rejected,
                    "resume is only allowed for a paused current download");
    }
    auto& progress = active_->items[*index];
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    auto observed = download_.resume(effect_target(*index));
    progress.detail = observed.detail;
    InstallationFactKind kind = InstallationFactKind::state_persisted;
    switch (observed.code) {
      case InstallationDownloadCode::cached_ready:
      case InstallationDownloadCode::completed:
        progress.state = batch_domain::InstallationItemState::installer_running;
        active_->state = batch_domain::InstallationBatchState::running;
        kind = InstallationFactKind::download_resumed;
        break;
      case InstallationDownloadCode::downloading:
      case InstallationDownloadCode::restart_required:
        // A restart-from-zero is a visible recovery outcome, not a normal
        // continuation. The adapter detail tells the UI to say so explicitly.
        progress.state = batch_domain::InstallationItemState::downloading;
        active_->state = batch_domain::InstallationBatchState::running;
        kind = InstallationFactKind::download_resumed;
        break;
      case InstallationDownloadCode::paused:
        progress.state = batch_domain::InstallationItemState::download_paused;
        active_->state = batch_domain::InstallationBatchState::download_paused;
        break;
      case InstallationDownloadCode::waiting_network:
        progress.state = batch_domain::InstallationItemState::waiting_network;
        active_->state = batch_domain::InstallationBatchState::running;
        break;
      case InstallationDownloadCode::source_invalid:
        progress.state = batch_domain::InstallationItemState::source_invalid;
        active_->state = batch_domain::InstallationBatchState::running;
        break;
      case InstallationDownloadCode::stopped:
      case InstallationDownloadCode::failed:
        break;
    }
    auto persisted = commit_and_release(*lease, kind, progress);
    if (!persisted.succeeded()) {
      return persisted;
    }
    return observed.code == InstallationDownloadCode::downloading ||
                   observed.code == InstallationDownloadCode::restart_required ||
                   observed.code == InstallationDownloadCode::cached_ready ||
                   observed.code == InstallationDownloadCode::completed
               ? persisted
               : result(InstallationBatchActionCode::rejected,
                        "the controlled download could not be resumed");
  }

  [[nodiscard]] InstallationBatchActionResult stop_current() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = next_runnable_index();
    if (!index.has_value() || !batch_domain::command_allowed(
                                  active_->items[*index].state,
                                  batch_domain::InstallationItemCommand::stop)) {
      return result(InstallationBatchActionCode::rejected, "stop is not allowed in the current state");
    }
    auto& progress = active_->items[*index];
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    active_->stop_requested = true;
    active_->state = batch_domain::InstallationBatchState::stopping;
    if (progress.state == batch_domain::InstallationItemState::downloading ||
        progress.state == batch_domain::InstallationItemState::download_paused) {
      auto observed = download_.stop(effect_target(*index));
      progress.detail = observed.detail;
      if (observed.code != InstallationDownloadCode::stopped) {
        active_->state = batch_domain::InstallationBatchState::recovery_required;
        auto persisted = commit_and_release(*lease, InstallationFactKind::normal_stop_requested,
                                            progress);
        if (!persisted.succeeded()) {
          return persisted;
        }
        return result(InstallationBatchActionCode::outcome_unknown,
                      "the unfinished controlled download could not be confirmed stopped");
      }
      progress.state = batch_domain::InstallationItemState::stop_pending;
    } else if (progress.state ==
                   batch_domain::InstallationItemState::force_termination_confirmation_pending ||
               progress.state ==
                   batch_domain::InstallationItemState::installer_interaction_pending) {
      // Normal stop never becomes a hidden force-termination. Return to the
      // owned installer observation path and wait for its normal boundary.
      progress.state = batch_domain::InstallationItemState::installer_running;
      progress.force_termination_confirmation_requested = false;
      progress.detail = "normal stop requested; the current installer may finish normally";
    } else if (progress.state != batch_domain::InstallationItemState::installer_running) {
      progress.state = batch_domain::InstallationItemState::stop_pending;
      progress.detail = "the item was not started because the batch was normally stopped";
    }
    return commit_and_release(*lease, InstallationFactKind::normal_stop_requested, progress);
  }

  [[nodiscard]] InstallationBatchActionResult request_force_termination() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = next_runnable_index();
    if (!index.has_value() || !batch_domain::command_allowed(
                                  active_->items[*index].state,
                                  batch_domain::InstallationItemCommand::request_force_termination) ||
        !active_->items[*index].installer_started) {
      return result(InstallationBatchActionCode::rejected,
                    "force termination is only allowed for a started controlled installer");
    }
    auto& progress = active_->items[*index];
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    // Persist the risk acknowledgement boundary before asking the UI to show
    // confirmation. No process effect has happened at this point.
    progress.force_termination_confirmation_requested = true;
    progress.state = batch_domain::InstallationItemState::force_termination_confirmation_pending;
    progress.detail = "force termination requires explicit acknowledgement of half-installed state risk";
    active_->state = batch_domain::InstallationBatchState::awaiting_user;
    auto persisted = commit_and_release(
        *lease, InstallationFactKind::forced_termination_confirmation_requested, progress);
    if (!persisted.succeeded()) {
      return persisted;
    }
    return result(InstallationBatchActionCode::confirmation_required,
                  "force termination requires explicit confirmation");
  }

  [[nodiscard]] InstallationBatchActionResult confirm_force_termination() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = next_runnable_index();
    if (!index.has_value() || !batch_domain::command_allowed(
                                  active_->items[*index].state,
                                  batch_domain::InstallationItemCommand::confirm_force_termination) ||
        !active_->items[*index].force_termination_confirmation_requested) {
      return result(InstallationBatchActionCode::rejected,
                    "force termination confirmation is not pending");
    }
    auto& progress = active_->items[*index];
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    auto observed = executor_.force_terminate(
        {.target = effect_target(*index),
         .opaque_operation_handle = progress.opaque_installer_handle});
    progress.detail = observed.detail;
    switch (observed.code) {
      case InstallerTerminationCode::terminated:
        // A terminated installer is never a completion claim. Keep the
        // preserved completed items and require result verification before any
        // user-confirmed completion or retry path is available.
        progress.force_termination_completed = true;
        progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
        active_->state = batch_domain::InstallationBatchState::awaiting_user;
        return commit_and_release(*lease, InstallationFactKind::forced_termination_observed,
                                  progress);
      case InstallerTerminationCode::unknown:
        // The adapter may have changed the external process before losing its
        // observation. Treat that ambiguity like an unknown installation
        // result and never launch a later item.
        progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
        active_->state = batch_domain::InstallationBatchState::awaiting_user;
        return commit_and_release(*lease, InstallationFactKind::forced_termination_observed,
                                  progress);
      case InstallerTerminationCode::still_running:
      case InstallerTerminationCode::unavailable:
      case InstallerTerminationCode::failed:
        // The production Windows adapter currently takes this path. It must
        // not make a process-result claim or bypass the normal observer.
        progress.state = batch_domain::InstallationItemState::installer_running;
        active_->state = active_->stop_requested
                             ? batch_domain::InstallationBatchState::stopping
                             : batch_domain::InstallationBatchState::running;
        break;
    }
    auto persisted = commit_and_release(*lease, InstallationFactKind::forced_termination_observed,
                                        progress);
    if (!persisted.succeeded()) {
      return persisted;
    }
    return result(InstallationBatchActionCode::rejected,
                  "the controlled installer was not confirmed terminated");
  }

  [[nodiscard]] InstallationBatchActionResult cancel_force_termination() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    auto index = next_runnable_index();
    if (!index.has_value() || !batch_domain::command_allowed(
                                  active_->items[*index].state,
                                  batch_domain::InstallationItemCommand::cancel_force_termination) ||
        !active_->items[*index].force_termination_confirmation_requested) {
      return result(InstallationBatchActionCode::rejected,
                    "force termination confirmation is not pending");
    }
    auto& progress = active_->items[*index];
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    progress.force_termination_confirmation_requested = false;
    progress.state = batch_domain::InstallationItemState::installer_running;
    progress.detail = "force termination confirmation was cancelled; the installer remains under observation";
    active_->state = active_->stop_requested
                         ? batch_domain::InstallationBatchState::stopping
                         : batch_domain::InstallationBatchState::running;
    return commit_and_release(
        *lease, InstallationFactKind::forced_termination_confirmation_cancelled, progress);
  }

  [[nodiscard]] InstallationBatchActionResult request_close() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    active_->close_requested = true;
    active_->state = batch_domain::InstallationBatchState::closing;
    auto index = next_runnable_index();
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    if (index.has_value() &&
        (active_->items[*index].state == batch_domain::InstallationItemState::downloading ||
         active_->items[*index].state ==
             batch_domain::InstallationItemState::download_paused)) {
      auto observed = download_.stop(effect_target(*index));
      active_->items[*index].detail = observed.detail;
      if (observed.code == InstallationDownloadCode::stopped) {
        active_->items[*index].state = batch_domain::InstallationItemState::stop_pending;
      }
      auto persisted = commit_and_release(*lease, InstallationFactKind::normal_close_requested,
                                          active_->items[*index]);
      if (!persisted.succeeded()) {
        return persisted;
      }
      return observed.code == InstallationDownloadCode::stopped
                 ? persisted
                 : result(InstallationBatchActionCode::outcome_unknown,
                          "the unfinished controlled download could not be confirmed stopped");
    }
    return commit_and_release(*lease, InstallationFactKind::normal_close_requested,
                              active_->items.front());
  }

  [[nodiscard]] InstallationBatchActionResult recover_read_only() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    auto index = current_started_or_pending_index();
    if (!index.has_value()) {
      return result(InstallationBatchActionCode::no_active_batch,
                    "no installer requires recovery verification");
    }
    auto& progress = active_->items[*index];
    auto const& profile = active_->plan.items[*index].execution_profile;
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    if (progress.state == batch_domain::InstallationItemState::result_confirmation_pending &&
        profile.post_install == catalog::PostInstallBehavior::controlled_preferences &&
        progress.installer_started && !progress.post_install_completed) {
      progress.state = batch_domain::InstallationItemState::installer_running;
    }
    if (progress.state == batch_domain::InstallationItemState::installer_running) {
      auto completion = executor_.observe_completion(
          {.target = effect_target(*index),
           .opaque_operation_handle = progress.opaque_installer_handle});
      switch (completion.code) {
        case InstallerCompletionCode::running:
          progress.detail = completion.detail;
          active_->state = batch_domain::InstallationBatchState::recovery_required;
          return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
        case InstallerCompletionCode::interaction_required:
          progress.state = batch_domain::InstallationItemState::installer_interaction_pending;
          active_->state = batch_domain::InstallationBatchState::recovery_required;
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
        case InstallerCompletionCode::unknown:
          progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
          active_->state = batch_domain::InstallationBatchState::recovery_required;
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
        case InstallerCompletionCode::failed:
          progress.state = batch_domain::InstallationItemState::failed;
          active_->state = batch_domain::InstallationBatchState::recovery_required;
          progress.detail = completion.detail;
          return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
        case InstallerCompletionCode::completed:
          break;
      }
      if (!post_install_completion_established(
              progress, profile, completion,
              batch_domain::InstallationBatchState::recovery_required)) {
        active_->state = batch_domain::InstallationBatchState::recovery_required;
        return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
      }
    }
    if (progress.state == batch_domain::InstallationItemState::installer_interaction_pending) {
      active_->state = batch_domain::InstallationBatchState::recovery_required;
      return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
    }
    if (progress.state == batch_domain::InstallationItemState::result_confirmation_pending &&
        profile.post_install == catalog::PostInstallBehavior::controlled_preferences &&
        !progress.post_install_completed) {
      active_->state = batch_domain::InstallationBatchState::recovery_required;
      return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
    }
    auto phase = progress.state == batch_domain::InstallationItemState::waiting_restart
                     ? InstallVerificationPhase::after_restart_read_only
                     : InstallVerificationPhase::recovery_read_only;
    auto observed = verifier_.verify({.target = effect_target(*index),
                                      .phase = phase,
                                      .opaque_operation_handle = progress.opaque_installer_handle});
    apply_verification(progress, profile, observed, phase);
    active_->state = batch_domain::InstallationBatchState::recovery_required;
    return commit_and_release(*lease, InstallationFactKind::recovery_observed, progress);
  }

  [[nodiscard]] InstallationBatchActionResult continue_after_recovery() {
    if (!ready_for_command()) {
      return result(InstallationBatchActionCode::not_restored);
    }
    if (active_->state == batch_domain::InstallationBatchState::failed_closed) {
      return fail_closed_result();
    }
    if (!active_->close_requested ||
        active_->state != batch_domain::InstallationBatchState::recovery_required) {
      return result(InstallationBatchActionCode::rejected,
                    "only a recovered closing batch may be explicitly continued");
    }
    auto index = next_runnable_index();
    if (index.has_value()) {
      auto const state = active_->items[*index].state;
      if (state == batch_domain::InstallationItemState::installer_running ||
          batch_domain::blocks_batch(state)) {
        return result(InstallationBatchActionCode::recovery_required,
                      "the recovered installer result still requires verification or user handling");
      }
    }
    auto lease = acquire_and_bind();
    if (!lease.has_value()) {
      return lease_failure_result();
    }
    active_->close_requested = false;
    active_->state = index.has_value() ? batch_domain::InstallationBatchState::ready
                                        : batch_domain::InstallationBatchState::completed;
    auto const& progress = index.has_value() ? active_->items[*index] : active_->items.front();
    return commit_and_release(*lease, InstallationFactKind::recovery_continued, progress);
  }

 private:
  [[nodiscard]] InstallationBatchActionResult result(InstallationBatchActionCode code,
                                                       std::string message = {}) const {
    return {.code = code,
            .snapshot = snapshot(),
            .message = message.empty() ? "" : "installation batch action was not completed"};
  }

  [[nodiscard]] bool ready_for_command() const noexcept {
    return writable_ && active_.has_value();
  }

  [[nodiscard]] InstallationBatchActionResult fail_closed_result() const {
    return result(InstallationBatchActionCode::outcome_unknown,
                  "a prior durable transition is unknown; batch is fail-closed");
  }

  [[nodiscard]] InstallationBatchActionResult lease_failure_result() const {
    return last_lease_acquire_code_ == OccupancyResultCode::occupied
               ? result(InstallationBatchActionCode::occupied,
                        "another controlled operation currently owns the device")
               : result(InstallationBatchActionCode::persistence_failed);
  }

  [[nodiscard]] InstallationEffectTarget effect_target(std::size_t index) const {
    auto const& item = active_->plan.items[index];
    return {.item_id = item.item_id,
            .cache_asset = item.cache_asset,
            .cache_root = item.cache_root,
            .execution_profile = item.execution_profile,
            .opaque_item_handle = opaque_item_handle(item)};
  }

  [[nodiscard]] static std::string opaque_item_handle(
      batch_domain::FrozenInstallationItem const& item) {
    // The adapter receives a deterministic opaque lookup key, not a source
    // address, package object, executable path, or command. Cache identity
    // is already validated at batch admission and remains frozen thereafter.
    return "batch-item:" + item.cache_asset.identity.stable_key();
  }

  [[nodiscard]] std::string validate_new_plan(
      batch_domain::FrozenBatchPlan const& plan) const {
    if (!plan.valid()) {
      return "the frozen batch plan is invalid";
    }
    if (admission_ != nullptr) {
      auto const admitted = admission_->admit(plan);
      if (admitted.code != FrozenBatchPlanAdmissionCode::accepted) {
        return admitted.detail.empty() ? "the frozen batch plan was rejected"
                                       : admitted.detail;
      }
    } else {
      auto const live_validation = validate_live_plan(plan);
      if (!live_validation.empty()) {
        return live_validation;
      }
    }
    for (auto const& item : plan.items) {
      auto const readiness = validate_controlled_item_readiness(item);
      if (!readiness.empty()) {
        return readiness;
      }
    }
    return {};
  }

  [[nodiscard]] std::string validate_controlled_item_readiness(
      batch_domain::FrozenInstallationItem const& item) const {
    if (item.execution_profile.execution !=
        catalog::WindowsExecutionReadiness::project_executor_registered) {
      return "the frozen controlled profile is declaration-only";
    }
    auto const readiness = readiness_.observe(item.execution_profile);
    if (readiness.code != ControlledProfileReadinessCode::registered) {
      return readiness.detail.empty() ? "the controlled executor is not registered"
                                      : readiness.detail;
    }
    return {};
  }

  [[nodiscard]] std::string validate_live_plan(
      batch_domain::FrozenBatchPlan const& plan) const {
    if (catalogs_ == nullptr || selections_ == nullptr) {
      return "the current catalog or controlled selection is unavailable";
    }
    auto const catalog_snapshot = catalogs_->snapshot();
    auto const selection_snapshot = selections_->snapshot();
    if (catalog_snapshot.mode != catalog_app::CatalogLifecycleMode::ready ||
        !catalog_snapshot.current.has_value() ||
        !catalog_snapshot.current_toml_bytes.has_value() ||
        !catalog_snapshot.current_catalog.has_value() ||
        selection_snapshot.mode != selection_app::SelectionLifecycleMode::ready) {
      return "the current catalog or controlled selection is not ready";
    }
    auto const& current = *catalog_snapshot.current;
    if (plan.catalog.raw_catalog_bytes != *catalog_snapshot.current_toml_bytes ||
        plan.catalog.content_identity != current.content_identity ||
        plan.catalog.application_id != current.application_id ||
        plan.catalog.schema_version != catalog_snapshot.current_catalog->schema_version ||
        plan.catalog.revision != current.revision ||
        plan.catalog.release_state != catalog_snapshot.current_catalog->release_state ||
        plan.catalog.local_trial !=
            (current.identity == catalog_app::EffectiveCatalogIdentity::local_trial)) {
      return "the frozen catalog does not match the active catalog";
    }
    for (auto const& item : plan.items) {
      if (std::ranges::find(selection_snapshot.sources, item.source) ==
          selection_snapshot.sources.end()) {
        return "the frozen source was not produced by the controlled selection lifecycle";
      }
      auto const runtime = std::ranges::find(
          catalog_snapshot.current_catalog->software, item.item_id,
          [](catalog::RuntimeSoftware const& software) { return software.definition.id; });
      if (runtime == catalog_snapshot.current_catalog->software.end() ||
          !runtime->definition.install_profile.has_value() ||
          *runtime->definition.install_profile != item.execution_profile.profile_id) {
        return "the frozen profile does not match current catalog software";
      }
      auto const provenance = validate_controlled_item_provenance(item);
      if (!provenance.empty()) {
        return provenance;
      }
    }
    return {};
  }

  [[nodiscard]] static std::string validate_controlled_item_provenance(
      batch_domain::FrozenInstallationItem const& item) {
    auto const profiles = catalog::initial_controlled_install_profiles();
    auto const profile = std::ranges::find(profiles, item.execution_profile.profile_id,
                                           &catalog::ControlledInstallProfile::id);
    if (profile == profiles.end() || profile->software_id != item.item_id ||
        profile->execution_kind != item.execution_profile.executor ||
        profile->execution != item.execution_profile.execution ||
        profile->completion_boundary != item.execution_profile.completion_boundary ||
        profile->post_install_behavior != item.execution_profile.post_install ||
        profile->restart_verification != item.execution_profile.restart ||
        profile->result_detection != item.execution_profile.result_detection ||
        profile->interaction_scope != item.execution_profile.interaction_scope ||
        std::ranges::find(profile->baselines, item.execution_profile.baseline) ==
            profile->baselines.end()) {
      return "the frozen profile is not a registered project-owned profile";
    }
    for (auto const& choice : item.execution_profile.choices) {
      if (std::ranges::find(profile->preferences, choice.preference_id,
                            &catalog::ControlledInstallPreference::id) ==
          profile->preferences.end()) {
        return "a frozen installation choice is not owned by the profile";
      }
    }
    auto const disposition_known = std::ranges::any_of(
        profile->preferences, [&item](catalog::ControlledInstallPreference const& preference) {
          return std::ranges::find(preference.disposition_order,
                                   item.execution_profile.interaction_disposition) !=
                 preference.disposition_order.end();
        });
    if (!disposition_known) {
      return "the frozen interaction disposition is not permitted by the profile";
    }
    auto expected = offline_package_cache::make_cache_asset(item.source, item.selected_package);
    if (!expected.has_value() || *expected != item.cache_asset) {
      return "the frozen cache asset does not match its controlled source and package";
    }
    return {};
  }

  [[nodiscard]] std::optional<std::size_t> next_runnable_index() const noexcept {
    if (!active_.has_value()) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < active_->items.size(); ++index) {
      auto const state = active_->items[index].state;
      if (!batch_domain::is_terminal(state)) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> current_blocked_index() const noexcept {
    auto const current = next_runnable_index();
    return current.has_value() && batch_domain::blocks_batch(active_->items[*current].state)
               ? current
               : std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> current_retryable_index() const noexcept {
    if (!active_.has_value()) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < active_->items.size(); ++index) {
      auto const state = active_->items[index].state;
      if (batch_domain::requires_fresh_retry_snapshot(state) &&
          batch_domain::command_allowed(state,
                                        batch_domain::InstallationItemCommand::retry)) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string validate_retry_plan(
      batch_domain::FrozenBatchPlan const& retry_plan,
      batch_domain::InstallationBatchRecord const& old, std::size_t index) const {
    if (!retry_plan.valid() || retry_plan.items.size() != 1) {
      return "retry requires a valid upstream-frozen single-item plan";
    }
    if (!retry_plan.retry_of_batch_id.has_value() ||
        *retry_plan.retry_of_batch_id != old.plan.batch_id) {
      return "retry plan must identify the immutable failed batch";
    }
    if (retry_plan.batch_id == old.plan.batch_id) {
      return "retry plan must use a distinct batch identity";
    }
    if (retry_plan.items.front().item_id != old.plan.items[index].item_id) {
      return "retry plan must contain the failed item only";
    }
    if (admission_ != nullptr) {
      auto const admitted = admission_->admit_retry(retry_plan);
      if (admitted.code != FrozenBatchPlanAdmissionCode::accepted) {
        return admitted.detail.empty() ? "the frozen retry plan was rejected" : admitted.detail;
      }
    } else {
      auto const provenance = validate_controlled_item_provenance(retry_plan.items.front());
      if (!provenance.empty()) {
        return provenance;
      }
    }
    return validate_controlled_item_readiness(retry_plan.items.front());
  }

  [[nodiscard]] std::optional<std::size_t> current_started_or_pending_index() const noexcept {
    auto const current = next_runnable_index();
    if (!current.has_value()) {
      return std::nullopt;
    }
    auto const state = active_->items[*current].state;
    return state == batch_domain::InstallationItemState::installer_running ||
                   state == batch_domain::InstallationItemState::installer_interaction_pending ||
                   state == batch_domain::InstallationItemState::result_confirmation_pending ||
                   state == batch_domain::InstallationItemState::waiting_restart
               ? current
               : std::nullopt;
  }

  [[nodiscard]] bool dependency_blocked(std::size_t index) const noexcept {
    auto const& item = active_->plan.items[index];
    for (auto const& dependency : item.dependencies) {
      auto const progress = std::ranges::find(active_->items, dependency,
                                              &batch_domain::InstallationItemProgress::item_id);
      if (progress == active_->items.end() ||
          (progress->state != batch_domain::InstallationItemState::succeeded &&
           progress->state != batch_domain::InstallationItemState::skipped_installed)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool post_install_completion_established(
      batch_domain::InstallationItemProgress& progress,
      batch_domain::FrozenExecutionProfile const& profile,
      ControlledInstallerCompletionObservation const& completion,
      batch_domain::InstallationBatchState pending_state) {
    if (profile.post_install == catalog::PostInstallBehavior::none) {
      return true;
    }
    switch (completion.post_install) {
      case PostInstallCompletionCode::completed:
        progress.post_install_completed = true;
        return true;
      case PostInstallCompletionCode::failed:
        progress.state = batch_domain::InstallationItemState::failed;
        progress.detail = completion.detail;
        return false;
      case PostInstallCompletionCode::not_required:
      case PostInstallCompletionCode::pending:
      case PostInstallCompletionCode::unknown:
        progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
        active_->state = pending_state;
        progress.detail = completion.detail;
        return false;
    }
    progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
    active_->state = pending_state;
    progress.detail = completion.detail;
    return false;
  }

  void apply_verification(
      batch_domain::InstallationItemProgress& progress,
      batch_domain::FrozenExecutionProfile const& profile,
      InstallVerificationObservation const& observed,
      InstallVerificationPhase phase) {
    switch (observed.code) {
      case InstallVerificationCode::installed:
        if (profile.completion_boundary ==
                catalog::InstallationCompletionBoundary::
                    post_install_then_restart_verification &&
            profile.restart ==
                catalog::RestartVerification::required_after_restart &&
            phase != InstallVerificationPhase::after_restart_read_only) {
          // The profile, rather than a process outcome, declares the only
          // permitted restart barrier. Before the post-restart observation it
          // cannot become a completion claim.
          progress.state = batch_domain::InstallationItemState::waiting_restart;
          active_->state = batch_domain::InstallationBatchState::waiting_restart;
        } else {
          progress.state = batch_domain::InstallationItemState::succeeded;
        }
        break;
      case InstallVerificationCode::restart_required:
        if (profile.completion_boundary ==
                catalog::InstallationCompletionBoundary::
                    post_install_then_restart_verification &&
            profile.restart == catalog::RestartVerification::required_after_restart) {
          progress.state = batch_domain::InstallationItemState::waiting_restart;
          active_->state = batch_domain::InstallationBatchState::waiting_restart;
        } else {
          progress.state =
              batch_domain::InstallationItemState::result_confirmation_pending;
          active_->state = batch_domain::InstallationBatchState::awaiting_user;
        }
        break;
      case InstallVerificationCode::absent:
      case InstallVerificationCode::failed:
        progress.state = batch_domain::InstallationItemState::failed;
        break;
      case InstallVerificationCode::post_action_pending:
      case InstallVerificationCode::unknown:
        // Even after an observed process exit, absence and unknown facts do not
        // authorize the next item. Only the verifier can establish success.
        progress.state = batch_domain::InstallationItemState::result_confirmation_pending;
        active_->state = batch_domain::InstallationBatchState::awaiting_user;
        break;
    }
    if ((phase == InstallVerificationPhase::recovery_read_only ||
         phase == InstallVerificationPhase::after_restart_read_only) &&
        progress.state == batch_domain::InstallationItemState::succeeded) {
      // Recovery records the known fact but still never auto-starts next work.
      active_->state = batch_domain::InstallationBatchState::recovery_required;
    }
    progress.detail = observed.detail;
  }

  [[nodiscard]] std::optional<OperationLease> acquire_and_bind() {
    if (auto recovered = acquire_or_recover_bound_lease(); recovered.has_value()) {
      return recovered;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<OperationLease> acquire_or_recover_bound_lease() {
    // A persisted active lease is never assumed to be ours after a restart.
    // Observe and release it only if the durable record and occupancy token
    // agree exactly; otherwise do not perform any external effect.
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
      } else if (observed.code != OccupancyResultCode::observed ||
                  observed.current.has_value()) {
        error_ = observed.detail.empty() ? "durable operation lease cannot be reconciled"
                                         : observed.detail;
        mark_outcome_unknown(active_->items.front());
        return std::nullopt;
      }
      error_ = "the durable installation lease disappeared before completion";
      mark_outcome_unknown(active_->items.front());
      return std::nullopt;
    }
    auto acquired = occupancy_.try_acquire({.kind = "installation-batch",
                                            .operation_id = active_->plan.batch_id,
                                            .correlation_id = active_->plan.correlation_id});
    if (acquired.code != OccupancyResultCode::acquired || !acquired.lease.has_value()) {
      last_lease_acquire_code_ = acquired.code;
      if (acquired.code == OccupancyResultCode::occupied) {
        error_ = "another controlled operation currently owns the device";
        return std::nullopt;
      }
      error_ = acquired.detail.empty() ? "installation batch occupancy cannot be acquired"
                                       : acquired.detail;
      mark_outcome_unknown(active_->items.front());
      return std::nullopt;
    }
    auto const& lease = *acquired.lease;
    active_->active_lease = {.kind = lease.identity.kind,
                             .operation_id = lease.identity.operation_id,
                             .correlation_id = lease.identity.correlation_id,
                             .lease_token_fingerprint =
                                 lease_token_fingerprint(lease.lease_token),
                             .occupancy_revision = lease.revision};
    auto persisted = persist();
    if (persisted != PersistOutcome::committed) {
      mark_outcome_unknown(active_->items.front());
      return std::nullopt;
    }
    auto logged = append(InstallationFactKind::state_persisted,
                         active_->items.front());
    if (!logged) {
      mark_outcome_unknown(active_->items.front());
      return std::nullopt;
    }
    return lease;
  }

  [[nodiscard]] InstallationBatchActionResult commit_and_release(
      OperationLease const& lease, InstallationFactKind kind,
      batch_domain::InstallationItemProgress const& progress) {
    finalize_normal_stop_if_terminal(progress);
    active_->generation++;
    active_->last_transition = {.generation = active_->generation,
                                .item_id = progress.item_id,
                                .item_state = progress.state,
                                .outcome = batch_domain::DurableTransitionOutcome::committed};
    auto persisted = persist();
    if (persisted != PersistOutcome::committed) {
      mark_outcome_unknown(progress);
      return result(persisted == PersistOutcome::outcome_unknown
                        ? InstallationBatchActionCode::outcome_unknown
                        : InstallationBatchActionCode::persistence_failed);
    }
    if (!append(kind, progress)) {
      mark_outcome_unknown(progress);
      return result(InstallationBatchActionCode::outcome_unknown);
    }
    facts_.observe({.kind = kind,
                    .batch_id = active_->plan.batch_id,
                    .item_id = progress.item_id,
                    .item_state = progress.state,
                    .result = execution_result(progress.state)});
    if (active_->state == batch_domain::InstallationBatchState::waiting_restart &&
        restart_resume_ != nullptr &&
        restart_resume_->snapshot().state ==
            restart_resume::RestartResumeState::idle) {
      auto checkpoint = restart_resume_->arm({
          .correlation_id = active_->plan.correlation_id,
          .participants = {{
              .operation = restart_resume::RestartResumeOperation::installation_batch,
              .operation_id = active_->plan.batch_id,
          }},
      });
      if (!checkpoint.succeeded()) {
        error_ = checkpoint.message.empty()
                     ? "restart resume registration failed after installation state was persisted"
                     : checkpoint.message;
        return result(InstallationBatchActionCode::blocked, error_);
      }
    }
    if (!releases_occupancy()) {
      return result(InstallationBatchActionCode::succeeded);
    }
    auto released = occupancy_.release(lease);
    if (released.code != OccupancyResultCode::released) {
      error_ = released.detail;
      mark_outcome_unknown(progress);
      return result(InstallationBatchActionCode::outcome_unknown,
                    "operation lease release outcome is unknown");
    }
    active_->active_lease.reset();
    auto const cleared = persist();
    if (cleared != PersistOutcome::committed) {
      active_->active_lease = {.kind = lease.identity.kind,
                               .operation_id = lease.identity.operation_id,
                               .correlation_id = lease.identity.correlation_id,
                               .lease_token_fingerprint =
                                   lease_token_fingerprint(lease.lease_token),
                               .occupancy_revision = lease.revision};
      mark_outcome_unknown(progress);
      return result(cleared == PersistOutcome::outcome_unknown
                        ? InstallationBatchActionCode::outcome_unknown
                        : InstallationBatchActionCode::persistence_failed);
    }
    return result(InstallationBatchActionCode::succeeded);
  }

  [[nodiscard]] bool releases_occupancy() const noexcept {
    return active_->state == batch_domain::InstallationBatchState::completed ||
           active_->state == batch_domain::InstallationBatchState::stopped;
  }

  void finalize_normal_stop_if_terminal(
      batch_domain::InstallationItemProgress const& current) {
    if (!active_->stop_requested || active_->close_requested ||
        !batch_domain::is_terminal(current.state)) {
      return;
    }
    for (auto& progress : active_->items) {
      if (!batch_domain::is_terminal(progress.state)) {
        progress.state = batch_domain::InstallationItemState::stop_pending;
        progress.detail = "not started because the batch was normally stopped";
      }
    }
    active_->state = batch_domain::InstallationBatchState::stopped;
  }

  enum class PersistOutcome { committed, failed, outcome_unknown };

  [[nodiscard]] PersistOutcome persist() {
    auto bytes = encode({.active = active_, .history = history_});
    if (!bytes.has_value()) {
      error_ = "installation batch state exceeds its closed format limits";
      return PersistOutcome::failed;
    }
    domain::DeviceState state{
        .value = {.schema = 2, .minimum_reader = 1, .minimum_writer = 2,
                  .payload = std::move(*bytes)},
    };
    StateCommitResult committed = revision_.has_value()
        ? states_.commit({.key = key_, .expected_revision = *revision_, .state = std::move(state)})
        : states_.initialize(key_, std::move(state));
    if (committed.status == StateCommitStatus::committed && committed.snapshot.has_value()) {
      revision_ = committed.snapshot->revision;
      return PersistOutcome::committed;
    }
    error_ = committed.error;
    return committed.status == StateCommitStatus::outcome_unknown
               ? PersistOutcome::outcome_unknown
               : PersistOutcome::failed;
  }

  [[nodiscard]] bool append(InstallationFactKind kind,
                            batch_domain::InstallationItemProgress const& progress) {
    auto receipt = log_.append({.value = active_->plan.correlation_id},
                               {.kind = ExecutionEventKind::state_transition,
                                .component = "installation-batch-runner",
                                .stage = to_string(progress.state),
                                .result = execution_result(progress.state),
                                .fields = {{.key = "batch",
                                            .value = active_->plan.batch_id,
                                            .disposition = DiagnosticValueDisposition::retain},
                                           {.key = "item",
                                            .value = progress.item_id,
                                            .disposition = DiagnosticValueDisposition::retain},
                                           {.key = "action",
                                            .value = fact_action_name(kind),
                                            .disposition = DiagnosticValueDisposition::retain}}});
    if (!receipt.persisted) {
      error_ = receipt.error;
      return false;
    }
    facts_.observe({.kind = kind,
                    .batch_id = active_->plan.batch_id,
                    .item_id = progress.item_id,
                    .item_state = progress.state,
                    .result = execution_result(progress.state)});
    return true;
  }

  void mark_outcome_unknown(batch_domain::InstallationItemProgress const& progress) {
    auto const item_id = progress.item_id;
    auto const item_state = progress.state;
    active_->state = batch_domain::InstallationBatchState::failed_closed;
    active_->last_transition = {.generation = active_->generation + 1,
                                .item_id = item_id,
                                .item_state = item_state,
                                .outcome = batch_domain::DurableTransitionOutcome::outcome_unknown,
                                .coverage_gap = true};
    active_->generation++;
    static_cast<void>(persist());
    facts_.observe({.kind = InstallationFactKind::coverage_gap,
                    .batch_id = active_->plan.batch_id,
                    .item_id = item_id,
                    .item_state = item_state,
                    .result = ExecutionResult::unknown});
    reconcile_unknown_transition();
  }

  void reconcile_unknown_transition() {
    // A failed or unknown durable write is not resolved from in-memory state.
    // Re-read the authoritative device record and retain the occupancy lease;
    // only recover_read_only() may inspect the current installer afterward.
    auto const read = states_.inspect(key_);
    if ((read.mode != StateReadMode::writable &&
         read.mode != StateReadMode::recovered_previous) || !read.snapshot.has_value()) {
      error_ = "the durable installation outcome cannot be authoritatively reread";
      return;
    }
    auto decoded = decode(read.snapshot->state.value.payload);
    if (!decoded.has_value() || !decoded->active.has_value()) {
      error_ = "the durable installation outcome cannot be reconstructed";
      return;
    }
    active_ = std::move(decoded->active);
    history_ = std::move(decoded->history);
    revision_ = read.snapshot->revision;
    active_->state = batch_domain::InstallationBatchState::recovery_required;
    error_ = "the durable installation outcome requires read-only recovery";
  }

  DeviceStateStore& states_;
  SharedOperationOccupancy& occupancy_;
  ExecutionLog& log_;
  InstallationDownloadPort& download_;
  ControlledInstallerExecutor& executor_;
  ControlledProfileReadinessPort& readiness_;
  InstallResultVerifier& verifier_;
  InstallationFactSink& facts_;
  catalog_app::SoftwareCatalogLifecycle const* catalogs_{nullptr};
  selection_app::SoftwareSelectionLifecycle const* selections_{nullptr};
  FrozenBatchPlanAdmissionPort const* admission_{nullptr};
  restart_resume::RestartResumeService* restart_resume_{nullptr};
  domain::StateKey key_ = domain::StateKey::machine(
      domain::AggregateId{"installation-batch"});
  std::optional<domain::RevisionToken> revision_;
  std::optional<batch_domain::InstallationBatchRecord> active_;
  std::vector<batch_domain::InstallationBatchHistory> history_;
  bool writable_{false};
  std::string error_;
  OccupancyResultCode last_lease_acquire_code_{OccupancyResultCode::storage_error};
};

InstallationBatchService::InstallationBatchService(
    DeviceStateStore& states, SharedOperationOccupancy& occupancy,
    ExecutionLog& log, InstallationDownloadPort& download,
    ControlledInstallerExecutor& executor,
    ControlledProfileReadinessPort& readiness,
    InstallResultVerifier& verifier, InstallationFactSink& facts,
    software_catalog::SoftwareCatalogLifecycle const& catalogs,
    software_selection::SoftwareSelectionLifecycle const& selections,
    restart_resume::RestartResumeService* restart_resume)
    : impl_(std::make_unique<Impl>(states, occupancy, log, download, executor,
                                   readiness, verifier, facts, catalogs, selections,
                                   restart_resume)) {}

InstallationBatchService::InstallationBatchService(
    DeviceStateStore& states, SharedOperationOccupancy& occupancy,
    ExecutionLog& log, InstallationDownloadPort& download,
    ControlledInstallerExecutor& executor,
    ControlledProfileReadinessPort& readiness,
    InstallResultVerifier& verifier, InstallationFactSink& facts,
    FrozenBatchPlanAdmissionPort const& admission,
    restart_resume::RestartResumeService* restart_resume)
    : impl_(std::make_unique<Impl>(states, occupancy, log, download, executor,
                                   readiness, verifier, facts, admission,
                                   restart_resume)) {}

InstallationBatchService::~InstallationBatchService() = default;

InstallationBatchActionResult InstallationBatchService::restore() {
  return impl_->restore();
}

batch_domain::InstallationBatchSnapshot InstallationBatchService::snapshot() const {
  return impl_->snapshot();
}

InstallationBatchActionResult InstallationBatchService::create(
    batch_domain::FrozenBatchPlan plan) {
  return impl_->create(std::move(plan));
}

InstallationBatchActionResult InstallationBatchService::advance() {
  return impl_->advance();
}

InstallationBatchActionResult InstallationBatchService::retry_current(
    batch_domain::FrozenBatchPlan retry_plan) {
  return impl_->retry_current(std::move(retry_plan));
}

InstallationBatchActionResult
InstallationBatchService::complete_current_installer_interaction() {
  return impl_->complete_current_installer_interaction();
}

InstallationBatchActionResult InstallationBatchService::confirm_current_complete() {
  return impl_->confirm_current_complete();
}

InstallationBatchActionResult InstallationBatchService::pause_current_download() {
  return impl_->pause_current_download();
}

InstallationBatchActionResult InstallationBatchService::resume_current_download() {
  return impl_->resume_current_download();
}

InstallationBatchActionResult InstallationBatchService::stop_current() {
  return impl_->stop_current();
}

InstallationBatchActionResult InstallationBatchService::request_force_termination() {
  return impl_->request_force_termination();
}

InstallationBatchActionResult InstallationBatchService::confirm_force_termination() {
  return impl_->confirm_force_termination();
}

InstallationBatchActionResult InstallationBatchService::cancel_force_termination() {
  return impl_->cancel_force_termination();
}

InstallationBatchActionResult InstallationBatchService::request_close() {
  return impl_->request_close();
}

InstallationBatchActionResult InstallationBatchService::recover_read_only() {
  return impl_->recover_read_only();
}

InstallationBatchActionResult InstallationBatchService::continue_after_recovery() {
  return impl_->continue_after_recovery();
}

char const* to_string(InstallationBatchActionCode value) noexcept {
  switch (value) {
    case InstallationBatchActionCode::succeeded:
      return "succeeded";
    case InstallationBatchActionCode::not_restored:
      return "not_restored";
    case InstallationBatchActionCode::rejected:
      return "rejected";
    case InstallationBatchActionCode::occupied:
      return "occupied";
    case InstallationBatchActionCode::read_only:
      return "read_only";
    case InstallationBatchActionCode::persistence_failed:
      return "persistence_failed";
    case InstallationBatchActionCode::outcome_unknown:
      return "outcome_unknown";
    case InstallationBatchActionCode::confirmation_required:
      return "confirmation_required";
    case InstallationBatchActionCode::no_active_batch:
      return "no_active_batch";
    case InstallationBatchActionCode::recovery_required:
      return "recovery_required";
    case InstallationBatchActionCode::blocked:
      return "blocked";
  }
  return "unknown";
}

}  // namespace azzs::application::installation_batch
