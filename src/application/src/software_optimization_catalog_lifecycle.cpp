#include "azzs/application/software_optimization_catalog_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace azzs::application {
namespace {

namespace catalog = domain::software_optimization_catalog;

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'O'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t kPayloadFormat = 1;
constexpr std::size_t kMaximumCatalogBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumIdentityCount = 100'000;
constexpr std::size_t kMaximumFingerprintBytes = 2'048;

[[nodiscard]] domain::StateKey catalog_key() {
  return domain::StateKey::machine(
      domain::AggregateId{"software-optimization-catalog"});
}

class Encoder final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void raw(std::span<std::byte const> value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
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

struct PersistedCatalogState final {
  std::string current_source;
  std::optional<std::string> previous_source;
  std::vector<catalog::StableIdentityRecord> identity_history;
};

[[nodiscard]] domain::StateBytes encode(PersistedCatalogState const& state) {
  Encoder encoder;
  encoder.raw(kMagic);
  encoder.u32(kPayloadFormat);
  encoder.text(state.current_source);
  encoder.u8(state.previous_source.has_value() ? 1U : 0U);
  if (state.previous_source.has_value()) {
    encoder.text(*state.previous_source);
  }
  encoder.u32(static_cast<std::uint32_t>(state.identity_history.size()));
  for (auto const& identity : state.identity_history) {
    encoder.text(identity.id.value);
    encoder.u8(static_cast<std::uint8_t>(identity.kind));
    encoder.text(identity.semantic_fingerprint);
  }
  return encoder.finish();
}

[[nodiscard]] std::optional<PersistedCatalogState> decode(
    domain::StateBytes const& bytes) {
  Decoder decoder{bytes};
  std::array<std::byte, kMagic.size()> magic{};
  std::uint32_t format{};
  PersistedCatalogState state;
  std::uint8_t previous{};
  std::uint32_t identity_count{};
  if (!decoder.raw(magic) || magic != kMagic || !decoder.u32(format) ||
      format != kPayloadFormat ||
      !decoder.text(state.current_source, kMaximumCatalogBytes) ||
      state.current_source.empty() || !decoder.u8(previous) || previous > 1) {
    return std::nullopt;
  }
  if (previous == 1) {
    std::string source;
    if (!decoder.text(source, kMaximumCatalogBytes) || source.empty()) {
      return std::nullopt;
    }
    state.previous_source = std::move(source);
  }
  if (!decoder.u32(identity_count) || identity_count > kMaximumIdentityCount) {
    return std::nullopt;
  }
  state.identity_history.reserve(identity_count);
  for (std::uint32_t index = 0; index < identity_count; ++index) {
    std::string id;
    std::string fingerprint;
    std::uint8_t kind{};
    if (!decoder.text(id, 96) || !decoder.u8(kind) ||
        kind > static_cast<std::uint8_t>(
                   catalog::StableEntityKind::compatibility_baseline) ||
        !decoder.text(fingerprint, kMaximumFingerprintBytes)) {
      return std::nullopt;
    }
    catalog::StableIdentityRecord record{
        .id = catalog::StableId{std::move(id)},
        .kind = static_cast<catalog::StableEntityKind>(kind),
        .semantic_fingerprint = std::move(fingerprint),
    };
    if (!record.id.valid() || record.semantic_fingerprint.empty()) {
      return std::nullopt;
    }
    state.identity_history.push_back(std::move(record));
  }
  if (decoder.remaining() != 0) {
    return std::nullopt;
  }
  return state;
}

struct LoadedCatalogState final {
  SoftwareOptimizationCatalogStateMode mode{
      SoftwareOptimizationCatalogStateMode::failed};
  bool uninitialized{false};
  std::optional<domain::DeviceStateSnapshot> device_snapshot;
  std::optional<PersistedCatalogState> persisted;
  std::optional<catalog::SoftwareOptimizationCatalog> current;
  std::optional<catalog::SoftwareOptimizationCatalog> previous;
  std::string error;
};

[[nodiscard]] bool complete_identity_history(
    catalog::SoftwareOptimizationCatalog const& current,
    std::span<catalog::StableIdentityRecord const> history) {
  std::unordered_set<std::string> unique;
  for (auto const& record : history) {
    if (!unique.insert(record.id.value).second) {
      return false;
    }
  }
  for (auto const& identity : catalog::stable_identities(current)) {
    auto const found = std::ranges::find_if(
        history, [&](catalog::StableIdentityRecord const& record) {
          return record.id == identity.id && record.kind == identity.kind &&
                 record.semantic_fingerprint == identity.semantic_fingerprint;
        });
    if (found == history.end()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] LoadedCatalogState load_state(
    DeviceStateStore& states,
    std::span<catalog::BuiltInRuleDefinition const> built_in_rules) {
  auto read = states.inspect(catalog_key());
  LoadedCatalogState result;
  switch (read.mode) {
    case StateReadMode::uninitialized:
      result.mode = SoftwareOptimizationCatalogStateMode::unavailable;
      result.uninitialized = true;
      return result;
    case StateReadMode::writable:
      result.mode = SoftwareOptimizationCatalogStateMode::available;
      break;
    case StateReadMode::recovered_previous:
      result.mode = SoftwareOptimizationCatalogStateMode::read_only;
      result.error =
          "catalog aggregate recovered N-1 and is read-only until repaired";
      break;
    case StateReadMode::read_only_future:
    case StateReadMode::read_only_corrupt:
      result.mode = SoftwareOptimizationCatalogStateMode::read_only;
      result.error = std::move(read.error);
      return result;
    case StateReadMode::busy:
    case StateReadMode::failed:
      result.mode = SoftwareOptimizationCatalogStateMode::failed;
      result.error = std::move(read.error);
      return result;
  }
  if (!read.snapshot.has_value()) {
    result.mode = SoftwareOptimizationCatalogStateMode::failed;
    result.error = "state store returned no software optimization catalog";
    return result;
  }
  result.device_snapshot = read.snapshot;
  auto decoded = decode(read.snapshot->state.value.payload);
  if (!decoded.has_value()) {
    result.mode = SoftwareOptimizationCatalogStateMode::read_only;
    result.error =
        "software optimization catalog payload is invalid or from an unknown format";
    return result;
  }
  auto current = catalog::load_catalog(decoded->current_source, built_in_rules);
  if (!current.accepted()) {
    result.mode = SoftwareOptimizationCatalogStateMode::read_only;
    result.error = "persisted current software optimization catalog is invalid";
    return result;
  }
  auto history_issues = catalog::validate_stable_identity_history(
      *current.catalog, decoded->identity_history);
  if (!history_issues.empty() ||
      !complete_identity_history(*current.catalog,
                                 decoded->identity_history)) {
    result.mode = SoftwareOptimizationCatalogStateMode::read_only;
    result.error = "persisted stable identity history does not match current catalog";
    return result;
  }
  result.current = std::move(current.catalog);
  if (decoded->previous_source.has_value()) {
    auto previous =
        catalog::load_catalog(*decoded->previous_source, built_in_rules);
    if (!previous.accepted()) {
      result.mode = SoftwareOptimizationCatalogStateMode::read_only;
      result.error = "persisted previous software optimization catalog is invalid";
      return result;
    }
    auto previous_history_issues = catalog::validate_stable_identity_history(
        *previous.catalog, decoded->identity_history);
    if (!previous_history_issues.empty()) {
      result.mode = SoftwareOptimizationCatalogStateMode::read_only;
      result.error =
          "persisted stable identity history does not match previous catalog";
      return result;
    }
    result.previous = std::move(previous.catalog);
  }
  result.persisted = std::move(decoded);
  return result;
}

[[nodiscard]] std::string fingerprint(std::string_view source) {
  std::uint64_t value = 14695981039346656037ULL;
  for (auto const character : source) {
    value ^= static_cast<unsigned char>(character);
    value *= 1099511628211ULL;
  }
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

[[nodiscard]] ExecutionLogReceipt append_event(
    ExecutionLog& log, CorrelationId const& correlation, ExecutionEventKind kind,
    std::string stage, ExecutionResult result, std::string detail = {},
    std::string source_path = {}, std::optional<std::uint64_t> revision = {}) {
  ExecutionEvent event{
      .kind = kind,
      .component = "software-optimization-catalog",
      .stage = std::move(stage),
      .result = result,
  };
  if (!detail.empty()) {
    event.error = ExecutionError{
        .source = "software-optimization-catalog",
        .message = std::move(detail),
    };
  }
  if (!source_path.empty()) {
    event.fields.push_back(DiagnosticField{
        .key = "candidate_path",
        .value = std::move(source_path),
        .disposition = DiagnosticValueDisposition::sensitive,
    });
  }
  if (revision.has_value()) {
    event.fields.push_back(DiagnosticField{
        .key = "catalog_revision",
        .value = std::to_string(*revision),
        .disposition = DiagnosticValueDisposition::retain,
    });
    event.last_trusted_state = LastTrustedState{
        .generation = *revision,
        .summary = "software optimization catalog revision",
    };
  }
  return log.append(correlation, event);
}

[[nodiscard]] SoftwareOptimizationCatalogLifecycleResult rejected_result(
    SoftwareOptimizationCatalogLifecycleCode code, std::string error,
    std::vector<catalog::CatalogIssue> issues = {}) {
  return {
      .code = code,
      .issues = std::move(issues),
      .error = std::move(error),
  };
}

[[nodiscard]] SoftwareOptimizationCatalogLifecycleCode occupancy_code(
    OccupancyResultCode code) noexcept {
  switch (code) {
    case OccupancyResultCode::occupied:
      return SoftwareOptimizationCatalogLifecycleCode::occupied;
    case OccupancyResultCode::read_only:
      return SoftwareOptimizationCatalogLifecycleCode::read_only;
    case OccupancyResultCode::invalid_request:
    case OccupancyResultCode::observed:
    case OccupancyResultCode::acquired:
    case OccupancyResultCode::released:
    case OccupancyResultCode::stale_lease:
    case OccupancyResultCode::conflict:
    case OccupancyResultCode::storage_error:
      return SoftwareOptimizationCatalogLifecycleCode::persistence_failed;
  }
  return SoftwareOptimizationCatalogLifecycleCode::persistence_failed;
}

[[nodiscard]] std::string commit_error(StateCommitResult const& committed) {
  if (!committed.error.empty()) {
    return committed.error;
  }
  if (!committed.failed_stage.empty()) {
    return "state commit failed at " + committed.failed_stage;
  }
  return "state commit did not complete";
}

}  // namespace

SoftwareOptimizationCatalogLifecycle::SoftwareOptimizationCatalogLifecycle(
    DeviceStateStore& states, ExecutionLog& log,
    SharedOperationOccupancy& occupancy,
    SoftwareOptimizationCatalogFile& files,
    std::span<catalog::BuiltInRuleDefinition const> built_in_rules) noexcept
    : states_(states),
      log_(log),
      occupancy_(occupancy),
      files_(files),
      built_in_rules_(built_in_rules.begin(), built_in_rules.end()) {}

SoftwareOptimizationCatalogSnapshot
SoftwareOptimizationCatalogLifecycle::snapshot() {
  auto loaded = load_state(states_, built_in_rules_);
  return {
      .mode = loaded.mode,
      .current = std::move(loaded.current),
      .previous_available = loaded.previous.has_value(),
      .identity_history = loaded.persisted.has_value()
                              ? std::move(loaded.persisted->identity_history)
                              : std::vector<catalog::StableIdentityRecord>{},
      .error = std::move(loaded.error),
  };
}

SoftwareOptimizationCatalogLifecycleResult
SoftwareOptimizationCatalogLifecycle::ensure_builtin(
    std::string_view source, std::string operation_id) {
  return apply_source(std::string{source}, CandidateKind::built_in,
                      std::move(operation_id), false, "builtin");
}

SoftwareOptimizationCatalogLifecycleResult
SoftwareOptimizationCatalogLifecycle::apply_update(
    std::string_view path, std::string operation_id) {
  auto const read = files_.read(path);
  if (!read.succeeded) {
    auto correlation = log_.begin_correlation();
    auto receipt = append_event(log_, correlation, ExecutionEventKind::adapter_result,
                                "read-update", ExecutionResult::failed,
                                read.error, std::string{path});
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::file_failed, read.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  return apply_source(read.source, CandidateKind::update,
                      std::move(operation_id), false, std::string{path});
}

SoftwareOptimizationCatalogImportPreview
SoftwareOptimizationCatalogLifecycle::preview_manual_import(
    std::string_view path, bool debug_mode) {
  auto correlation = log_.begin_correlation();
  if (!debug_mode) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::user_command,
        "preview-manual-import", ExecutionResult::failed,
        "manual import is available only in debug mode", std::string{path});
    return {
        .code = receipt.persisted
                    ? SoftwareOptimizationCatalogLifecycleCode::debug_mode_required
                    : SoftwareOptimizationCatalogLifecycleCode::logging_failed,
        .path = std::string{path},
        .error = receipt.persisted
                     ? "manual import is available only in debug mode"
                     : receipt.error,
    };
  }
  auto read = files_.read(path);
  if (!read.succeeded) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::adapter_result,
        "preview-manual-import", ExecutionResult::failed, read.error,
        std::string{path});
    return {
        .code = receipt.persisted
                    ? SoftwareOptimizationCatalogLifecycleCode::file_failed
                    : SoftwareOptimizationCatalogLifecycleCode::logging_failed,
        .path = std::string{path},
        .error = receipt.persisted ? read.error : receipt.error,
    };
  }
  auto candidate = catalog::load_catalog(read.source, built_in_rules_);
  if (!candidate.accepted()) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        "preview-manual-import", ExecutionResult::failed,
        "candidate catalog was rejected", std::string{path});
    return {
        .code = receipt.persisted
                    ? SoftwareOptimizationCatalogLifecycleCode::rejected
                    : SoftwareOptimizationCatalogLifecycleCode::logging_failed,
        .path = std::string{path},
        .preview_token = fingerprint(read.source),
        .issues = std::move(candidate.package_issues),
        .error = receipt.persisted ? "candidate catalog was rejected"
                                   : receipt.error,
    };
  }
  auto loaded = load_state(states_, built_in_rules_);
  if (loaded.mode == SoftwareOptimizationCatalogStateMode::read_only ||
      loaded.mode == SoftwareOptimizationCatalogStateMode::failed) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        "preview-manual-import", ExecutionResult::failed, loaded.error,
        std::string{path}, candidate.catalog->revision);
    return {
        .code = receipt.persisted
                    ? (loaded.mode ==
                               SoftwareOptimizationCatalogStateMode::read_only
                           ? SoftwareOptimizationCatalogLifecycleCode::read_only
                           : SoftwareOptimizationCatalogLifecycleCode::persistence_failed)
                    : SoftwareOptimizationCatalogLifecycleCode::logging_failed,
        .path = std::string{path},
        .preview_token = fingerprint(read.source),
        .candidate = catalog::summarize(*candidate.catalog),
        .issues = candidate.catalog->release_issues,
        .error = receipt.persisted ? loaded.error : receipt.error,
    };
  }
  if (loaded.persisted.has_value()) {
    auto identity_issues = catalog::validate_stable_identity_history(
        *candidate.catalog, loaded.persisted->identity_history);
    if (!identity_issues.empty()) {
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition,
          "preview-manual-import", ExecutionResult::failed,
          "candidate reuses a stable identity", std::string{path},
          candidate.catalog->revision);
      return {
          .code = receipt.persisted
                      ? SoftwareOptimizationCatalogLifecycleCode::rejected
                      : SoftwareOptimizationCatalogLifecycleCode::logging_failed,
          .path = std::string{path},
          .preview_token = fingerprint(read.source),
          .candidate = catalog::summarize(*candidate.catalog),
          .issues = std::move(identity_issues),
          .error = receipt.persisted ? "candidate reuses a stable identity"
                                     : receipt.error,
      };
    }
  }
  auto preview = SoftwareOptimizationCatalogImportPreview{
      .code = SoftwareOptimizationCatalogLifecycleCode::preview_ready,
      .path = std::string{path},
      .preview_token = fingerprint(read.source),
      .candidate = catalog::summarize(*candidate.catalog),
      .issues = candidate.catalog->release_issues,
  };
  if (loaded.current.has_value()) {
    preview.downgrade =
        candidate.catalog->revision < loaded.current->revision;
    if (preview.downgrade) {
      preview.lost_or_changed_schemes = catalog::schemes_lost_or_changed(
          *loaded.current, *candidate.catalog);
    }
  }
  auto receipt = append_event(
      log_, correlation, ExecutionEventKind::state_transition,
      "preview-manual-import", ExecutionResult::succeeded, {},
      std::string{path}, candidate.catalog->revision);
  if (!receipt.persisted) {
    preview.code = SoftwareOptimizationCatalogLifecycleCode::logging_failed;
    preview.error = std::move(receipt.error);
  }
  return preview;
}

SoftwareOptimizationCatalogLifecycleResult
SoftwareOptimizationCatalogLifecycle::apply_manual_import(
    std::string_view path, std::string_view expected_preview_token,
    bool confirmed, bool debug_mode, std::string operation_id) {
  if (!debug_mode || !confirmed) {
    auto correlation = log_.begin_correlation();
    auto const detail = !debug_mode
                            ? "manual import is available only in debug mode"
                            : "manual import requires explicit confirmation";
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::user_command,
        "apply-manual-import", ExecutionResult::failed, detail,
        std::string{path});
    auto result = rejected_result(
        !debug_mode
            ? SoftwareOptimizationCatalogLifecycleCode::debug_mode_required
            : SoftwareOptimizationCatalogLifecycleCode::confirmation_required,
        detail);
    if (!receipt.persisted) {
      result.code = SoftwareOptimizationCatalogLifecycleCode::logging_failed;
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  auto read = files_.read(path);
  if (!read.succeeded) {
    auto correlation = log_.begin_correlation();
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::adapter_result,
        "apply-manual-import", ExecutionResult::failed, read.error,
        std::string{path});
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::file_failed, read.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  if (expected_preview_token.empty() ||
      fingerprint(read.source) != expected_preview_token) {
    auto correlation = log_.begin_correlation();
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        "apply-manual-import", ExecutionResult::failed,
        "candidate changed after preview", std::string{path});
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::preview_stale,
        "candidate changed after preview");
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  return apply_source(std::move(read.source), CandidateKind::manual_import,
                      std::move(operation_id), true, std::string{path});
}

SoftwareOptimizationCatalogLifecycleResult
SoftwareOptimizationCatalogLifecycle::apply_source(
    std::string source, CandidateKind kind, std::string operation_id,
    bool allow_downgrade, std::string source_path) {
  auto const stage = [kind]() -> char const* {
    switch (kind) {
      case CandidateKind::built_in:
        return "load-builtin";
      case CandidateKind::update:
        return "apply-update";
      case CandidateKind::manual_import:
        return "apply-manual-import";
    }
    return "apply-candidate";
  }();
  auto correlation = log_.begin_correlation();
  auto started = append_event(log_, correlation, ExecutionEventKind::user_command,
                              stage, ExecutionResult::started, {},
                              source_path);
  if (!started.persisted) {
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::logging_failed,
        "catalog operation did not start because its log event was not persisted");
    result.logging_error = std::move(started.error);
    return result;
  }

  if (source.empty() || source.size() > kMaximumCatalogBytes) {
    auto const detail = "candidate catalog size is outside the supported range";
    auto receipt = append_event(log_, correlation,
                                ExecutionEventKind::state_transition,
                                stage, ExecutionResult::failed,
                                detail, source_path);
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::rejected, detail);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  auto candidate = catalog::load_catalog(source, built_in_rules_);
  if (!candidate.accepted()) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::failed,
        "candidate catalog was rejected", source_path);
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::rejected,
        "candidate catalog was rejected", std::move(candidate.package_issues));
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }

  auto initial = load_state(states_, built_in_rules_);
  if (initial.mode == SoftwareOptimizationCatalogStateMode::read_only ||
      initial.mode == SoftwareOptimizationCatalogStateMode::failed) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::failed, initial.error, source_path,
        candidate.catalog->revision);
    auto result = rejected_result(
        initial.mode == SoftwareOptimizationCatalogStateMode::read_only
            ? SoftwareOptimizationCatalogLifecycleCode::read_only
            : SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        initial.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  if (kind == CandidateKind::built_in && initial.current.has_value()) {
    auto result = SoftwareOptimizationCatalogLifecycleResult{
        .code = SoftwareOptimizationCatalogLifecycleCode::unchanged,
        .active = catalog::summarize(*initial.current),
    };
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::succeeded,
        "an active catalog already exists; builtin did not replace it",
        source_path, initial.current->revision);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  if (initial.persisted.has_value()) {
    auto identity_issues = catalog::validate_stable_identity_history(
        *candidate.catalog, initial.persisted->identity_history);
    if (!identity_issues.empty()) {
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition,
          stage, ExecutionResult::failed,
          "candidate reuses a stable identity", source_path,
          candidate.catalog->revision);
      auto result = rejected_result(
          SoftwareOptimizationCatalogLifecycleCode::rejected,
          "candidate reuses a stable identity", std::move(identity_issues));
      if (!receipt.persisted) {
        result.logging_error = std::move(receipt.error);
      }
      return result;
    }
  }
  if (initial.current.has_value()) {
    if (candidate.catalog->revision == initial.current->revision) {
      auto result = SoftwareOptimizationCatalogLifecycleResult{
          .code = SoftwareOptimizationCatalogLifecycleCode::unchanged,
          .active = catalog::summarize(*initial.current),
      };
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition,
          stage, ExecutionResult::succeeded,
          "candidate revision is already active", source_path,
          initial.current->revision);
      if (!receipt.persisted) {
        result.logging_error = std::move(receipt.error);
      }
      return result;
    }
    if (!allow_downgrade &&
        candidate.catalog->revision < initial.current->revision) {
      auto const detail =
          "catalog update cannot downgrade; use debug import or rollback";
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition,
          stage, ExecutionResult::failed, detail, source_path,
          candidate.catalog->revision);
      auto result = rejected_result(
          SoftwareOptimizationCatalogLifecycleCode::rejected, detail);
      if (!receipt.persisted) {
        result.logging_error = std::move(receipt.error);
      }
      return result;
    }
  }

  auto occupied = occupancy_.try_acquire(OperationIdentity{
      .kind = "software-optimization-catalog",
      .operation_id = std::move(operation_id),
      .correlation_id = correlation.value,
  });
  if (occupied.code != OccupancyResultCode::acquired ||
      !occupied.lease.has_value()) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::failed,
        occupied.detail.empty() ? "device operation occupancy is unavailable"
                                : occupied.detail,
        source_path, candidate.catalog->revision);
    auto result = rejected_result(
        occupancy_code(occupied.code),
        occupied.detail.empty() ? "device operation occupancy is unavailable"
                                : occupied.detail);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  auto const lease = *occupied.lease;
  auto finish = [&](SoftwareOptimizationCatalogLifecycleResult result) {
    auto released = occupancy_.release(lease);
    if (released.code != OccupancyResultCode::released) {
      result.occupancy_error = released.detail.empty()
                                   ? "catalog occupancy lease was not released"
                                   : std::move(released.detail);
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::adapter_result,
          "release-occupancy", ExecutionResult::failed,
          result.occupancy_error, source_path,
          result.active.has_value()
              ? std::optional<std::uint64_t>{result.active->revision}
              : std::nullopt);
      if (!receipt.persisted && result.logging_error.empty()) {
        result.logging_error = std::move(receipt.error);
      }
    }
    return result;
  };

  auto loaded = load_state(states_, built_in_rules_);
  if (loaded.mode == SoftwareOptimizationCatalogStateMode::read_only ||
      loaded.mode == SoftwareOptimizationCatalogStateMode::failed) {
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::failed, loaded.error, source_path,
        candidate.catalog->revision);
    auto result = rejected_result(
        loaded.mode == SoftwareOptimizationCatalogStateMode::read_only
            ? SoftwareOptimizationCatalogLifecycleCode::read_only
            : SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        loaded.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }
  if (kind == CandidateKind::built_in && loaded.current.has_value()) {
    auto result = SoftwareOptimizationCatalogLifecycleResult{
        .code = SoftwareOptimizationCatalogLifecycleCode::unchanged,
        .active = catalog::summarize(*loaded.current),
    };
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::succeeded,
        "another instance established the active catalog", source_path,
        loaded.current->revision);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }
  if (loaded.persisted.has_value()) {
    auto identity_issues = catalog::validate_stable_identity_history(
        *candidate.catalog, loaded.persisted->identity_history);
    if (!identity_issues.empty()) {
      auto result = rejected_result(
          SoftwareOptimizationCatalogLifecycleCode::rejected,
          "candidate reuses a stable identity", std::move(identity_issues));
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition,
          stage, ExecutionResult::failed, result.error,
          source_path, candidate.catalog->revision);
      if (!receipt.persisted) {
        result.logging_error = std::move(receipt.error);
      }
      return finish(std::move(result));
    }
  }
  if (loaded.current.has_value()) {
    if (candidate.catalog->revision == loaded.current->revision) {
      auto result = SoftwareOptimizationCatalogLifecycleResult{
          .code = SoftwareOptimizationCatalogLifecycleCode::unchanged,
          .active = catalog::summarize(*loaded.current),
      };
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition, stage,
          ExecutionResult::succeeded,
          "candidate revision became active while waiting for occupancy",
          source_path, loaded.current->revision);
      if (!receipt.persisted) {
        result.logging_error = std::move(receipt.error);
      }
      return finish(std::move(result));
    }
    if (!allow_downgrade &&
        candidate.catalog->revision < loaded.current->revision) {
      auto result = rejected_result(
          SoftwareOptimizationCatalogLifecycleCode::rejected,
          "catalog revision changed and update would now downgrade");
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::state_transition, stage,
          ExecutionResult::failed, result.error, source_path,
          candidate.catalog->revision);
      if (!receipt.persisted) {
        result.logging_error = std::move(receipt.error);
      }
      return finish(std::move(result));
    }
  }

  PersistedCatalogState desired{
      .current_source = std::move(source),
      .previous_source = loaded.persisted.has_value()
                             ? std::optional<std::string>{
                                   loaded.persisted->current_source}
                             : std::nullopt,
      .identity_history = loaded.persisted.has_value()
                              ? catalog::merge_stable_identity_history(
                                    loaded.persisted->identity_history,
                                    *candidate.catalog)
                              : catalog::stable_identities(*candidate.catalog),
  };
  domain::DeviceState state{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode(desired)},
  };
  StateCommitResult committed;
  if (loaded.uninitialized) {
    committed = states_.initialize(catalog_key(), std::move(state));
  } else if (loaded.device_snapshot.has_value()) {
    committed = states_.commit(StateCommitRequest{
        .key = catalog_key(),
        .expected_revision = loaded.device_snapshot->revision,
        .state = std::move(state),
    });
  } else {
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        "catalog state snapshot disappeared before commit");
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition, stage,
        ExecutionResult::failed, result.error, source_path,
        candidate.catalog->revision);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }
  if (committed.status != StateCommitStatus::committed) {
    auto detail = commit_error(committed);
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition,
        stage, ExecutionResult::failed, detail, source_path,
        candidate.catalog->revision);
    auto result = rejected_result(
        committed.status == StateCommitStatus::read_only
            ? SoftwareOptimizationCatalogLifecycleCode::read_only
            : SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        std::move(detail));
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }

  auto const downgraded =
      loaded.current.has_value() &&
      candidate.catalog->revision < loaded.current->revision;
  auto result = SoftwareOptimizationCatalogLifecycleResult{
      .code = downgraded
                  ? SoftwareOptimizationCatalogLifecycleCode::downgraded
                  : SoftwareOptimizationCatalogLifecycleCode::applied,
      .state_changed = true,
      .active = catalog::summarize(*candidate.catalog),
      .issues = candidate.catalog->release_issues,
  };
  auto receipt = append_event(
      log_, correlation, ExecutionEventKind::state_transition,
      stage, ExecutionResult::succeeded, {}, source_path,
      candidate.catalog->revision);
  if (!receipt.persisted) {
    result.logging_error = std::move(receipt.error);
  }
  return finish(std::move(result));
}

SoftwareOptimizationCatalogLifecycleResult
SoftwareOptimizationCatalogLifecycle::rollback(std::string operation_id) {
  auto correlation = log_.begin_correlation();
  auto started = append_event(log_, correlation, ExecutionEventKind::user_command,
                              "rollback", ExecutionResult::started);
  if (!started.persisted) {
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::logging_failed,
        "catalog rollback did not start because its log event was not persisted");
    result.logging_error = std::move(started.error);
    return result;
  }
  auto occupied = occupancy_.try_acquire(OperationIdentity{
      .kind = "software-optimization-catalog",
      .operation_id = std::move(operation_id),
      .correlation_id = correlation.value,
  });
  if (occupied.code != OccupancyResultCode::acquired ||
      !occupied.lease.has_value()) {
    auto result = rejected_result(
        occupancy_code(occupied.code),
        occupied.detail.empty() ? "device operation occupancy is unavailable"
                                : occupied.detail);
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition, "rollback",
        ExecutionResult::failed, result.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return result;
  }
  auto const lease = *occupied.lease;
  auto finish = [&](SoftwareOptimizationCatalogLifecycleResult result) {
    auto released = occupancy_.release(lease);
    if (released.code != OccupancyResultCode::released) {
      result.occupancy_error = released.detail.empty()
                                   ? "catalog occupancy lease was not released"
                                   : std::move(released.detail);
      auto receipt = append_event(
          log_, correlation, ExecutionEventKind::adapter_result,
          "release-occupancy", ExecutionResult::failed,
          result.occupancy_error, {},
          result.active.has_value()
              ? std::optional<std::uint64_t>{result.active->revision}
              : std::nullopt);
      if (!receipt.persisted && result.logging_error.empty()) {
        result.logging_error = std::move(receipt.error);
      }
    }
    return result;
  };
  auto loaded = load_state(states_, built_in_rules_);
  if (loaded.mode == SoftwareOptimizationCatalogStateMode::read_only ||
      loaded.mode == SoftwareOptimizationCatalogStateMode::failed) {
    auto result = rejected_result(
        loaded.mode == SoftwareOptimizationCatalogStateMode::read_only
            ? SoftwareOptimizationCatalogLifecycleCode::read_only
            : SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        loaded.error);
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition, "rollback",
        ExecutionResult::failed, result.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }
  if (!loaded.persisted.has_value() || !loaded.previous.has_value() ||
      !loaded.persisted->previous_source.has_value()) {
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::no_previous,
        "no previous valid software optimization catalog is available");
    auto receipt = append_event(log_, correlation,
                                ExecutionEventKind::state_transition,
                                "rollback", ExecutionResult::failed,
                                result.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }

  PersistedCatalogState desired{
      .current_source = *loaded.persisted->previous_source,
      .previous_source = loaded.persisted->current_source,
      .identity_history = loaded.persisted->identity_history,
  };
  if (!loaded.device_snapshot.has_value()) {
    auto result = rejected_result(
        SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        "catalog state snapshot disappeared before rollback");
    auto receipt = append_event(
        log_, correlation, ExecutionEventKind::state_transition, "rollback",
        ExecutionResult::failed, result.error);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }
  auto committed = states_.commit(StateCommitRequest{
      .key = catalog_key(),
      .expected_revision = loaded.device_snapshot->revision,
      .state = domain::DeviceState{
          .value = {.schema = 2,
                    .minimum_reader = 1,
                    .minimum_writer = 2,
                    .payload = encode(desired)},
      },
  });
  if (committed.status != StateCommitStatus::committed) {
    auto detail = commit_error(committed);
    auto result = rejected_result(
        committed.status == StateCommitStatus::read_only
            ? SoftwareOptimizationCatalogLifecycleCode::read_only
            : SoftwareOptimizationCatalogLifecycleCode::persistence_failed,
        detail);
    auto receipt = append_event(log_, correlation,
                                ExecutionEventKind::state_transition,
                                "rollback", ExecutionResult::failed,
                                detail);
    if (!receipt.persisted) {
      result.logging_error = std::move(receipt.error);
    }
    return finish(std::move(result));
  }
  auto result = SoftwareOptimizationCatalogLifecycleResult{
      .code = SoftwareOptimizationCatalogLifecycleCode::rolled_back,
      .state_changed = true,
      .active = catalog::summarize(*loaded.previous),
      .issues = loaded.previous->release_issues,
  };
  auto receipt = append_event(
      log_, correlation, ExecutionEventKind::state_transition, "rollback",
      ExecutionResult::succeeded, {}, {}, loaded.previous->revision);
  if (!receipt.persisted) {
    result.logging_error = std::move(receipt.error);
  }
  return finish(std::move(result));
}

}  // namespace azzs::application
