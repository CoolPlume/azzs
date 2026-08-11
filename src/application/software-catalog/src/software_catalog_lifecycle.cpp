#include "azzs/application/software_catalog_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace azzs::application::software_catalog {
namespace {

namespace catalog_domain = domain::software_catalog;

constexpr std::uint32_t k_machine_payload_version = 3;
constexpr std::uint32_t k_draft_payload_version = 1;
constexpr std::uint32_t k_unsaved_payload_version = 2;
constexpr std::size_t k_max_catalog_bytes = 16U * 1024U * 1024U;
constexpr std::array<std::byte, 8> k_machine_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'C'}, std::byte{'A'},
    std::byte{'T'}, std::byte{'M'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::array<std::byte, 8> k_draft_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'C'}, std::byte{'A'},
    std::byte{'T'}, std::byte{'D'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::array<std::byte, 8> k_unsaved_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'C'}, std::byte{'A'},
    std::byte{'T'}, std::byte{'U'}, std::byte{'0'}, std::byte{'1'},
};

struct PersistedCatalog final {
  CatalogCandidateOrigin origin{CatalogCandidateOrigin::built_in};
  std::optional<EffectiveCatalogIdentity> effective_identity;
  std::string content_identity;
  std::string bytes;

  auto operator<=>(PersistedCatalog const&) const = default;
};

enum class StableItemKind : std::uint8_t {
  software = 1,
  driver = 2,
};

struct StableItemLedgerEntry final {
  std::string id;
  StableItemKind kind{StableItemKind::software};
  std::string anchor_identity;

  auto operator<=>(StableItemLedgerEntry const&) const = default;
};

struct MachineCatalogState final {
  std::optional<PersistedCatalog> current;
  std::optional<PersistedCatalog> previous;
  std::optional<catalog_domain::PublishedCatalogReference> last_formal_release;
  std::vector<StableItemLedgerEntry> stable_items;

  auto operator<=>(MachineCatalogState const&) const = default;
};

struct SubjectDraftState final {
  std::optional<std::string> saved;

  auto operator<=>(SubjectDraftState const&) const = default;
};

enum class UnsavedCheckpointKind : std::uint8_t {
  editor_content = 1,
  cleanup_only = 2,
};

struct UnsavedCheckpointPayload final {
  UnsavedCheckpointKind kind{UnsavedCheckpointKind::editor_content};
  std::string bytes;
};

class ByteWriter final {
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

  [[nodiscard]] domain::StateBytes finish() && { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class ByteReader final {
 public:
  explicit ByteReader(std::span<std::byte const> bytes) : bytes_(bytes) {}

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

  [[nodiscard]] bool raw(std::span<std::byte> value) {
    if (remaining() < value.size()) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                value.size(), value.begin());
    position_ += value.size();
    return true;
  }

  [[nodiscard]] bool text(std::string& value) {
    std::uint32_t size{};
    if (!u32(size) || size > k_max_catalog_bytes || remaining() < size) {
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

[[nodiscard]] std::optional<std::uint8_t> encode_origin(
    CatalogCandidateOrigin origin) {
  switch (origin) {
    case CatalogCandidateOrigin::built_in:
      return 1;
    case CatalogCandidateOrigin::update:
      return 2;
    case CatalogCandidateOrigin::manual_import:
      return 3;
    case CatalogCandidateOrigin::saved_draft:
      return 4;
    case CatalogCandidateOrigin::rollback:
      return 5;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CatalogCandidateOrigin> decode_origin(
    std::uint8_t value) {
  switch (value) {
    case 1:
      return CatalogCandidateOrigin::built_in;
    case 2:
      return CatalogCandidateOrigin::update;
    case 3:
      return CatalogCandidateOrigin::manual_import;
    case 4:
      return CatalogCandidateOrigin::saved_draft;
    case 5:
      return CatalogCandidateOrigin::rollback;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_effective_identity(
    EffectiveCatalogIdentity identity) {
  switch (identity) {
    case EffectiveCatalogIdentity::released:
      return 1;
    case EffectiveCatalogIdentity::local_trial:
      return 2;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EffectiveCatalogIdentity>
decode_effective_identity(std::uint8_t value) {
  switch (value) {
    case 1:
      return EffectiveCatalogIdentity::released;
    case 2:
      return EffectiveCatalogIdentity::local_trial;
    default:
      return std::nullopt;
  }
}

void encode_persisted_catalog(ByteWriter& writer,
                              PersistedCatalog const& catalog) {
  writer.u8(*encode_origin(catalog.origin));
  writer.u8(*encode_effective_identity(
      catalog.effective_identity.value_or(EffectiveCatalogIdentity::local_trial)));
  writer.text(catalog.content_identity);
  writer.text(catalog.bytes);
}

[[nodiscard]] bool decode_persisted_catalog(ByteReader& reader,
                                            PersistedCatalog& catalog) {
  std::uint8_t encoded_origin{};
  std::uint8_t encoded_identity{};
  if (!reader.u8(encoded_origin) || !reader.u8(encoded_identity)) {
    return false;
  }
  auto origin = decode_origin(encoded_origin);
  auto identity = decode_effective_identity(encoded_identity);
  if (!origin.has_value() || !identity.has_value() ||
      !reader.text(catalog.content_identity) ||
      catalog.content_identity.empty() || !reader.text(catalog.bytes)) {
    return false;
  }
  catalog.origin = *origin;
  catalog.effective_identity = *identity;
  if (catalog_domain::content_identity(catalog.bytes) !=
      catalog.content_identity) {
    return false;
  }
  return true;
}

[[nodiscard]] domain::StateBytes encode_machine_state(
    MachineCatalogState const& state) {
  ByteWriter writer;
  writer.raw(k_machine_magic);
  writer.u32(k_machine_payload_version);
  writer.u8(state.current.has_value() ? 1 : 0);
  if (state.current.has_value()) {
    encode_persisted_catalog(writer, *state.current);
  }
  writer.u8(state.previous.has_value() ? 1 : 0);
  if (state.previous.has_value()) {
    encode_persisted_catalog(writer, *state.previous);
  }
  writer.u8(state.last_formal_release.has_value() ? 1 : 0);
  if (state.last_formal_release.has_value()) {
    writer.u64(state.last_formal_release->revision);
    writer.text(state.last_formal_release->content_identity);
  }
  writer.u32(static_cast<std::uint32_t>(state.stable_items.size()));
  for (auto const& item : state.stable_items) {
    writer.u8(static_cast<std::uint8_t>(item.kind));
    writer.text(item.id);
    writer.text(item.anchor_identity);
  }
  return std::move(writer).finish();
}

[[nodiscard]] bool decode_machine_state(
    std::span<std::byte const> bytes, MachineCatalogState& state) {
  ByteReader reader{bytes};
  std::array<std::byte, k_machine_magic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t has_current{};
  std::uint8_t has_previous{};
  std::uint8_t has_formal_release{};
  if (!reader.raw(magic) || magic != k_machine_magic || !reader.u32(version) ||
      version != k_machine_payload_version || !reader.u8(has_current) ||
      has_current > 1) {
    return false;
  }
  state = {};
  if (has_current != 0) {
    PersistedCatalog current;
    if (!decode_persisted_catalog(reader, current)) {
      return false;
    }
    state.current = std::move(current);
  }
  if (!reader.u8(has_previous) || has_previous > 1) {
    return false;
  }
  if (has_previous != 0) {
    PersistedCatalog previous;
    if (!decode_persisted_catalog(reader, previous)) {
      return false;
    }
    state.previous = std::move(previous);
  }
  if (!reader.u8(has_formal_release) || has_formal_release > 1) {
    return false;
  }
  if (has_formal_release != 0) {
    catalog_domain::PublishedCatalogReference reference;
    if (!reader.u64(reference.revision) || reference.revision == 0 ||
        !reader.text(reference.content_identity) ||
        reference.content_identity.empty()) {
      return false;
    }
    state.last_formal_release = std::move(reference);
  }
  std::uint32_t ledger_count{};
  if (!reader.u32(ledger_count) || ledger_count > 100000U) {
    return false;
  }
  state.stable_items.reserve(ledger_count);
  for (std::uint32_t index = 0; index < ledger_count; ++index) {
    std::uint8_t kind{};
    StableItemLedgerEntry item;
    if (!reader.u8(kind) ||
        (kind != static_cast<std::uint8_t>(StableItemKind::software) &&
         kind != static_cast<std::uint8_t>(StableItemKind::driver)) ||
        !reader.text(item.id) || item.id.empty() ||
        !reader.text(item.anchor_identity)) {
      return false;
    }
    item.kind = static_cast<StableItemKind>(kind);
    state.stable_items.push_back(std::move(item));
  }
  return reader.remaining() == 0;
}

[[nodiscard]] domain::StateBytes encode_draft_state(
    SubjectDraftState const& state) {
  ByteWriter writer;
  writer.raw(k_draft_magic);
  writer.u32(k_draft_payload_version);
  writer.u8(state.saved.has_value() ? 1 : 0);
  if (state.saved.has_value()) {
    writer.text(*state.saved);
  }
  return std::move(writer).finish();
}

[[nodiscard]] bool decode_draft_state(std::span<std::byte const> bytes,
                                      SubjectDraftState& state) {
  ByteReader reader{bytes};
  std::array<std::byte, k_draft_magic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t has_saved{};
  if (!reader.raw(magic) || magic != k_draft_magic || !reader.u32(version) ||
      version != k_draft_payload_version || !reader.u8(has_saved) ||
      has_saved > 1) {
    return false;
  }
  state = {};
  if (has_saved != 0) {
    std::string saved;
    if (!reader.text(saved)) {
      return false;
    }
    state.saved = std::move(saved);
  }
  return reader.remaining() == 0;
}

[[nodiscard]] domain::StateBytes encode_unsaved(
    std::string_view bytes,
    UnsavedCheckpointKind kind = UnsavedCheckpointKind::editor_content) {
  ByteWriter writer;
  writer.raw(k_unsaved_magic);
  writer.u32(k_unsaved_payload_version);
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.text(bytes);
  return std::move(writer).finish();
}

[[nodiscard]] std::optional<UnsavedCheckpointPayload> decode_unsaved(
    std::span<std::byte const> bytes) {
  ByteReader reader{bytes};
  std::array<std::byte, k_unsaved_magic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t encoded_kind{};
  std::string value;
  if (!reader.raw(magic) || magic != k_unsaved_magic ||
      !reader.u32(version)) {
    return std::nullopt;
  }
  if (version == 1) {
    if (!reader.text(value) || reader.remaining() != 0) {
      return std::nullopt;
    }
    return UnsavedCheckpointPayload{
        .kind = UnsavedCheckpointKind::editor_content,
        .bytes = std::move(value),
    };
  }
  if (version != k_unsaved_payload_version || !reader.u8(encoded_kind) ||
      (encoded_kind !=
           static_cast<std::uint8_t>(UnsavedCheckpointKind::editor_content) &&
       encoded_kind !=
           static_cast<std::uint8_t>(UnsavedCheckpointKind::cleanup_only)) ||
      !reader.text(value) || reader.remaining() != 0) {
    return std::nullopt;
  }
  return UnsavedCheckpointPayload{
      .kind = static_cast<UnsavedCheckpointKind>(encoded_kind),
      .bytes = std::move(value),
  };
}

[[nodiscard]] domain::DeviceState state_with_payload(
    domain::StateBytes payload) {
  return domain::DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = std::move(payload)},
  };
}

[[nodiscard]] std::size_t item_count(
    std::optional<catalog_domain::SoftwareCatalogDocument> const& document) {
  if (!document.has_value()) {
    return 0;
  }
  return document->software.size() + document->drivers.size();
}

[[nodiscard]] std::string fallback_correlation(std::uint64_t sequence) {
  return "catalog-correlation-" + std::to_string(sequence);
}

[[nodiscard]] std::string operation_id(std::string_view action,
                                       std::uint64_t sequence) {
  return std::string{action} + "-" + std::to_string(sequence);
}

void append_anchor_part(std::string& value, std::string_view part) {
  value += std::to_string(part.size());
  value.push_back(':');
  value.append(part);
  value.push_back(';');
}

[[nodiscard]] std::string software_anchor_identity(
    catalog_domain::SoftwareDefinition const& software) {
  if (software.branch.empty()) {
    return {};
  }
  std::string anchor{"software;"};
  append_anchor_part(anchor, software.branch);
  append_anchor_part(anchor, software.install_profile.value_or(""));
  return catalog_domain::content_identity(anchor);
}

[[nodiscard]] std::string driver_anchor_identity(
    catalog_domain::DriverDefinition const& driver) {
  if (driver.branch.empty() || !driver.entry_type.has_value() ||
      driver.hardware_kinds.empty()) {
    return {};
  }
  std::string anchor{"driver;"};
  append_anchor_part(anchor, driver.branch);
  append_anchor_part(
      anchor, std::to_string(static_cast<int>(*driver.entry_type)));
  auto hardware = driver.hardware_kinds;
  std::ranges::sort(hardware);
  for (auto const& kind : hardware) {
    append_anchor_part(anchor, kind);
  }
  return catalog_domain::content_identity(anchor);
}

[[nodiscard]] std::vector<StableItemLedgerEntry> ledger_entries(
    catalog_domain::SoftwareCatalogDocument const& document) {
  std::vector<StableItemLedgerEntry> entries;
  entries.reserve(document.software.size() + document.drivers.size());
  for (auto const& software : document.software) {
    entries.push_back(StableItemLedgerEntry{
        .id = software.id,
        .kind = StableItemKind::software,
        .anchor_identity = software_anchor_identity(software),
    });
  }
  for (auto const& driver : document.drivers) {
    entries.push_back(StableItemLedgerEntry{
        .id = driver.id,
        .kind = StableItemKind::driver,
        .anchor_identity = driver_anchor_identity(driver),
    });
  }
  std::ranges::sort(entries, {}, &StableItemLedgerEntry::id);
  return entries;
}

[[nodiscard]] std::string_view origin_name(
    CatalogCandidateOrigin origin) noexcept {
  switch (origin) {
    case CatalogCandidateOrigin::built_in:
      return "built-in";
    case CatalogCandidateOrigin::update:
      return "update";
    case CatalogCandidateOrigin::manual_import:
      return "manual-import";
    case CatalogCandidateOrigin::saved_draft:
      return "saved-draft";
    case CatalogCandidateOrigin::rollback:
      return "rollback";
  }
  return "unknown";
}

[[nodiscard]] std::string_view issue_code_name(
    catalog_domain::CatalogIssueCode code) noexcept {
  using enum catalog_domain::CatalogIssueCode;
  switch (code) {
    case malformed_toml:
      return "malformed_toml";
    case unsupported_schema:
      return "unsupported_schema";
    case missing_required_field:
      return "missing_required_field";
    case invalid_field:
      return "invalid_field";
    case duplicate_field:
      return "duplicate_field";
    case duplicate_stable_id:
      return "duplicate_stable_id";
    case stable_id_reused:
      return "stable_id_reused";
    case unknown_execution_semantics:
      return "unknown_execution_semantics";
    case invalid_reference:
      return "invalid_reference";
    case missing_dependency:
      return "missing_dependency";
    case dependency_cycle:
      return "dependency_cycle";
    case unavailable_dependency:
      return "unavailable_dependency";
    case draft_release_state:
      return "draft_release_state";
    case required_item_missing:
      return "required_item_missing";
    case required_item_disabled:
      return "required_item_disabled";
    case release_revision_regression:
      return "release_revision_regression";
    case release_revision_conflict:
      return "release_revision_conflict";
    case release_dependency_error:
      return "release_dependency_error";
    case install_profile_unavailable:
      return "install_profile_unavailable";
    case install_profile_not_release_ready:
      return "install_profile_not_release_ready";
    case prohibited_content:
      return "prohibited_content";
  }
  return "unknown_catalog_issue";
}

}  // namespace

class SoftwareCatalogLifecycle::Impl final {
 public:
  Impl(DeviceStateStore& states, ExecutionLog& log,
       SharedOperationOccupancy& occupancy, SoftwareCatalogFileReader& files,
       SoftwareCatalogCodec& codec, catalog_domain::SoftwareCatalogPolicy policy,
       CatalogMaintenanceAccess& maintenance_access,
       domain::StateSubject subject)
      : states_(states),
        log_(log),
        occupancy_(occupancy),
        files_(files),
        codec_(codec),
        policy_(std::move(policy)),
        maintenance_access_(maintenance_access),
        machine_key_(domain::StateKey::machine(
            domain::AggregateId{"software-catalog-active"})),
        draft_key_(domain::StateKey::for_subject(
            std::move(subject),
            domain::AggregateId{"software-catalog-draft"})) {}

  struct EvaluatedCatalog final {
    PersistedCatalog persisted;
    std::optional<catalog_domain::SoftwareCatalogDocument> document;
    catalog_domain::RuntimeCatalogLoad runtime;
    catalog_domain::SoftwareCatalogReleaseGate release_gate;
    ActiveCatalogInfo info;
  };

  struct PreparedCandidate final {
    CatalogCandidatePreview preview;
    PersistedCatalog persisted;
    CorrelationId correlation;
  };

  [[nodiscard]] CatalogActionResult restore() {
    prepared_.reset();
    unsaved_.reset();
    recovered_unsaved_ = false;
    cleanup_pending_ = false;
    checkpoint_cleanup_pending_ = false;
    checkpoint_conflict_recovered_ = false;
    recovery_read_only_ = false;
    retained_unreadable_current_.reset();
    current_.reset();
    previous_.reset();
    machine_state_ = {};
    draft_state_ = {};
    machine_revision_.reset();
    draft_revision_.reset();
    mode_ = CatalogLifecycleMode::not_restored;
    machine_access_ = CatalogAggregateAccess::not_restored;
    draft_access_ = CatalogAggregateAccess::not_restored;
    error_.clear();

    auto machine = load_machine();

    std::optional<EvaluatedCatalog> restored_current;
    std::optional<EvaluatedCatalog> restored_previous;
    if (machine_state_.current.has_value()) {
      restored_current = evaluate(*machine_state_.current);
    }
    if (machine_state_.previous.has_value()) {
      restored_previous = evaluate(*machine_state_.previous);
    }

    // A future catalog in either N/N-1 generation makes the whole aggregate
    // opaque to this workbench before the independent draft aggregate can write.
    if (restored_current.has_value() &&
        has_unknown_catalog_mode(*restored_current)) {
      enter_recovery_read_only(*restored_current, true);
    }
    if (restored_previous.has_value() &&
        has_unknown_catalog_mode(*restored_previous)) {
      enter_recovery_read_only(*restored_previous, false);
    }

    if (restored_current.has_value()) {
      if (restored_current->runtime.accepted()) {
        current_ = std::move(*restored_current);
      } else {
        append_restore_error(
            "stored current catalog cannot be loaded by this workbench");
      }
    }
    if (restored_previous.has_value() &&
        restored_previous->runtime.accepted()) {
      previous_ = std::move(*restored_previous);
    }

    auto draft = load_draft(!recovery_read_only_);

    if (!recovery_read_only_) {
      restore_unsaved_checkpoint();
    }
    if (!recovery_read_only_ && draft_state_.saved.has_value() && current_.has_value() &&
        current_->persisted.origin == CatalogCandidateOrigin::saved_draft &&
        catalog_domain::content_identity(*draft_state_.saved) ==
            current_->info.content_identity &&
        !unsaved_.has_value()) {
      auto cleanup = clear_applied_draft();
      if (!cleanup.succeeded()) {
        cleanup_pending_ = true;
      }
    }

    auto const failed = machine.code == CatalogActionCode::persistence_failed ||
                        draft.code == CatalogActionCode::persistence_failed;
    auto const occupied = machine.code == CatalogActionCode::occupied ||
                          draft.code == CatalogActionCode::occupied;
    auto const read_only = recovery_read_only_ ||
                          machine.code == CatalogActionCode::read_only ||
                          draft.code == CatalogActionCode::read_only;
    if (!machine.succeeded()) {
      append_restore_error(machine.message);
    }
    if (!draft.succeeded()) {
      append_restore_error(draft.message);
    }
    if (failed || occupied) {
      mode_ = CatalogLifecycleMode::failed;
      return CatalogActionResult{
          .code = failed ? CatalogActionCode::persistence_failed
                         : CatalogActionCode::occupied,
          .message = error_,
      };
    }
    if (read_only) {
      mode_ = CatalogLifecycleMode::read_only;
      return CatalogActionResult{
          .code = CatalogActionCode::read_only,
          .message = error_,
      };
    }
    mode_ = CatalogLifecycleMode::ready;
    return CatalogActionResult{
        .code = CatalogActionCode::succeeded,
        .message = error_,
    };
  }

  [[nodiscard]] SoftwareCatalogLifecycleSnapshot snapshot() const {
    SoftwareCatalogLifecycleSnapshot value{
        .mode = mode_,
        .machine_access = machine_access_,
        .draft_access = draft_access_,
        .error = error_,
    };
    if (current_.has_value()) {
      value.current = current_->info;
      value.current_toml_bytes = current_->persisted.bytes;
      value.current_document = current_->document;
      value.current_catalog = current_->runtime.catalog;
    }
    value.retained_unreadable_current_toml_bytes =
        retained_unreadable_current_;
    if (previous_.has_value()) {
      value.previous = previous_->info;
      value.previous_catalog = previous_->runtime.catalog;
    }
    value.draft.saved_present = draft_state_.saved.has_value();
    value.draft.cleanup_pending = cleanup_pending_;
    value.draft.checkpoint_cleanup_pending = checkpoint_cleanup_pending_;
    if (unsaved_.has_value()) {
      value.draft.toml_bytes = unsaved_;
      value.draft.state = recovered_unsaved_
                              ? DraftWorkState::recovered_unsaved
                              : DraftWorkState::unsaved_changes;
      fill_draft_validation(value.draft, *unsaved_);
    } else if (draft_state_.saved.has_value()) {
      value.draft.toml_bytes = draft_state_.saved;
      value.draft.state = DraftWorkState::saved_not_applied;
      fill_draft_validation(value.draft, *draft_state_.saved);
    }
    return value;
  }

  [[nodiscard]] CatalogCandidatePreview preview_built_in() {
    if (recovery_read_only_) {
      return recovery_read_only_preview(CatalogCandidateOrigin::built_in);
    }
    return preview_candidate(CatalogCandidateOrigin::built_in,
                             files_.read_built_in());
  }

  [[nodiscard]] CatalogCandidatePreview preview_update() {
    if (recovery_read_only_) {
      return recovery_read_only_preview(CatalogCandidateOrigin::update);
    }
    return preview_candidate(CatalogCandidateOrigin::update,
                             files_.read_update());
  }

  [[nodiscard]] CatalogCandidatePreview preview_manual_import(
      std::string path) {
    if (recovery_read_only_) {
      return recovery_read_only_preview(CatalogCandidateOrigin::manual_import,
                                        std::move(path));
    }
    if (!debug_mode_enabled()) {
      CatalogCandidatePreview preview{
          .origin = CatalogCandidateOrigin::manual_import,
          .path = std::move(path),
          .error = "manual catalog import requires debug mode",
      };
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    return preview_candidate(CatalogCandidateOrigin::manual_import,
                             files_.read_manual_import(path));
  }

  [[nodiscard]] CatalogCandidatePreview preview_candidate(
      CatalogCandidateOrigin origin, CatalogFileRead read) {
    prepared_.reset();
    CatalogCandidatePreview preview{
        .origin = origin,
        .path = std::move(read.path),
    };
    if (recovery_read_only_) {
      return recovery_read_only_preview(origin, std::move(preview.path));
    }
    if (mode_ == CatalogLifecycleMode::not_restored) {
      preview.error = "catalog lifecycle must be restored before preview";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    if (origin != CatalogCandidateOrigin::built_in &&
        origin != CatalogCandidateOrigin::update &&
        origin != CatalogCandidateOrigin::manual_import) {
      preview.error = "file preview origin is not supported";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    if (!read.succeeded) {
      preview.error = read.error.empty() ? "catalog file read failed" : read.error;
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    PersistedCatalog persisted{.origin = origin, .bytes = read.bytes};
    preview.content_identity =
        catalog_domain::content_identity(persisted.bytes);
    auto evaluated = evaluate(persisted);
    preview.runtime = evaluated.runtime;
    preview.release_gate = evaluated.release_gate;
    if (evaluated.document.has_value()) {
      preview.revision = evaluated.document->revision;
    }
    preview.item_count = item_count(evaluated.document);
    preview.selection_impact = selection_impact(evaluated.runtime);
    if (!evaluated.runtime.accepted()) {
      preview.error = "candidate catalog failed runtime loading";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    if ((origin == CatalogCandidateOrigin::built_in ||
         origin == CatalogCandidateOrigin::update) &&
        !evaluated.release_gate.passed()) {
      preview.error =
          "built-in and update catalogs must pass the formal release gate";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    if (current_.has_value() &&
        preview.revision < current_->info.revision) {
      preview.downgrade = true;
      if (origin != CatalogCandidateOrigin::manual_import) {
        preview.error =
            "downgrade is available only through confirmed debug import";
        static_cast<void>(log_preview(preview, ExecutionResult::failed));
        return preview;
      }
    }

    preview.ready = true;
    preview.confirmation_token = make_confirmation_token(persisted.bytes);
    auto correlation = log_preview(preview, ExecutionResult::succeeded);
    prepared_ = PreparedCandidate{.preview = preview,
                                  .persisted = std::move(persisted),
                                  .correlation = std::move(correlation)};
    return preview;
  }

  [[nodiscard]] CatalogCandidatePreview preview_rollback() {
    prepared_.reset();
    CatalogCandidatePreview preview{
        .origin = CatalogCandidateOrigin::rollback,
    };
    if (recovery_read_only_) {
      return recovery_read_only_preview(CatalogCandidateOrigin::rollback);
    }
    if (mode_ == CatalogLifecycleMode::not_restored) {
      preview.error = "catalog lifecycle must be restored before rollback";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    if (!machine_state_.previous.has_value()) {
      preview.error = "no previous usable software catalog is available";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    auto persisted = *machine_state_.previous;
    persisted.origin = CatalogCandidateOrigin::rollback;
    preview.content_identity =
        catalog_domain::content_identity(persisted.bytes);
    auto evaluated = evaluate(persisted);
    preview.runtime = evaluated.runtime;
    preview.release_gate = evaluated.release_gate;
    if (evaluated.document.has_value()) {
      preview.revision = evaluated.document->revision;
    }
    preview.item_count = item_count(evaluated.document);
    preview.selection_impact = selection_impact(evaluated.runtime);
    if (!evaluated.runtime.accepted()) {
      preview.error = "previous catalog no longer passes runtime loading";
      static_cast<void>(log_preview(preview, ExecutionResult::failed));
      return preview;
    }
    preview.downgrade = current_.has_value() &&
                        preview.revision < current_->info.revision;
    preview.ready = true;
    preview.confirmation_token = make_confirmation_token(persisted.bytes);
    auto correlation = log_preview(preview, ExecutionResult::succeeded);
    prepared_ = PreparedCandidate{.preview = preview,
                                  .persisted = std::move(persisted),
                                  .correlation = std::move(correlation)};
    return preview;
  }

  [[nodiscard]] CatalogActionResult apply_preview(std::string_view token) {
    if (mode_ == CatalogLifecycleMode::not_restored) {
      return action(CatalogActionCode::not_restored,
                    "catalog lifecycle is not restored");
    }
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (temporary_recovery_access()) {
      return action(CatalogActionCode::rejected,
                    "temporary recovery access cannot apply a catalog");
    }
    if (!machine_writable_ || !machine_revision_.has_value()) {
      return aggregate_unavailable(machine_access_,
                                   "machine catalog state is unavailable");
    }
    auto attach_impact = [&](CatalogActionResult result) {
      if (prepared_.has_value()) {
        result.selection_impact = prepared_->preview.selection_impact;
      }
      return result;
    };
    if (!prepared_.has_value() || !prepared_->preview.ready || token.empty() ||
        token != prepared_->preview.confirmation_token) {
      auto result = attach_impact(action(
          CatalogActionCode::rejected,
          "catalog confirmation token is missing or stale"));
      if (prepared_.has_value()) {
        auto receipt = append_log(
            prepared_->correlation, "apply", ExecutionResult::failed,
            {{"catalog_apply_rejection", "missing-or-stale-token",
              DiagnosticValueDisposition::retain},
             {"catalog_origin",
              std::string{origin_name(prepared_->persisted.origin)},
              DiagnosticValueDisposition::retain},
             {"candidate_path", prepared_->preview.path,
              DiagnosticValueDisposition::sensitive},
             {"catalog_identity", prepared_->preview.content_identity,
              DiagnosticValueDisposition::retain}});
        attach_log_receipt(result, std::move(receipt));
      }
      return result;
    }
    if (!prepared_->preview.log_persisted) {
      auto result = attach_impact(action(
          CatalogActionCode::persistence_failed,
          "catalog preview was not durably logged; application was blocked"));
      result.runtime = prepared_->preview.runtime;
      result.release_gate = prepared_->preview.release_gate;
      result.log_error = prepared_->preview.log_error;
      return result;
    }
    if (prepared_->persisted.origin == CatalogCandidateOrigin::manual_import &&
        !debug_mode_enabled()) {
      auto receipt = append_log(
          prepared_->correlation, "apply", ExecutionResult::failed,
          {{"catalog_apply_rejection", "debug-mode-required",
            DiagnosticValueDisposition::retain},
           {"catalog_origin",
            std::string{origin_name(prepared_->persisted.origin)},
            DiagnosticValueDisposition::retain},
           {"candidate_path", prepared_->preview.path,
            DiagnosticValueDisposition::sensitive},
           {"catalog_identity", prepared_->preview.content_identity,
            DiagnosticValueDisposition::retain}});
      auto result = attach_impact(action(
          CatalogActionCode::debug_mode_required,
          "manual catalog import requires debug mode"));
      attach_log_receipt(result, std::move(receipt));
      return result;
    }
    if (unsaved_.has_value()) {
      auto receipt = append_log(
          prepared_->correlation, "apply", ExecutionResult::failed,
          {{"catalog_apply_rejection", "unsaved-edits-present",
            DiagnosticValueDisposition::retain},
           {"catalog_origin",
            std::string{origin_name(prepared_->persisted.origin)},
            DiagnosticValueDisposition::retain},
           {"candidate_path", prepared_->preview.path,
            DiagnosticValueDisposition::sensitive},
           {"catalog_identity", prepared_->preview.content_identity,
            DiagnosticValueDisposition::retain}});
      auto result = attach_impact(action(
          CatalogActionCode::rejected,
          "unsaved or recovered edits must be saved or discarded first"));
      attach_log_receipt(result, std::move(receipt));
      return result;
    }

    auto correlation = prepared_->correlation;
    if (correlation.value.empty()) {
      correlation = begin_correlation();
    }
    auto reject_apply = [&](CatalogActionResult result,
                            std::string reason) {
      result.selection_impact = prepared_->preview.selection_impact;
      auto receipt = append_log(
          correlation, "apply", ExecutionResult::failed,
          {{"catalog_apply_rejection", std::move(reason),
            DiagnosticValueDisposition::retain},
           {"catalog_origin",
            std::string{origin_name(prepared_->persisted.origin)},
            DiagnosticValueDisposition::retain},
           {"candidate_path", prepared_->preview.path,
            DiagnosticValueDisposition::sensitive},
           {"catalog_identity", prepared_->preview.content_identity,
            DiagnosticValueDisposition::retain}});
      attach_log_receipt(result, std::move(receipt));
      return result;
    };

    auto persisted = prepared_->persisted;
    auto reevaluated = evaluate(persisted);
    if (!reevaluated.runtime.accepted()) {
      return reject_apply(
          action_with_validation(CatalogActionCode::rejected,
                                 "catalog failed runtime revalidation",
                                 reevaluated),
          "runtime-revalidation-failed");
    }
    if ((prepared_->persisted.origin == CatalogCandidateOrigin::built_in ||
         prepared_->persisted.origin == CatalogCandidateOrigin::update) &&
        !reevaluated.release_gate.passed()) {
      return reject_apply(
          action_with_validation(
              CatalogActionCode::rejected,
              "built-in or update catalog failed formal release revalidation",
              reevaluated),
          "release-revalidation-failed");
    }
    if (prepared_->persisted.origin == CatalogCandidateOrigin::update &&
        current_.has_value() &&
        reevaluated.info.revision < current_->info.revision) {
      return reject_apply(
          action_with_validation(
              CatalogActionCode::rejected,
              "catalog update cannot silently downgrade the active revision",
              reevaluated),
          "update-revision-regression");
    }
    persisted = reevaluated.persisted;

    auto lease = acquire_lease("catalog-apply", correlation);
    if (!lease.lease.has_value()) {
      auto receipt = append_log(
          correlation, "apply", ExecutionResult::failed,
          {{"catalog_apply_rejection", "occupancy-unavailable",
            DiagnosticValueDisposition::retain},
           {"catalog_origin",
            std::string{origin_name(prepared_->persisted.origin)},
            DiagnosticValueDisposition::retain},
           {"candidate_path", prepared_->preview.path,
            DiagnosticValueDisposition::sensitive},
           {"catalog_identity", prepared_->preview.content_identity,
            DiagnosticValueDisposition::retain}});
      auto result = attach_impact(action(lease.code, lease.message));
      attach_log_receipt(result, std::move(receipt));
      return result;
    }

    auto const candidate_identity = reevaluated.info.content_identity;
    if (current_.has_value() &&
        current_->info.content_identity == candidate_identity &&
        current_->persisted.bytes == persisted.bytes &&
        (current_->info.identity == reevaluated.info.identity ||
         persisted.origin == CatalogCandidateOrigin::manual_import ||
         persisted.origin == CatalogCandidateOrigin::saved_draft ||
         persisted.origin == CatalogCandidateOrigin::rollback)) {
      auto result = action_with_validation(CatalogActionCode::no_change,
                                           "catalog content is already active",
                                           reevaluated,
                                           prepared_->preview.selection_impact);
      if (prepared_->persisted.origin == CatalogCandidateOrigin::saved_draft) {
        auto cleanup = clear_applied_draft();
        if (!cleanup.succeeded()) {
          cleanup_pending_ = true;
          result.code = CatalogActionCode::applied_cleanup_pending;
          result.message = cleanup.message;
        } else {
          result.draft_changed = cleanup.draft_changed;
        }
      }
      auto receipt = append_log(
          correlation, "apply", ExecutionResult::succeeded,
          {{"catalog_apply_result", "no-change",
            DiagnosticValueDisposition::retain},
           {"catalog_origin",
            std::string{origin_name(prepared_->persisted.origin)},
            DiagnosticValueDisposition::retain},
           {"candidate_path", prepared_->preview.path,
            DiagnosticValueDisposition::sensitive},
           {"catalog_identity", candidate_identity,
            DiagnosticValueDisposition::retain}});
      attach_log_receipt(result, std::move(receipt));
      if (!result.log_persisted &&
          (result.draft_changed || cleanup_pending_)) {
        result.code = CatalogActionCode::applied_log_incomplete;
        result.message = cleanup_pending_
                             ? "catalog content was already active; action "
                               "log and saved draft cleanup are incomplete"
                             : "catalog content was already active; saved "
                               "draft was cleared but the action log was not "
                               "persisted";
      }
      release_lease(*lease.lease, correlation);
      prepared_.reset();
      return result;
    }

    MachineCatalogState desired{
        .current = persisted,
        .previous = machine_state_.current,
        .last_formal_release = machine_state_.last_formal_release,
        .stable_items = merged_stable_item_ledger(*reevaluated.document),
    };
    if (reevaluated.info.identity == EffectiveCatalogIdentity::released &&
        (!desired.last_formal_release.has_value() ||
         reevaluated.info.revision >
             desired.last_formal_release->revision)) {
      desired.last_formal_release = catalog_domain::PublishedCatalogReference{
          .revision = reevaluated.info.revision,
          .content_identity = reevaluated.info.content_identity,
      };
    }
    auto committed = commit_machine(desired);
    if (!committed.succeeded()) {
      release_lease(*lease.lease, correlation);
      committed.runtime = reevaluated.runtime;
      committed.release_gate = reevaluated.release_gate;
      committed.selection_impact = prepared_->preview.selection_impact;
      auto receipt = append_log(
          correlation, "apply", ExecutionResult::failed,
          {{"catalog_identity", candidate_identity,
            DiagnosticValueDisposition::retain},
           {"catalog_origin",
            std::string{origin_name(prepared_->persisted.origin)},
            DiagnosticValueDisposition::retain},
           {"candidate_path", prepared_->preview.path,
            DiagnosticValueDisposition::sensitive}});
      attach_log_receipt(committed, std::move(receipt));
      return committed;
    }

    machine_state_ = std::move(desired);
    current_ = std::move(reevaluated);
    previous_.reset();
    if (machine_state_.previous.has_value()) {
      auto evaluated_previous = evaluate(*machine_state_.previous);
      if (evaluated_previous.runtime.accepted()) {
        previous_ = std::move(evaluated_previous);
      }
    }

    auto result = action_with_validation(CatalogActionCode::succeeded,
                                         "catalog applied", *current_,
                                         prepared_->preview.selection_impact);
    result.current_changed = true;
    if (current_->persisted.origin == CatalogCandidateOrigin::saved_draft) {
      auto cleanup = clear_applied_draft();
      if (!cleanup.succeeded()) {
        cleanup_pending_ = true;
        result.code = CatalogActionCode::applied_cleanup_pending;
        result.message =
            "catalog applied; saved draft cleanup must be retried";
      } else {
        result.draft_changed = cleanup.draft_changed;
      }
    }
    std::vector<DiagnosticField> applied_fields{
        {"catalog_revision", std::to_string(current_->info.revision),
         DiagnosticValueDisposition::retain},
        {"catalog_identity", current_->info.content_identity,
         DiagnosticValueDisposition::retain},
        {"catalog_application_id", current_->info.application_id,
         DiagnosticValueDisposition::retain},
        {"catalog_release_state",
         current_->info.identity == EffectiveCatalogIdentity::released
             ? "release"
             : "local-trial",
         DiagnosticValueDisposition::retain},
        {"catalog_origin", std::string{origin_name(current_->persisted.origin)},
         DiagnosticValueDisposition::retain},
        {"candidate_path", prepared_->preview.path,
         DiagnosticValueDisposition::sensitive},
    };
    append_issue_fields(applied_fields, "catalog_runtime_issue",
                        current_->runtime.issues);
    append_issue_fields(applied_fields, "catalog_release_issue",
                        current_->release_gate.issues);
    auto receipt = append_log(correlation, "apply", ExecutionResult::succeeded,
                              std::move(applied_fields));
    attach_log_receipt(result, std::move(receipt));
    if (!result.log_persisted) {
      result.code = CatalogActionCode::applied_log_incomplete;
      result.message = cleanup_pending_
                           ? "catalog applied; action log and saved draft "
                             "cleanup are incomplete"
                           : "catalog applied; action log was not persisted";
    }
    release_lease(*lease.lease, correlation);
    prepared_.reset();
    return result;
  }

  [[nodiscard]] CatalogActionResult edit(std::string bytes) {
    if (mode_ == CatalogLifecycleMode::not_restored) {
      return action(CatalogActionCode::not_restored,
                    "catalog lifecycle is not restored");
    }
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (!editor_access_enabled()) {
      return action(CatalogActionCode::debug_mode_required,
                    "catalog editing requires debug mode or recovery access");
    }
    if (!draft_writable_) {
      return aggregate_unavailable(draft_access_,
                                   "catalog draft state is unavailable");
    }
    if (bytes.size() > k_max_catalog_bytes) {
      return action(CatalogActionCode::rejected,
                    "catalog draft exceeds the supported size");
    }
    unsaved_ = std::move(bytes);
    recovered_unsaved_ = false;
    auto evaluated = evaluate(PersistedCatalog{
        .origin = CatalogCandidateOrigin::saved_draft,
        .bytes = *unsaved_,
    });
    auto result = action_with_validation(CatalogActionCode::succeeded,
                                         "unsaved catalog edits retained",
                                         evaluated);
    result.draft_changed = true;
    return result;
  }

  [[nodiscard]] CatalogActionResult edit_document(
      catalog_domain::SoftwareCatalogDocument document) {
    return edit(codec_.encode(document));
  }

  [[nodiscard]] CatalogActionResult checkpoint_unsaved() {
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (temporary_recovery_access()) {
      return action(CatalogActionCode::rejected,
                    "temporary recovery access cannot checkpoint catalog edits");
    }
    if (!unsaved_.has_value()) {
      return action(CatalogActionCode::no_change,
                    "there are no unsaved catalog edits to checkpoint");
    }
    if (!draft_revision_.has_value()) {
      return aggregate_unavailable(draft_access_,
                                   "catalog draft aggregate is unavailable");
    }
    if (checkpoint_conflict_recovered_) {
      return action(CatalogActionCode::conflict,
                    "recovered conflicting edits must be saved or discarded "
                    "before creating another checkpoint");
    }
    auto existing = states_.read_checkpoint(draft_key_, *draft_revision_);
    if (existing.status == CheckpointStatus::available &&
        existing.checkpoint.has_value()) {
      auto payload = decode_unsaved(existing.checkpoint->payload);
      if (!payload.has_value()) {
        return action(CatalogActionCode::persistence_failed,
                      "existing catalog checkpoint cannot be decoded");
      }
      if (payload->kind == UnsavedCheckpointKind::editor_content &&
          payload->bytes != *unsaved_) {
        return reject_foreign_checkpoint(existing);
      }
    } else if (existing.status != CheckpointStatus::absent &&
               existing.status != CheckpointStatus::consumed) {
      return action(checkpoint_error_code(existing.status), existing.error);
    }
    auto written = states_.write_checkpoint(
        draft_key_, StateCheckpoint{
                        .base_revision = *draft_revision_,
                        .payload = encode_unsaved(*unsaved_),
                    });
    if (written.status != CheckpointStatus::available) {
      return action(checkpoint_error_code(written.status), written.error);
    }
    auto result = action(CatalogActionCode::succeeded,
                         "unsaved catalog checkpoint persisted");
    result.draft_changed = true;
    return result;
  }

  [[nodiscard]] CatalogActionResult save_draft() {
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (!editor_access_enabled()) {
      return action(CatalogActionCode::debug_mode_required,
                    "saving a catalog draft requires debug or recovery access");
    }
    if (!unsaved_.has_value()) {
      return action(CatalogActionCode::no_change,
                    "there are no unsaved catalog edits");
    }
    auto evaluated = evaluate(PersistedCatalog{
        .origin = CatalogCandidateOrigin::saved_draft,
        .bytes = *unsaved_,
    });
    auto const malformed = std::ranges::any_of(
        evaluated.runtime.issues, [](catalog_domain::CatalogIssue const& issue) {
          return issue.code == catalog_domain::CatalogIssueCode::malformed_toml;
        });
    if (!evaluated.document.has_value() || malformed) {
      return action_with_validation(
          CatalogActionCode::rejected,
          "malformed TOML cannot be saved as a catalog draft", evaluated);
    }
    auto const saved_bytes = *unsaved_;
    auto const checkpoint_plan = checkpoint_cleanup_plan(saved_bytes);
    SubjectDraftState desired{.saved = saved_bytes};
    auto committed = commit_draft(desired);
    if (!committed.succeeded()) {
      return committed;
    }
    draft_state_ = std::move(desired);
    unsaved_.reset();
    recovered_unsaved_ = false;
    checkpoint_conflict_recovered_ = false;
    checkpoint_cleanup_pending_ =
        checkpoint_plan == CheckpointCleanupPlan::retry_later;
    std::string cleanup_error;
    if (checkpoint_plan == CheckpointCleanupPlan::write_cleanup &&
        !replace_and_consume_checkpoint(saved_bytes, cleanup_error)) {
      checkpoint_cleanup_pending_ = true;
      auto result = action_with_validation(
          CatalogActionCode::saved_cleanup_pending,
          "catalog draft was saved; recovery checkpoint cleanup must be "
          "retried: " + cleanup_error,
          evaluated);
      result.draft_changed = true;
      return result;
    }
    if (checkpoint_plan == CheckpointCleanupPlan::retry_later) {
      auto result = action_with_validation(
          CatalogActionCode::saved_cleanup_pending,
          "catalog draft was saved; checkpoint ownership could not be "
          "determined and must be retried",
          evaluated);
      result.draft_changed = true;
      return result;
    }
    auto result = action_with_validation(CatalogActionCode::succeeded,
                                         "catalog draft saved without applying",
                                         evaluated);
    result.draft_changed = true;
    return result;
  }

  [[nodiscard]] CatalogActionResult delete_saved_draft() {
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (temporary_recovery_access()) {
      return action(CatalogActionCode::rejected,
                    "temporary recovery access cannot delete a saved draft");
    }
    if (!debug_mode_enabled()) {
      return action(CatalogActionCode::debug_mode_required,
                    "deleting a catalog draft requires an active debug session");
    }
    if (unsaved_.has_value()) {
      return action(CatalogActionCode::rejected,
                    "discard unsaved edits before deleting the saved draft");
    }
    if (!draft_state_.saved.has_value()) {
      return action(CatalogActionCode::no_change,
                    "no saved catalog draft exists");
    }
    auto const deleted_bytes = *draft_state_.saved;
    auto const checkpoint_plan = checkpoint_cleanup_plan(deleted_bytes);
    SubjectDraftState desired;
    auto committed = commit_draft(desired);
    if (!committed.succeeded()) {
      return committed;
    }
    draft_state_ = {};
    cleanup_pending_ = false;
    checkpoint_cleanup_pending_ =
        checkpoint_plan == CheckpointCleanupPlan::retry_later;
    std::string cleanup_error;
    if (checkpoint_plan == CheckpointCleanupPlan::write_cleanup &&
        !replace_and_consume_checkpoint(deleted_bytes, cleanup_error)) {
      checkpoint_cleanup_pending_ = true;
      auto result = action(
          CatalogActionCode::saved_cleanup_pending,
          "saved catalog draft was deleted; recovery checkpoint cleanup "
          "must be retried: " + cleanup_error);
      result.draft_changed = true;
      return result;
    }
    if (checkpoint_plan == CheckpointCleanupPlan::retry_later) {
      auto result = action(
          CatalogActionCode::saved_cleanup_pending,
          "saved catalog draft was deleted; checkpoint ownership could not "
          "be determined and must be retried");
      result.draft_changed = true;
      return result;
    }
    auto result = action(CatalogActionCode::succeeded,
                         "saved catalog draft deleted");
    result.draft_changed = true;
    return result;
  }

  [[nodiscard]] CatalogActionResult discard_unsaved() {
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (!editor_access_enabled()) {
      return action(CatalogActionCode::debug_mode_required,
                    "discarding catalog edits requires debug or recovery access");
    }
    if (!unsaved_.has_value()) {
      return action(CatalogActionCode::no_change,
                    "there are no unsaved catalog edits");
    }
    if (!draft_revision_.has_value()) {
      return aggregate_unavailable(draft_access_,
                                   "catalog draft aggregate is unavailable");
    }
    if (checkpoint_conflict_recovered_) {
      std::string cleanup_error;
      if (!replace_and_consume_checkpoint(*unsaved_, cleanup_error)) {
        return action(CatalogActionCode::persistence_failed, cleanup_error);
      }
    } else {
      auto consumed = states_.consume_checkpoint(draft_key_, *draft_revision_);
      if (consumed.status != CheckpointStatus::consumed &&
          consumed.status != CheckpointStatus::absent) {
        return action(checkpoint_error_code(consumed.status), consumed.error);
      }
    }
    unsaved_.reset();
    recovered_unsaved_ = false;
    checkpoint_conflict_recovered_ = false;
    checkpoint_cleanup_pending_ = false;
    auto result = action(CatalogActionCode::succeeded,
                         "only unsaved catalog edits were discarded");
    result.draft_changed = true;
    return result;
  }

  [[nodiscard]] CatalogActionResult apply_saved_draft() {
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (temporary_recovery_access()) {
      return action(CatalogActionCode::rejected,
                    "temporary recovery access cannot apply a saved draft");
    }
    if (!debug_mode_enabled()) {
      return action(CatalogActionCode::debug_mode_required,
                    "applying a catalog draft requires an active debug session");
    }
    if (unsaved_.has_value()) {
      return action(CatalogActionCode::rejected,
                    "save or discard unsaved edits before applying");
    }
    if (!draft_state_.saved.has_value()) {
      return action(CatalogActionCode::unavailable,
                    "no saved catalog draft exists");
    }
    auto persisted = PersistedCatalog{
        .origin = CatalogCandidateOrigin::saved_draft,
        .bytes = *draft_state_.saved,
    };
    auto evaluated = evaluate(persisted);
    if (!evaluated.runtime.accepted()) {
      return action_with_validation(CatalogActionCode::rejected,
                                    "saved draft failed runtime revalidation",
                                    evaluated);
    }
    CatalogCandidatePreview preview{
        .ready = true,
        .origin = CatalogCandidateOrigin::saved_draft,
        .content_identity = evaluated.info.content_identity,
        .revision = evaluated.info.revision,
        .item_count = item_count(evaluated.document),
        .selection_impact = selection_impact(evaluated.runtime),
        .runtime = evaluated.runtime,
        .release_gate = evaluated.release_gate,
    };
    preview.confirmation_token = make_confirmation_token(persisted.bytes);
    auto correlation = log_preview(preview, ExecutionResult::succeeded);
    prepared_ = PreparedCandidate{.preview = preview,
                                  .persisted = std::move(persisted),
                                  .correlation = std::move(correlation)};
    return apply_preview(preview.confirmation_token);
  }

  [[nodiscard]] CatalogActionResult handle_close(CatalogCloseChoice choice) {
    if (recovery_read_only_) {
      return recovery_read_only_action();
    }
    if (!unsaved_.has_value()) {
      return action(CatalogActionCode::succeeded,
                    "workbench may close; no unsaved catalog edits exist");
    }
    switch (choice) {
      case CatalogCloseChoice::save_draft_and_close:
        return save_draft();
      case CatalogCloseChoice::discard_unsaved_and_close:
        return discard_unsaved();
      case CatalogCloseChoice::return_to_editor:
        return action(CatalogActionCode::returned_to_editor,
                      "workbench close cancelled; edits retained");
    }
    return action(CatalogActionCode::rejected, "unknown close choice");
  }

 private:
  struct LeaseAttempt final {
    CatalogActionCode code{CatalogActionCode::occupied};
    std::optional<OperationLease> lease;
    std::string message;
  };

  enum class CheckpointCleanupPlan {
    write_cleanup,
    preserve_foreign,
    retry_later,
  };

  [[nodiscard]] static bool has_unknown_catalog_mode(
      EvaluatedCatalog const& evaluated) {
    return std::ranges::any_of(
        evaluated.runtime.issues, [](catalog_domain::CatalogIssue const& issue) {
          return issue.code == catalog_domain::CatalogIssueCode::unsupported_schema ||
                 issue.code ==
                     catalog_domain::CatalogIssueCode::unknown_execution_semantics;
        });
  }

  void enter_recovery_read_only(EvaluatedCatalog const& evaluated,
                                bool is_current_generation) {
    recovery_read_only_ = true;
    machine_writable_ = false;
    machine_access_ = CatalogAggregateAccess::read_only;
    if (is_current_generation) {
      retained_unreadable_current_ = evaluated.persisted.bytes;
      append_restore_error(
          "stored current catalog uses unknown execution semantics");
      return;
    }
    append_restore_error(
        "stored previous catalog uses unknown execution semantics");
  }

  [[nodiscard]] bool debug_mode_enabled() const noexcept {
    return maintenance_access_.editor_access() ==
           CatalogEditorAccess::debug_mode;
  }

  [[nodiscard]] bool editor_access_enabled() const noexcept {
    return maintenance_access_.editor_access() !=
           CatalogEditorAccess::unavailable;
  }

  [[nodiscard]] bool temporary_recovery_access() const noexcept {
    return maintenance_access_.editor_access() ==
           CatalogEditorAccess::temporary_close_recovery;
  }

  [[nodiscard]] static CatalogActionResult recovery_read_only_action() {
    return action(CatalogActionCode::read_only,
                  "stored catalog uses unknown execution semantics and is "
                  "retained read-only until this workbench is updated");
  }

  [[nodiscard]] static CatalogCandidatePreview recovery_read_only_preview(
      CatalogCandidateOrigin origin, std::string path = {}) {
    return CatalogCandidatePreview{
        .origin = origin,
        .path = std::move(path),
        .error = "stored catalog uses unknown execution semantics and is "
                 "retained read-only until this workbench is updated",
    };
  }

  [[nodiscard]] CheckpointCleanupPlan checkpoint_cleanup_plan(
      std::string_view bytes) {
    if (checkpoint_conflict_recovered_) {
      return CheckpointCleanupPlan::write_cleanup;
    }
    if (!draft_revision_.has_value()) {
      return CheckpointCleanupPlan::retry_later;
    }
    auto checkpoint = states_.read_checkpoint(draft_key_, *draft_revision_);
    if (checkpoint.status == CheckpointStatus::absent ||
        checkpoint.status == CheckpointStatus::consumed) {
      return CheckpointCleanupPlan::write_cleanup;
    }
    if (checkpoint.status != CheckpointStatus::available ||
        !checkpoint.checkpoint.has_value()) {
      return CheckpointCleanupPlan::retry_later;
    }
    auto payload = decode_unsaved(checkpoint.checkpoint->payload);
    if (!payload.has_value()) {
      return CheckpointCleanupPlan::retry_later;
    }
    if (payload->kind == UnsavedCheckpointKind::cleanup_only ||
        payload->bytes == bytes) {
      return CheckpointCleanupPlan::write_cleanup;
    }
    return CheckpointCleanupPlan::preserve_foreign;
  }

  [[nodiscard]] CatalogActionResult reject_foreign_checkpoint(
      CheckpointResult const& checkpoint) const {
    return action(CatalogActionCode::conflict,
                  checkpoint.error.empty()
                      ? "another catalog instance owns unsaved checkpoint data"
                      : checkpoint.error);
  }

  [[nodiscard]] CatalogActionResult load_machine() {
    auto read = states_.inspect(machine_key_);
    if (read.mode == StateReadMode::uninitialized) {
      auto initialized = states_.initialize(
          machine_key_, state_with_payload(encode_machine_state({})));
      if (!resolve_initialization(initialized, machine_key_,
                                  encode_machine_state({}),
                                  machine_revision_)) {
        auto failed =
            state_failure(initialized, "machine catalog state initialization");
        machine_access_ = aggregate_access(failed.code);
        return failed;
      }
      machine_writable_ = true;
      machine_access_ = CatalogAggregateAccess::writable;
      return action(CatalogActionCode::succeeded,
                    "machine catalog state initialized");
    }
    if (read.mode == StateReadMode::read_only_corrupt ||
        read.mode == StateReadMode::read_only_future) {
      machine_writable_ = false;
      machine_access_ = CatalogAggregateAccess::read_only;
      mode_ = CatalogLifecycleMode::read_only;
      append_restore_error(read.error);
      return action(CatalogActionCode::read_only, read.error);
    }
    if (read.mode == StateReadMode::busy) {
      machine_writable_ = false;
      machine_access_ = CatalogAggregateAccess::occupied;
      return action(CatalogActionCode::occupied, read.error);
    }
    if (!read.snapshot.has_value()) {
      machine_writable_ = false;
      machine_access_ = CatalogAggregateAccess::failed;
      mode_ = CatalogLifecycleMode::failed;
      append_restore_error(read.error);
      return action(CatalogActionCode::persistence_failed, read.error);
    }
    machine_revision_ = read.snapshot->revision;
    machine_writable_ = true;
    machine_access_ = CatalogAggregateAccess::writable;
    if (!decode_machine_state(read.snapshot->state.value.payload,
                              machine_state_)) {
      mode_ = CatalogLifecycleMode::read_only;
      machine_writable_ = false;
      machine_access_ = CatalogAggregateAccess::read_only;
      append_restore_error(
          "software catalog machine payload is unsupported or damaged");
      return action(CatalogActionCode::read_only, error_);
    }
    return action(CatalogActionCode::succeeded,
                  "machine catalog state restored");
  }

  [[nodiscard]] CatalogActionResult load_draft(bool initialize_if_absent) {
    auto read = states_.inspect(draft_key_);
    if (read.mode == StateReadMode::uninitialized) {
      if (!initialize_if_absent) {
        draft_writable_ = false;
        draft_access_ = CatalogAggregateAccess::read_only;
        return action(CatalogActionCode::succeeded,
                      "catalog draft state remains absent during read-only "
                      "catalog recovery");
      }
      auto initialized = states_.initialize(
          draft_key_, state_with_payload(encode_draft_state({})));
      if (!resolve_initialization(initialized, draft_key_,
                                  encode_draft_state({}), draft_revision_)) {
        auto failed =
            state_failure(initialized, "catalog draft state initialization");
        draft_access_ = aggregate_access(failed.code);
        return failed;
      }
      draft_writable_ = true;
      draft_access_ = CatalogAggregateAccess::writable;
      return action(CatalogActionCode::succeeded,
                    "catalog draft state initialized");
    }
    if (read.mode == StateReadMode::read_only_corrupt ||
        read.mode == StateReadMode::read_only_future) {
      draft_writable_ = false;
      draft_access_ = CatalogAggregateAccess::read_only;
      mode_ = CatalogLifecycleMode::read_only;
      append_restore_error(read.error);
      return action(CatalogActionCode::read_only, read.error);
    }
    if (read.mode == StateReadMode::busy) {
      draft_writable_ = false;
      draft_access_ = CatalogAggregateAccess::occupied;
      return action(CatalogActionCode::occupied, read.error);
    }
    if (!read.snapshot.has_value()) {
      draft_writable_ = false;
      draft_access_ = CatalogAggregateAccess::failed;
      mode_ = CatalogLifecycleMode::failed;
      append_restore_error(read.error);
      return action(CatalogActionCode::persistence_failed, read.error);
    }
    draft_revision_ = read.snapshot->revision;
    draft_writable_ = initialize_if_absent;
    draft_access_ = initialize_if_absent ? CatalogAggregateAccess::writable
                                         : CatalogAggregateAccess::read_only;
    if (!decode_draft_state(read.snapshot->state.value.payload, draft_state_)) {
      mode_ = CatalogLifecycleMode::read_only;
      draft_writable_ = false;
      draft_access_ = CatalogAggregateAccess::read_only;
      append_restore_error(
          "software catalog draft payload is unsupported or damaged");
      return action(CatalogActionCode::read_only, error_);
    }
    return action(CatalogActionCode::succeeded,
                  "catalog draft state restored");
  }

  void restore_unsaved_checkpoint() {
    if (!draft_revision_.has_value()) {
      return;
    }
    auto checkpoint = states_.read_checkpoint(draft_key_, *draft_revision_);
    if (checkpoint.status == CheckpointStatus::absent ||
        checkpoint.status == CheckpointStatus::consumed) {
      return;
    }
    if ((checkpoint.status != CheckpointStatus::available &&
         checkpoint.status != CheckpointStatus::conflict) ||
        !checkpoint.checkpoint.has_value()) {
      append_restore_error(
          checkpoint.error.empty()
              ? "unsaved catalog checkpoint could not be recovered"
              : "unsaved catalog checkpoint could not be recovered: " +
                    checkpoint.error);
      return;
    }
    auto recovered = decode_unsaved(checkpoint.checkpoint->payload);
    if (!recovered.has_value()) {
      append_restore_error("unsaved catalog checkpoint could not be recovered");
      return;
    }
    auto const matches_saved_draft =
        draft_state_.saved.has_value() &&
        *draft_state_.saved == recovered->bytes;
    if (checkpoint.status == CheckpointStatus::conflict &&
        recovered->kind == UnsavedCheckpointKind::editor_content &&
        !matches_saved_draft) {
      unsaved_ = std::move(recovered->bytes);
      recovered_unsaved_ = true;
      checkpoint_conflict_recovered_ = true;
      append_restore_error(
          "unsaved catalog checkpoint conflicts with a newer saved draft; "
          "explicit save or discard is required");
      return;
    }
    if (checkpoint.status == CheckpointStatus::conflict) {
      std::string cleanup_error;
      if (!replace_and_consume_checkpoint(recovered->bytes, cleanup_error)) {
        checkpoint_cleanup_pending_ = true;
        append_restore_error(
            "stale catalog checkpoint cleanup must be retried: " +
            cleanup_error);
      }
      return;
    }
    if (recovered->kind == UnsavedCheckpointKind::cleanup_only ||
        matches_saved_draft) {
      auto consumed = states_.consume_checkpoint(draft_key_, *draft_revision_);
      if (consumed.status != CheckpointStatus::consumed &&
          consumed.status != CheckpointStatus::absent) {
        checkpoint_cleanup_pending_ = true;
        append_restore_error(
            "saved catalog checkpoint cleanup must be retried: " +
                consumed.error);
      }
      return;
    }
    unsaved_ = std::move(recovered->bytes);
    recovered_unsaved_ = true;
  }

  [[nodiscard]] bool replace_and_consume_checkpoint(
      std::string_view bytes, std::string& error) {
    if (!draft_revision_.has_value()) {
      error = "catalog draft aggregate is unavailable";
      return false;
    }
    auto written = states_.write_checkpoint(
        draft_key_, StateCheckpoint{
                        .base_revision = *draft_revision_,
                        .payload = encode_unsaved(
                            bytes, UnsavedCheckpointKind::cleanup_only),
                    });
    if (written.status != CheckpointStatus::available) {
      error = written.error.empty() ? "checkpoint replacement failed"
                                    : std::move(written.error);
      return false;
    }
    auto consumed = states_.consume_checkpoint(draft_key_, *draft_revision_);
    if (consumed.status != CheckpointStatus::consumed &&
        consumed.status != CheckpointStatus::absent) {
      error = consumed.error.empty() ? "checkpoint consumption failed"
                                    : std::move(consumed.error);
      return false;
    }
    return true;
  }

  void append_restore_error(std::string_view value) {
    if (value.empty()) {
      return;
    }
    if (error_.find(value) != std::string::npos) {
      return;
    }
    if (!error_.empty()) {
      error_ += "; ";
    }
    error_ += value;
  }

  void validate_stable_item_ledger(
      catalog_domain::SoftwareCatalogDocument const& document,
      catalog_domain::RuntimeCatalogLoad& runtime) const {
    if (!runtime.accepted()) {
      return;
    }
    auto const proposed = ledger_entries(document);
    for (auto const& item : proposed) {
      auto const existing = std::ranges::find(
          machine_state_.stable_items, item.id,
          &StableItemLedgerEntry::id);
      if (existing == machine_state_.stable_items.end()) {
        continue;
      }
      auto const incompatible_anchor =
          !existing->anchor_identity.empty() &&
          !item.anchor_identity.empty() &&
          existing->anchor_identity != item.anchor_identity;
      if (existing->kind != item.kind || incompatible_anchor) {
        runtime.issues.push_back(catalog_domain::CatalogIssue{
            .scope = catalog_domain::CatalogIssueScope::package,
            .code = catalog_domain::CatalogIssueCode::stable_id_reused,
            .location = item.kind == StableItemKind::software
                            ? "software." + item.id + ".id"
                            : "drivers." + item.id + ".id",
            .item_id = item.id,
            .message = "stable catalog id was reused for a different product "
                       "identity or execution anchor",
        });
      }
    }
    if (std::ranges::any_of(
            runtime.issues, [](catalog_domain::CatalogIssue const& issue) {
              return issue.scope == catalog_domain::CatalogIssueScope::package;
            })) {
      runtime.outcome = catalog_domain::RuntimeLoadOutcome::rejected;
      runtime.catalog.reset();
    }
  }

  [[nodiscard]] std::vector<StableItemLedgerEntry> merged_stable_item_ledger(
      catalog_domain::SoftwareCatalogDocument const& document) const {
    auto merged = machine_state_.stable_items;
    for (auto const& item : ledger_entries(document)) {
      auto existing = std::ranges::find(
          merged, item.id, &StableItemLedgerEntry::id);
      if (existing == merged.end()) {
        merged.push_back(item);
      } else if (existing->anchor_identity.empty() &&
                 !item.anchor_identity.empty()) {
        existing->anchor_identity = item.anchor_identity;
      }
    }
    std::ranges::sort(merged, {}, &StableItemLedgerEntry::id);
    return merged;
  }

  void apply_formal_release_references(
      catalog_domain::SoftwareCatalogDocument const& document,
      std::string_view content_identity,
      catalog_domain::SoftwareCatalogReleaseGate& gate) const {
    std::array<std::optional<catalog_domain::PublishedCatalogReference>, 2>
        references{policy_.last_published,
                   machine_state_.last_formal_release};
    for (std::size_t index = 0; index < references.size(); ++index) {
      auto const& reference = references[index];
      if (!reference.has_value()) {
        continue;
      }
      if (index == 1 && references[0].has_value() &&
          *references[0] == *reference) {
        continue;
      }
      auto code = std::optional<catalog_domain::CatalogIssueCode>{};
      auto message = std::string{};
      if (document.revision < reference->revision) {
        code = catalog_domain::CatalogIssueCode::release_revision_regression;
        message = "release revision is older than a known formal catalog";
      } else if (document.revision == reference->revision &&
                 content_identity != reference->content_identity) {
        code = catalog_domain::CatalogIssueCode::release_revision_conflict;
        message = "one formal release revision cannot identify different "
                  "content";
      }
      if (code.has_value()) {
        gate.issues.push_back(catalog_domain::CatalogIssue{
            .scope = catalog_domain::CatalogIssueScope::release,
            .code = *code,
            .location = "revision",
            .message = std::move(message),
        });
      }
    }
    if (!gate.issues.empty()) {
      gate.outcome = catalog_domain::ReleaseGateOutcome::failed;
    }
  }

  [[nodiscard]] EvaluatedCatalog evaluate(PersistedCatalog persisted) const {
    EvaluatedCatalog value{.persisted = std::move(persisted)};
    auto decoded = codec_.decode(value.persisted.bytes);
    value.document = decoded.document;
    if (!decoded.document.has_value()) {
      value.runtime.issues = std::move(decoded.issues);
      value.release_gate.outcome =
          catalog_domain::ReleaseGateOutcome::not_evaluated;
      value.release_gate.issues = value.runtime.issues;
      return value;
    }
    if (!decoded.issues.empty()) {
      value.runtime.issues = std::move(decoded.issues);
      value.release_gate.outcome =
          catalog_domain::ReleaseGateOutcome::not_evaluated;
      value.release_gate.issues = value.runtime.issues;
      return value;
    }
    value.runtime = catalog_domain::validate_for_runtime(*value.document, policy_);
    auto const identity =
        catalog_domain::content_identity(value.persisted.bytes);
    if (!value.persisted.content_identity.empty() &&
        value.persisted.content_identity != identity) {
      value.runtime.outcome = catalog_domain::RuntimeLoadOutcome::rejected;
      value.runtime.catalog.reset();
      value.runtime.issues.push_back(catalog_domain::CatalogIssue{
          .scope = catalog_domain::CatalogIssueScope::package,
          .code = catalog_domain::CatalogIssueCode::malformed_toml,
          .location = "content_identity",
          .message = "persisted catalog content identity does not match its "
                     "payload",
      });
    }
    value.persisted.content_identity = identity;
    validate_stable_item_ledger(*value.document, value.runtime);
    auto release_policy = policy_;
    release_policy.last_published.reset();
    value.release_gate = catalog_domain::evaluate_release_gate(
        *value.document, value.runtime, release_policy, identity);
    if (value.runtime.accepted()) {
      apply_formal_release_references(*value.document, identity,
                                      value.release_gate);
    }
    if (!value.runtime.accepted()) {
      return value;
    }
    value.info = ActiveCatalogInfo{
        .revision = value.document->revision,
        .item_count = item_count(value.document),
        .origin = value.persisted.origin,
        .identity = value.persisted.effective_identity.value_or(
            (value.release_gate.passed() &&
             (value.persisted.origin == CatalogCandidateOrigin::built_in ||
              value.persisted.origin == CatalogCandidateOrigin::update))
                ? EffectiveCatalogIdentity::released
                : EffectiveCatalogIdentity::local_trial),
        .content_identity = identity,
        .application_id = "catalog-" + std::to_string(value.document->revision) +
                          "-" + identity,
        .release_issues = value.release_gate.issues,
    };
    for (auto const& issue : value.runtime.issues) {
      if (issue.scope == catalog_domain::CatalogIssueScope::item) {
        value.info.local_issues.push_back(issue);
      }
    }
    value.persisted.effective_identity = value.info.identity;
    return value;
  }

  void fill_draft_validation(CatalogDraftInfo& info,
                             std::string const& bytes) const {
    auto evaluated = evaluate(PersistedCatalog{
        .origin = CatalogCandidateOrigin::saved_draft,
        .bytes = bytes,
    });
    info.validation_failed = !evaluated.runtime.accepted();
    info.runtime_issues = evaluated.runtime.issues;
    info.release_issues = evaluated.release_gate.issues;
    auto const malformed = std::ranges::any_of(
        evaluated.runtime.issues, [](catalog_domain::CatalogIssue const& issue) {
          return issue.code == catalog_domain::CatalogIssueCode::malformed_toml;
        });
    if (evaluated.document.has_value() && !malformed) {
      info.document = evaluated.document;
      info.candidate_revision = evaluated.document->revision;
    }
  }

  [[nodiscard]] CatalogSelectionImpact selection_impact(
      catalog_domain::RuntimeCatalogLoad const& candidate) const {
    CatalogSelectionImpact impact;
    if (!candidate.catalog.has_value()) {
      return impact;
    }
    struct Definition final {
      std::optional<catalog_domain::SoftwareDefinition> software;
      std::optional<catalog_domain::DriverDefinition> driver;
      bool available{true};

      auto operator<=>(Definition const&) const = default;
    };
    auto collect = [](catalog_domain::RuntimeSoftwareCatalog const& catalog) {
      std::unordered_map<std::string, Definition> values;
      for (auto const& software : catalog.software) {
        auto semantics = software.definition;
        semantics.name.clear();
        semantics.notice.clear();
        semantics.optimization_note.reset();
        semantics.education.reset();
        semantics.localizations.clear();
        semantics.display_extensions.clear();
        for (auto& source : semantics.sources) {
          source.display_extensions.clear();
          for (auto& history : source.history) {
            history.reason.clear();
            history.display_extensions.clear();
          }
        }
        values.emplace(
            software.definition.id,
            Definition{.software = std::move(semantics),
                       .available = software.availability ==
                                    catalog_domain::ItemAvailability::available});
      }
      for (auto const& driver : catalog.drivers) {
        auto semantics = driver.definition;
        semantics.name.clear();
        semantics.notice.clear();
        semantics.localizations.clear();
        semantics.display_extensions.clear();
        for (auto& source : semantics.sources) {
          source.display_extensions.clear();
          for (auto& history : source.history) {
            history.reason.clear();
            history.display_extensions.clear();
          }
        }
        values.emplace(
            driver.definition.id,
            Definition{.driver = std::move(semantics),
                       .available = driver.availability ==
                                    catalog_domain::ItemAvailability::available});
      }
      return values;
    };

    std::unordered_map<std::string, Definition> existing;
    if (current_.has_value() && current_->runtime.catalog.has_value()) {
      existing = collect(*current_->runtime.catalog);
    }
    auto proposed = collect(*candidate.catalog);
    for (auto const& [id, definition] : proposed) {
      auto const old = existing.find(id);
      if (old == existing.end()) {
        impact.added.push_back(id);
        impact.items.push_back(CatalogSelectionImpactItem{
            .id = id,
            .kind = definition.software.has_value()
                        ? CatalogSelectionItemKind::software
                        : CatalogSelectionItemKind::driver,
            .reason = CatalogSelectionImpactReason::added,
        });
        if (!definition.available) {
          impact.items.push_back(CatalogSelectionImpactItem{
              .id = id,
              .kind = definition.software.has_value()
                          ? CatalogSelectionItemKind::software
                          : CatalogSelectionItemKind::driver,
              .reason = CatalogSelectionImpactReason::runtime_unavailable,
          });
        }
      } else if (old->second.software != definition.software ||
                 old->second.driver != definition.driver) {
        impact.changed.push_back(id);
        impact.items.push_back(CatalogSelectionImpactItem{
            .id = id,
            .kind = definition.software.has_value()
                        ? CatalogSelectionItemKind::software
                        : CatalogSelectionItemKind::driver,
            .reason =
                CatalogSelectionImpactReason::execution_semantics_changed,
        });
      }
      if (old != existing.end() &&
          old->second.available != definition.available) {
        if (std::ranges::find(impact.changed, id) == impact.changed.end()) {
          impact.changed.push_back(id);
        }
        impact.items.push_back(CatalogSelectionImpactItem{
            .id = id,
            .kind = definition.software.has_value()
                        ? CatalogSelectionItemKind::software
                        : CatalogSelectionItemKind::driver,
            .reason = definition.available
                          ? CatalogSelectionImpactReason::runtime_available
                          : CatalogSelectionImpactReason::runtime_unavailable,
        });
      }
      if (!definition.available) {
        impact.disabled.push_back(id);
      }
    }
    for (auto const& [id, definition] : existing) {
      static_cast<void>(definition);
      if (!proposed.contains(id)) {
        impact.removed.push_back(id);
        impact.items.push_back(CatalogSelectionImpactItem{
            .id = id,
            .kind = definition.software.has_value()
                        ? CatalogSelectionItemKind::software
                        : CatalogSelectionItemKind::driver,
            .reason = CatalogSelectionImpactReason::removed,
        });
      }
    }
    std::ranges::sort(impact.added);
    std::ranges::sort(impact.removed);
    std::ranges::sort(impact.changed);
    std::ranges::sort(impact.disabled);
    std::ranges::sort(impact.items, [](CatalogSelectionImpactItem const& left,
                                      CatalogSelectionImpactItem const& right) {
      if (left.id != right.id) {
        return left.id < right.id;
      }
      return left.reason < right.reason;
    });
    return impact;
  }

  [[nodiscard]] CatalogActionResult commit_machine(
      MachineCatalogState const& desired) {
    if (!machine_writable_ || !machine_revision_.has_value()) {
      return aggregate_unavailable(machine_access_,
                                   "machine catalog state is not writable");
    }
    auto payload = encode_machine_state(desired);
    auto committed = states_.commit(StateCommitRequest{
        .key = machine_key_,
        .expected_revision = *machine_revision_,
        .state = state_with_payload(payload),
    });
    return resolve_commit(committed, machine_key_, payload, machine_revision_,
                          "machine catalog apply");
  }

  [[nodiscard]] CatalogActionResult commit_draft(
      SubjectDraftState const& desired) {
    if (!draft_writable_ || !draft_revision_.has_value()) {
      return aggregate_unavailable(draft_access_,
                                   "catalog draft state is not writable");
    }
    auto payload = encode_draft_state(desired);
    auto committed = states_.commit(StateCommitRequest{
        .key = draft_key_,
        .expected_revision = *draft_revision_,
        .state = state_with_payload(payload),
    });
    return resolve_commit(committed, draft_key_, payload, draft_revision_,
                          "catalog draft commit");
  }

  [[nodiscard]] CatalogActionResult clear_applied_draft() {
    if (!draft_state_.saved.has_value()) {
      cleanup_pending_ = false;
      return action(CatalogActionCode::no_change,
                    "applied catalog draft is already clear");
    }
    SubjectDraftState desired;
    auto committed = commit_draft(desired);
    if (!committed.succeeded()) {
      return committed;
    }
    draft_state_ = {};
    cleanup_pending_ = false;
    committed.draft_changed = true;
    return committed;
  }

  [[nodiscard]] CatalogActionResult resolve_commit(
      StateCommitResult const& committed, domain::StateKey const& key,
      domain::StateBytes const& desired,
      std::optional<domain::RevisionToken>& revision,
      std::string_view operation) {
    if (committed.status == StateCommitStatus::committed &&
        committed.snapshot.has_value()) {
      revision = committed.snapshot->revision;
      return action(CatalogActionCode::succeeded,
                    std::string{operation} + " committed");
    }
    if (committed.status == StateCommitStatus::outcome_unknown) {
      auto observed = states_.inspect(key);
      if (observed.snapshot.has_value() &&
          observed.snapshot->state.value.payload == desired) {
        revision = observed.snapshot->revision;
        return action(CatalogActionCode::succeeded,
                      std::string{operation} +
                          " committed and confirmed by reread");
      }
      return action(CatalogActionCode::outcome_unknown,
                    committed.error.empty()
                        ? std::string{operation} + " outcome is unknown"
                        : committed.error);
    }
    return state_failure(committed, operation);
  }

  [[nodiscard]] bool resolve_initialization(
      StateCommitResult const& initialized, domain::StateKey const& key,
      domain::StateBytes const& desired,
      std::optional<domain::RevisionToken>& revision) {
    if (initialized.status == StateCommitStatus::committed &&
        initialized.snapshot.has_value()) {
      revision = initialized.snapshot->revision;
      return true;
    }
    if (initialized.status == StateCommitStatus::outcome_unknown) {
      auto observed = states_.inspect(key);
      if (observed.snapshot.has_value() &&
          observed.snapshot->state.value.payload == desired) {
        revision = observed.snapshot->revision;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] CatalogActionResult state_failure(
      StateCommitResult const& result, std::string_view operation) const {
    auto code = CatalogActionCode::persistence_failed;
    if (result.status == StateCommitStatus::conflict) {
      code = CatalogActionCode::conflict;
    } else if (result.status == StateCommitStatus::read_only) {
      code = CatalogActionCode::read_only;
    } else if (result.status == StateCommitStatus::busy) {
      code = CatalogActionCode::occupied;
    } else if (result.status == StateCommitStatus::outcome_unknown) {
      code = CatalogActionCode::outcome_unknown;
    }
    return action(code, result.error.empty()
                            ? std::string{operation} + " failed"
                            : result.error);
  }

  [[nodiscard]] LeaseAttempt acquire_lease(
      std::string_view action_name, CorrelationId const& correlation) {
    auto const sequence = ++operation_sequence_;
    auto correlation_value = correlation.value;
    if (correlation_value.empty()) {
      correlation_value = fallback_correlation(sequence);
    }
    auto acquired = occupancy_.try_acquire(OperationIdentity{
        .kind = "software-catalog",
        .operation_id = operation_id(action_name, sequence),
        .correlation_id = std::move(correlation_value),
    });
    if (acquired.code == OccupancyResultCode::acquired &&
        acquired.lease.has_value()) {
      return {.code = CatalogActionCode::succeeded,
              .lease = std::move(acquired.lease)};
    }
    auto code = CatalogActionCode::occupied;
    if (acquired.code == OccupancyResultCode::read_only) {
      code = CatalogActionCode::read_only;
    } else if (acquired.code == OccupancyResultCode::storage_error ||
               acquired.code == OccupancyResultCode::invalid_request) {
      code = CatalogActionCode::persistence_failed;
    }
    return {.code = code,
            .message = acquired.detail.empty()
                           ? "another device operation owns the occupancy"
                           : acquired.detail};
  }

  void release_lease(OperationLease const& lease,
                     CorrelationId const& correlation) {
    auto released = occupancy_.release(lease);
    if (released.code != OccupancyResultCode::released) {
      append_log(correlation, "occupancy-release", ExecutionResult::failed,
                 {{"catalog_occupancy_release", released.detail,
                   DiagnosticValueDisposition::retain}});
    }
  }

  [[nodiscard]] CorrelationId begin_correlation() {
    auto correlation = log_.begin_correlation();
    if (correlation.value.empty()) {
      correlation.value = fallback_correlation(++operation_sequence_);
    }
    return correlation;
  }

  [[nodiscard]] CorrelationId log_preview(
      CatalogCandidatePreview& preview, ExecutionResult result) {
    if (recovery_read_only_) {
      preview.log_error =
          "catalog recovery is read-only; preview was not logged";
      return {};
    }
    auto correlation = begin_correlation();
    std::vector<DiagnosticField> fields{
        {"candidate_path", preview.path,
         DiagnosticValueDisposition::sensitive},
        {"catalog_identity", preview.content_identity,
         DiagnosticValueDisposition::retain},
        {"catalog_origin", std::string{origin_name(preview.origin)},
         DiagnosticValueDisposition::retain},
        {"catalog_revision", std::to_string(preview.revision),
         DiagnosticValueDisposition::retain},
        {"catalog_item_count", std::to_string(preview.item_count),
         DiagnosticValueDisposition::retain},
    };
    append_issue_fields(fields, "catalog_runtime_issue",
                        preview.runtime.issues);
    append_issue_fields(fields, "catalog_release_issue",
                        preview.release_gate.issues);
    auto receipt = append_log(correlation, "preview", result,
                              std::move(fields));
    preview.log_persisted = receipt.persisted;
    preview.log_error = receipt.persisted
                            ? std::string{}
                            : (receipt.error.empty()
                                   ? "catalog preview log was not persisted"
                                   : std::move(receipt.error));
    return correlation;
  }

  static void append_issue_fields(
      std::vector<DiagnosticField>& fields, std::string_view key,
      std::vector<catalog_domain::CatalogIssue> const& issues) {
    for (auto const& issue : issues) {
      fields.push_back(DiagnosticField{
          .key = std::string{key},
          .value = std::string{issue_code_name(issue.code)},
          .disposition = DiagnosticValueDisposition::retain,
      });
      fields.push_back(DiagnosticField{
          .key = std::string{key} + "_detail",
          .value = issue.location + "|" + issue.item_id + "|" + issue.message,
          .disposition = DiagnosticValueDisposition::sensitive,
      });
    }
  }

  ExecutionLogReceipt append_log(CorrelationId const& correlation,
                                 std::string stage,
                                 ExecutionResult result,
                                 std::vector<DiagnosticField> fields) {
    if (recovery_read_only_) {
      return {.persisted = false,
              .error = "catalog recovery is read-only; log append was blocked"};
    }
    return log_.append(
        correlation,
        ExecutionEvent{
            .kind = ExecutionEventKind::state_transition,
            .component = "software_catalog",
            .stage = std::move(stage),
            .result = result,
            .fields = std::move(fields),
        });
  }

  static void attach_log_receipt(CatalogActionResult& result,
                                 ExecutionLogReceipt receipt) {
    result.log_persisted = receipt.persisted;
    result.log_error = receipt.persisted
                           ? std::string{}
                           : (receipt.error.empty()
                                  ? "catalog action log was not persisted"
                                  : std::move(receipt.error));
  }

  [[nodiscard]] std::string make_confirmation_token(
      std::string_view bytes) {
    return "catalog-preview-" + catalog_domain::content_identity(bytes) + "-" +
           std::to_string(++preview_sequence_);
  }

  [[nodiscard]] static CatalogActionCode checkpoint_error_code(
      CheckpointStatus status) {
    if (status == CheckpointStatus::conflict) {
      return CatalogActionCode::conflict;
    }
    if (status == CheckpointStatus::busy) {
      return CatalogActionCode::occupied;
    }
    if (status == CheckpointStatus::read_only) {
      return CatalogActionCode::read_only;
    }
    return CatalogActionCode::persistence_failed;
  }

  [[nodiscard]] static CatalogAggregateAccess aggregate_access(
      CatalogActionCode code) noexcept {
    if (code == CatalogActionCode::read_only) {
      return CatalogAggregateAccess::read_only;
    }
    if (code == CatalogActionCode::occupied) {
      return CatalogAggregateAccess::occupied;
    }
    return CatalogAggregateAccess::failed;
  }

  [[nodiscard]] static CatalogActionResult aggregate_unavailable(
      CatalogAggregateAccess access, std::string message) {
    auto code = CatalogActionCode::persistence_failed;
    if (access == CatalogAggregateAccess::not_restored) {
      code = CatalogActionCode::not_restored;
    } else if (access == CatalogAggregateAccess::read_only) {
      code = CatalogActionCode::read_only;
    } else if (access == CatalogAggregateAccess::occupied) {
      code = CatalogActionCode::occupied;
    }
    return action(code, std::move(message));
  }

  [[nodiscard]] static CatalogActionResult action(CatalogActionCode code,
                                                  std::string message) {
    return CatalogActionResult{.code = code, .message = std::move(message)};
  }

  [[nodiscard]] static CatalogActionResult action_with_validation(
      CatalogActionCode code, std::string message,
      EvaluatedCatalog const& evaluated,
      CatalogSelectionImpact selection_impact = {}) {
    return CatalogActionResult{
        .code = code,
        .runtime = evaluated.runtime,
        .release_gate = evaluated.release_gate,
        .selection_impact = std::move(selection_impact),
        .message = std::move(message),
    };
  }

  DeviceStateStore& states_;
  ExecutionLog& log_;
  SharedOperationOccupancy& occupancy_;
  SoftwareCatalogFileReader& files_;
  SoftwareCatalogCodec& codec_;
  catalog_domain::SoftwareCatalogPolicy policy_;
  CatalogMaintenanceAccess& maintenance_access_;
  domain::StateKey machine_key_;
  domain::StateKey draft_key_;
  CatalogLifecycleMode mode_{CatalogLifecycleMode::not_restored};
  CatalogAggregateAccess machine_access_{CatalogAggregateAccess::not_restored};
  CatalogAggregateAccess draft_access_{CatalogAggregateAccess::not_restored};
  bool machine_writable_{false};
  bool draft_writable_{false};
  MachineCatalogState machine_state_;
  SubjectDraftState draft_state_;
  std::optional<domain::RevisionToken> machine_revision_;
  std::optional<domain::RevisionToken> draft_revision_;
  std::optional<EvaluatedCatalog> current_;
  std::optional<EvaluatedCatalog> previous_;
  std::optional<std::string> unsaved_;
  bool recovered_unsaved_{false};
  bool checkpoint_conflict_recovered_{false};
  bool cleanup_pending_{false};
  bool checkpoint_cleanup_pending_{false};
  bool recovery_read_only_{false};
  std::optional<std::string> retained_unreadable_current_;
  std::optional<PreparedCandidate> prepared_;
  std::uint64_t preview_sequence_{0};
  std::uint64_t operation_sequence_{0};
  std::string error_;
};

SoftwareCatalogLifecycle::SoftwareCatalogLifecycle(
    DeviceStateStore& states, ExecutionLog& log,
    SharedOperationOccupancy& occupancy, SoftwareCatalogFileReader& files,
    SoftwareCatalogCodec& codec,
    catalog_domain::SoftwareCatalogPolicy policy,
    CatalogMaintenanceAccess& maintenance_access,
    domain::StateSubject state_subject)
    : impl_(std::make_unique<Impl>(states, log, occupancy, files, codec,
                                   std::move(policy),
                                   maintenance_access,
                                   std::move(state_subject))) {}

SoftwareCatalogLifecycle::~SoftwareCatalogLifecycle() = default;

CatalogActionResult SoftwareCatalogLifecycle::restore() {
  return impl_->restore();
}

SoftwareCatalogLifecycleSnapshot SoftwareCatalogLifecycle::snapshot() const {
  return impl_->snapshot();
}

CatalogCandidatePreview SoftwareCatalogLifecycle::preview_built_in() {
  return impl_->preview_built_in();
}

CatalogCandidatePreview SoftwareCatalogLifecycle::preview_update() {
  return impl_->preview_update();
}

CatalogCandidatePreview SoftwareCatalogLifecycle::preview_manual_import(
    std::string path) {
  return impl_->preview_manual_import(std::move(path));
}

CatalogCandidatePreview SoftwareCatalogLifecycle::preview_rollback() {
  return impl_->preview_rollback();
}

CatalogActionResult SoftwareCatalogLifecycle::apply_preview(
    std::string_view confirmation_token) {
  return impl_->apply_preview(confirmation_token);
}

CatalogActionResult SoftwareCatalogLifecycle::edit(std::string toml_bytes) {
  return impl_->edit(std::move(toml_bytes));
}

CatalogActionResult SoftwareCatalogLifecycle::edit_document(
    domain::software_catalog::SoftwareCatalogDocument document) {
  return impl_->edit_document(std::move(document));
}

CatalogActionResult SoftwareCatalogLifecycle::checkpoint_unsaved() {
  return impl_->checkpoint_unsaved();
}

CatalogActionResult SoftwareCatalogLifecycle::save_draft() {
  return impl_->save_draft();
}

CatalogActionResult SoftwareCatalogLifecycle::delete_saved_draft() {
  return impl_->delete_saved_draft();
}

CatalogActionResult SoftwareCatalogLifecycle::discard_unsaved() {
  return impl_->discard_unsaved();
}

CatalogActionResult SoftwareCatalogLifecycle::apply_saved_draft() {
  return impl_->apply_saved_draft();
}

CatalogActionResult SoftwareCatalogLifecycle::handle_close(
    CatalogCloseChoice choice) {
  return impl_->handle_close(choice);
}

}  // namespace azzs::application::software_catalog
