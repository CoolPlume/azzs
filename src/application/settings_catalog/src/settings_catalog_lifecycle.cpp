#include "azzs/settings_catalog/settings_catalog_lifecycle.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::application::settings_catalog {
namespace {

[[nodiscard]] CatalogSnapshotStatus map_snapshot_status(
    CatalogStorageReadStatus status) noexcept {
  switch (status) {
    case CatalogStorageReadStatus::uninitialized:
      return CatalogSnapshotStatus::unavailable;
    case CatalogStorageReadStatus::writable:
      return CatalogSnapshotStatus::available;
    case CatalogStorageReadStatus::recovered_read_only:
      return CatalogSnapshotStatus::recovered_read_only;
    case CatalogStorageReadStatus::read_only:
      return CatalogSnapshotStatus::read_only;
    case CatalogStorageReadStatus::busy:
      return CatalogSnapshotStatus::busy;
    case CatalogStorageReadStatus::failed:
      return CatalogSnapshotStatus::failed;
  }
  return CatalogSnapshotStatus::failed;
}

[[nodiscard]] PrepareStatus map_prepare_status(
    CatalogStorageReadStatus status) noexcept {
  switch (status) {
    case CatalogStorageReadStatus::uninitialized:
      return PrepareStatus::unavailable;
    case CatalogStorageReadStatus::writable:
      return PrepareStatus::ready;
    case CatalogStorageReadStatus::recovered_read_only:
    case CatalogStorageReadStatus::read_only:
      return PrepareStatus::read_only;
    case CatalogStorageReadStatus::busy:
      return PrepareStatus::busy;
    case CatalogStorageReadStatus::failed:
      return PrepareStatus::failed;
  }
  return PrepareStatus::failed;
}

[[nodiscard]] InitializeStatus map_initialize_write(
    CatalogStorageWriteStatus status) noexcept {
  switch (status) {
    case CatalogStorageWriteStatus::committed:
      return InitializeStatus::initialized;
    case CatalogStorageWriteStatus::conflict:
      return InitializeStatus::conflict;
    case CatalogStorageWriteStatus::read_only:
      return InitializeStatus::read_only;
    case CatalogStorageWriteStatus::busy:
      return InitializeStatus::busy;
    case CatalogStorageWriteStatus::failed:
    case CatalogStorageWriteStatus::outcome_unknown:
      return InitializeStatus::failed;
  }
  return InitializeStatus::failed;
}

[[nodiscard]] ConfirmStatus map_confirm_write(
    CatalogStorageWriteStatus status) noexcept {
  switch (status) {
    case CatalogStorageWriteStatus::committed:
      return ConfirmStatus::committed;
    case CatalogStorageWriteStatus::conflict:
      return ConfirmStatus::conflict;
    case CatalogStorageWriteStatus::read_only:
      return ConfirmStatus::read_only;
    case CatalogStorageWriteStatus::busy:
      return ConfirmStatus::busy;
    case CatalogStorageWriteStatus::failed:
      return ConfirmStatus::failed;
    case CatalogStorageWriteStatus::outcome_unknown:
      return ConfirmStatus::outcome_unknown;
  }
  return ConfirmStatus::failed;
}

[[nodiscard]] bool acquired(OccupancyResult const& result) noexcept {
  return result.code == OccupancyResultCode::acquired &&
         result.lease.has_value();
}

[[nodiscard]] InitializeStatus map_initialize_occupancy(
    OccupancyResultCode code) noexcept {
  switch (code) {
    case OccupancyResultCode::occupied:
      return InitializeStatus::occupied;
    case OccupancyResultCode::conflict:
      return InitializeStatus::conflict;
    case OccupancyResultCode::read_only:
      return InitializeStatus::read_only;
    case OccupancyResultCode::acquired:
    case OccupancyResultCode::invalid_request:
    case OccupancyResultCode::observed:
    case OccupancyResultCode::released:
    case OccupancyResultCode::stale_lease:
    case OccupancyResultCode::storage_error:
      return InitializeStatus::failed;
  }
  return InitializeStatus::failed;
}

[[nodiscard]] ConfirmStatus map_confirm_occupancy(
    OccupancyResultCode code) noexcept {
  switch (code) {
    case OccupancyResultCode::occupied:
      return ConfirmStatus::occupied;
    case OccupancyResultCode::conflict:
      return ConfirmStatus::conflict;
    case OccupancyResultCode::read_only:
      return ConfirmStatus::read_only;
    case OccupancyResultCode::acquired:
    case OccupancyResultCode::invalid_request:
    case OccupancyResultCode::observed:
    case OccupancyResultCode::released:
    case OccupancyResultCode::stale_lease:
    case OccupancyResultCode::storage_error:
      return ConfirmStatus::failed;
  }
  return ConfirmStatus::failed;
}

[[nodiscard]] std::string normalized_import_path(std::string_view path) {
  std::string normalized;
  normalized.reserve(path.size());
  bool previous_separator = false;
  for (unsigned char const byte : path) {
    if (byte < 0x20 || byte == 0x7f) {
      return {};
    }
    auto const character = static_cast<char>(byte);
    if (character == '/' || character == '\\') {
      if (!previous_separator) {
        normalized.push_back('/');
      }
      previous_separator = true;
      continue;
    }
    normalized.push_back(character);
    previous_separator = false;
  }
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

[[nodiscard]] std::string path_fingerprint(std::string_view path) {
  std::uint64_t fingerprint = 14'695'981'039'346'656'037ULL;
  for (unsigned char const byte : path) {
    fingerprint ^= byte;
    fingerprint *= 1'099'511'628'211ULL;
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(16, '0');
  for (std::size_t index = 0; index < result.size(); ++index) {
    auto const shift = static_cast<unsigned>((result.size() - index - 1) * 4);
    result[index] = kHex[(fingerprint >> shift) & 0x0fU];
  }
  return result;
}

[[nodiscard]] std::string redacted_import_path(std::string_view path) {
  auto const normalized = normalized_import_path(path);
  auto const separator = normalized.find_last_of('/');
  auto filename = separator == std::string::npos
                      ? std::string_view{normalized}
                      : std::string_view{normalized}.substr(separator + 1);
  if (filename.empty() || filename == "." || filename == "..") {
    return "local-catalog-file";
  }
  constexpr std::size_t maximum_retained_filename = 255;
  if (filename.size() > maximum_retained_filename) {
    filename = filename.substr(filename.size() - maximum_retained_filename);
  }
  return std::string{filename} + "#" + path_fingerprint(normalized);
}

[[nodiscard]] std::string_view import_source_type_name(
    CatalogImportSourceType type) noexcept {
  switch (type) {
    case CatalogImportSourceType::local_file:
      return "local-file";
  }
  return "unknown";
}

[[nodiscard]] std::string_view import_status_name(
    CatalogImportStatus status) noexcept {
  switch (status) {
    case CatalogImportStatus::loaded:
      return "loaded";
    case CatalogImportStatus::not_found:
      return "not-found";
    case CatalogImportStatus::invalid:
      return "invalid";
    case CatalogImportStatus::failed:
      return "failed";
  }
  return "failed";
}

[[nodiscard]] std::string import_failure_detail(
    CatalogImportStatus status,
    CatalogImportSourceDescriptor const& source) {
  auto detail = "settings catalog import '" + source.redacted_path + "' ";
  switch (status) {
    case CatalogImportStatus::not_found:
      return detail + "was not found";
    case CatalogImportStatus::invalid:
      return detail + "is invalid";
    case CatalogImportStatus::failed:
      return detail + "could not be read";
    case CatalogImportStatus::loaded:
      return detail + "did not provide catalog content";
  }
  return detail + "could not be loaded";
}

[[nodiscard]] std::string_view confirm_status_name(
    ConfirmStatus status) noexcept {
  switch (status) {
    case ConfirmStatus::committed:
      return "committed";
    case ConfirmStatus::confirmation_required:
      return "confirmation-required";
    case ConfirmStatus::stale_preview:
      return "stale-preview";
    case ConfirmStatus::occupied:
      return "occupied";
    case ConfirmStatus::conflict:
      return "conflict";
    case ConfirmStatus::read_only:
      return "read-only";
    case ConfirmStatus::busy:
      return "busy";
    case ConfirmStatus::failed:
      return "failed";
    case ConfirmStatus::outcome_unknown:
      return "outcome-unknown";
  }
  return "failed";
}

}  // namespace

struct SettingsCatalogLifecycle::LoadedState final {
  CatalogStorageReadStatus storage_status{CatalogStorageReadStatus::failed};
  std::optional<catalog_domain::ValidatedSettingsCatalog> current;
  std::optional<catalog_domain::ValidatedSettingsCatalog> previous;
  std::vector<catalog_domain::CatalogIdentityTombstone> identity_tombstones;
  std::optional<domain::RevisionToken> revision;
  std::string detail;
};

SettingsCatalogLifecycle::SettingsCatalogLifecycle(
    SettingsCatalogStateStorage& storage,
    SettingsCatalogImportSource& imports, ExecutionLog& log,
    SharedOperationOccupancy& occupancy,
    SettingsCatalogImportAuthorization const& import_authorization,
    catalog_domain::SupportedCapabilities capabilities) noexcept
    : storage_(storage),
      imports_(imports),
      log_(log),
      occupancy_(occupancy),
      import_authorization_(import_authorization),
      capabilities_(std::move(capabilities)) {}

SettingsCatalogLifecycle::LoadedState SettingsCatalogLifecycle::load() {
  auto stored = storage_.read();
  LoadedState loaded{
      .storage_status = stored.status,
      .revision = std::move(stored.revision),
      .detail = std::move(stored.detail),
  };
  if (!stored.state.has_value()) {
    return loaded;
  }

  auto current = catalog_domain::validate(std::move(stored.state->current),
                                          capabilities_);
  if (!current.validated.has_value()) {
    loaded.storage_status = CatalogStorageReadStatus::read_only;
    loaded.detail = "persisted current settings catalog is invalid";
    return loaded;
  }
  loaded.current = std::move(current.validated);

  if (stored.state->previous.has_value()) {
    auto previous = catalog_domain::validate(
        std::move(*stored.state->previous), capabilities_);
    if (!previous.validated.has_value()) {
      loaded.storage_status = CatalogStorageReadStatus::read_only;
      loaded.detail = "persisted previous settings catalog is invalid";
      return loaded;
    }
    loaded.previous = std::move(previous.validated);
  }
  loaded.identity_tombstones = std::move(stored.state->identity_tombstones);
  loaded.identity_tombstones = catalog_domain::merge_identity_tombstones(
      std::move(loaded.identity_tombstones), loaded.current->catalog);
  if (loaded.previous.has_value()) {
    loaded.identity_tombstones = catalog_domain::merge_identity_tombstones(
        std::move(loaded.identity_tombstones), loaded.previous->catalog);
  }
  if (!catalog_domain::validate_transition(loaded.identity_tombstones,
                                           *loaded.current)
           .empty() ||
      (loaded.previous.has_value() &&
       !catalog_domain::validate_transition(loaded.identity_tombstones,
                                            *loaded.previous)
            .empty())) {
    loaded.storage_status = CatalogStorageReadStatus::read_only;
    loaded.detail = "persisted settings catalog identity history is invalid";
  }
  return loaded;
}

CatalogSnapshot SettingsCatalogLifecycle::snapshot() {
  auto loaded = load();
  return CatalogSnapshot{
      .status = map_snapshot_status(loaded.storage_status),
      .current = std::move(loaded.current),
      .previous = std::move(loaded.previous),
      .detail = std::move(loaded.detail),
  };
}

InitializeResult SettingsCatalogLifecycle::initialize_builtin(
    catalog_domain::SettingsCatalog catalog) {
  auto validation = catalog_domain::validate(std::move(catalog), capabilities_);
  if (!validation.validated.has_value()) {
    return {.status = InitializeStatus::rejected,
            .problems = std::move(validation.problems),
            .detail = "built-in settings catalog failed validation"};
  }
  return initialize_validated(std::move(*validation.validated));
}

InitializeResult SettingsCatalogLifecycle::initialize_validated(
    catalog_domain::ValidatedSettingsCatalog catalog) {
  auto stored = storage_.read();
  if (stored.status != CatalogStorageReadStatus::uninitialized) {
    if (stored.status == CatalogStorageReadStatus::writable) {
      return {.status = InitializeStatus::already_initialized};
    }
    auto status = InitializeStatus::failed;
    if (stored.status == CatalogStorageReadStatus::recovered_read_only ||
        stored.status == CatalogStorageReadStatus::read_only) {
      status = InitializeStatus::read_only;
    } else if (stored.status == CatalogStorageReadStatus::busy) {
      status = InitializeStatus::busy;
    }
    return {.status = status,
            .detail = std::move(stored.detail)};
  }

  auto correlation = log_.begin_correlation();
  log_event(correlation, "initialize", ExecutionResult::started,
            catalog.catalog.revision);
  auto lease = occupancy_.try_acquire(OperationIdentity{
      .kind = "settings-catalog",
      .operation_id = "initialize-settings-catalog",
      .correlation_id = correlation.value,
  });
  if (!acquired(lease)) {
    log_event(correlation, "initialize", ExecutionResult::failed,
              catalog.catalog.revision, lease.detail);
    return {.status = map_initialize_occupancy(lease.code),
            .detail = std::move(lease.detail)};
  }

  auto const revision = catalog.catalog.revision;
  auto written = storage_.write(
      std::nullopt,
      SettingsCatalogState{
          .current = catalog.catalog,
          .identity_tombstones =
              catalog_domain::identity_tombstones(catalog.catalog)});
  auto released = occupancy_.release(*lease.lease);
  auto status = map_initialize_write(written.status);
  auto detail = std::move(written.detail);
  if (released.code != OccupancyResultCode::released) {
    if (!detail.empty()) {
      detail += "; ";
    }
    detail += "settings catalog occupancy release failed: " + released.detail;
  }
  log_event(correlation, "initialize",
            status == InitializeStatus::initialized
                ? ExecutionResult::succeeded
                : ExecutionResult::failed,
            revision, detail.empty() ? std::nullopt
                                     : std::optional<std::string>{detail});
  return {.status = status, .detail = std::move(detail)};
}

PrepareResult SettingsCatalogLifecycle::prepare_update(
    catalog_domain::SettingsCatalog candidate) {
  return prepare_candidate(std::move(candidate), CatalogChangeOrigin::update,
                           std::nullopt, std::nullopt);
}

PrepareResult SettingsCatalogLifecycle::prepare_debug_import(
    std::string path) {
  if (!import_authorization_.debug_import_allowed()) {
    return {.status = PrepareStatus::debug_mode_required,
            .detail =
                "manual settings catalog import requires debug mode"};
  }
  auto correlation = log_.begin_correlation();
  auto imported = imports_.read_import(path);
  CatalogImportSourceDescriptor source{
      .type = imported.source_type,
      .redacted_path = redacted_import_path(
          imported.source_path.empty() ? path : imported.source_path),
  };
  log_import_event(
      correlation, "debug-import-load",
      imported.status == CatalogImportStatus::loaded &&
              imported.catalog.has_value()
          ? ExecutionResult::succeeded
          : ExecutionResult::failed,
      source, std::string{import_status_name(imported.status)});
  if (imported.status != CatalogImportStatus::loaded ||
      !imported.catalog.has_value()) {
    return {.status = PrepareStatus::rejected,
            .import_source = source,
            .detail = import_failure_detail(imported.status, source)};
  }
  return prepare_candidate(std::move(*imported.catalog),
                           CatalogChangeOrigin::debug_import,
                           std::move(source), std::move(correlation));
}

PrepareResult SettingsCatalogLifecycle::prepare_rollback() {
  auto loaded = load();
  auto status = map_prepare_status(loaded.storage_status);
  if (status != PrepareStatus::ready || !loaded.current.has_value() ||
      !loaded.revision.has_value()) {
    return {.status = status, .detail = std::move(loaded.detail)};
  }
  if (!loaded.previous.has_value()) {
    return {.status = PrepareStatus::no_previous,
            .detail = "no previous usable settings catalog is available"};
  }

  auto correlation = log_.begin_correlation();
  auto preview = catalog_domain::preview_changes(*loaded.current,
                                                  *loaded.previous);
  PreparedCatalogChange prepared{
      .confirmation_token = "settings-catalog-preview-" +
                            std::to_string(next_confirmation_++),
      .origin = CatalogChangeOrigin::rollback,
      .changes = std::move(preview),
      .setting_count = loaded.previous->catalog.settings.size(),
      .plan_count = loaded.previous->catalog.plans.size(),
  };
  auto const candidate_revision = prepared.changes.candidate_revision;
  pending_ = PendingChange{
      .prepared = prepared,
      .next_state = {.current = loaded.previous->catalog,
                     .previous = loaded.current->catalog,
                     .identity_tombstones = loaded.identity_tombstones},
      .expected_revision = *loaded.revision,
      .correlation = correlation,
  };
  log_event(correlation, "preview-rollback", ExecutionResult::succeeded,
            candidate_revision);
  return {.status = PrepareStatus::ready, .prepared = std::move(prepared)};
}

PrepareResult SettingsCatalogLifecycle::prepare_candidate(
    catalog_domain::SettingsCatalog candidate, CatalogChangeOrigin origin,
    std::optional<CatalogImportSourceDescriptor> import_source,
    std::optional<CorrelationId> correlation) {
  auto loaded = load();
  auto status = map_prepare_status(loaded.storage_status);
  if (status != PrepareStatus::ready || !loaded.current.has_value() ||
      !loaded.revision.has_value()) {
    if (import_source.has_value() && correlation.has_value()) {
      log_import_event(
          *correlation, "debug-import-validation", ExecutionResult::failed,
          *import_source, "rejected",
          "current settings catalog state is unavailable for import");
    }
    return {.status = status,
            .import_source = std::move(import_source),
            .detail =
                "settings catalog import cannot proceed because current state is unavailable"};
  }

  auto validation = catalog_domain::validate(std::move(candidate),
                                             capabilities_);
  if (!validation.validated.has_value()) {
    if (import_source.has_value() && correlation.has_value()) {
      log_import_event(
          *correlation, "debug-import-validation", ExecutionResult::failed,
          *import_source, "rejected",
          "candidate settings catalog failed validation");
    }
    return {.status = PrepareStatus::rejected,
            .import_source = import_source,
            .problems = std::move(validation.problems),
            .detail = "candidate settings catalog failed validation"};
  }
  auto transition = catalog_domain::validate_transition(
      loaded.identity_tombstones, *validation.validated);
  if (!transition.empty()) {
    if (import_source.has_value() && correlation.has_value()) {
      log_import_event(
          *correlation, "debug-import-validation", ExecutionResult::failed,
          *import_source, "rejected",
          "candidate reuses a stable settings catalog identifier");
    }
    return {.status = PrepareStatus::rejected,
            .import_source = import_source,
            .problems = std::move(transition),
            .detail =
                "candidate changes or reuses immutable catalog semantics"};
  }

  auto const current_revision = loaded.current->catalog.revision;
  auto const candidate_revision = validation.validated->catalog.revision;
  if (validation.validated->catalog == loaded.current->catalog) {
    if (import_source.has_value() && correlation.has_value()) {
      log_import_event(*correlation, "debug-import-validation",
                       ExecutionResult::succeeded, *import_source,
                       "no-change");
    }
    return {.status = PrepareStatus::no_change,
            .import_source = import_source,
            .detail = "candidate settings catalog is already active"};
  }
  if (candidate_revision == current_revision) {
    if (import_source.has_value() && correlation.has_value()) {
      log_import_event(
          *correlation, "debug-import-validation", ExecutionResult::failed,
          *import_source, "rejected",
          "one settings catalog revision cannot identify different content");
    }
    return {.status = PrepareStatus::rejected,
            .import_source = import_source,
            .detail =
                "one settings catalog revision cannot identify different content"};
  }
  if (candidate_revision < current_revision &&
      origin == CatalogChangeOrigin::update) {
    return {.status = PrepareStatus::downgrade_requires_debug_import,
            .import_source = import_source,
            .detail =
                "a lower settings catalog revision requires debug import or rollback"};
  }

  if (import_source.has_value() && correlation.has_value()) {
    log_import_event(*correlation, "debug-import-validation",
                     ExecutionResult::succeeded, *import_source, "accepted");
  }

  auto change_correlation = correlation.has_value()
                                ? std::move(*correlation)
                                : log_.begin_correlation();
  auto preview = catalog_domain::preview_changes(*loaded.current,
                                                  *validation.validated);
  PreparedCatalogChange prepared{
      .confirmation_token = "settings-catalog-preview-" +
                            std::to_string(next_confirmation_++),
      .origin = origin,
      .changes = std::move(preview),
      .import_source = import_source,
      .setting_count = validation.validated->catalog.settings.size(),
      .plan_count = validation.validated->catalog.plans.size(),
  };
  pending_ = PendingChange{
      .prepared = prepared,
      .next_state = {.current = validation.validated->catalog,
                     .previous = loaded.current->catalog,
                     .identity_tombstones =
                         catalog_domain::merge_identity_tombstones(
                             loaded.identity_tombstones,
                             validation.validated->catalog)},
      .expected_revision = *loaded.revision,
      .correlation = change_correlation,
  };
  log_event(change_correlation,
            origin == CatalogChangeOrigin::debug_import
                ? "preview-debug-import"
                : "preview-update",
            ExecutionResult::succeeded, candidate_revision);
  return {.status = PrepareStatus::ready,
          .prepared = std::move(prepared),
          .import_source = std::move(import_source)};
}

ConfirmResult SettingsCatalogLifecycle::confirm(
    std::string_view confirmation_token) {
  if (!pending_.has_value() || confirmation_token.empty()) {
    return {.status = ConfirmStatus::confirmation_required,
            .detail = "a prepared catalog preview must be confirmed"};
  }
  if (pending_->prepared.confirmation_token != confirmation_token) {
    if (pending_->prepared.import_source.has_value()) {
      log_import_event(pending_->correlation, "debug-import-confirm",
                       ExecutionResult::failed,
                       *pending_->prepared.import_source, "stale-preview",
                       "settings catalog confirmation token is stale");
    }
    return {.status = ConfirmStatus::stale_preview,
            .import_source = pending_->prepared.import_source,
            .detail = "catalog confirmation token does not match the preview"};
  }

  auto const import_source = pending_->prepared.import_source;
  auto const candidate_revision =
      pending_->prepared.changes.candidate_revision;
  if (pending_->prepared.import_source.has_value()) {
    log_import_event(pending_->correlation, "debug-import-confirm",
                     ExecutionResult::started,
                     *pending_->prepared.import_source,
                     "confirmation-started");
  }
  log_event(pending_->correlation, "commit", ExecutionResult::started,
            candidate_revision);
  auto lease = occupancy_.try_acquire(OperationIdentity{
      .kind = "settings-catalog",
      .operation_id = pending_->prepared.confirmation_token,
      .correlation_id = pending_->correlation.value,
  });
  if (!acquired(lease)) {
    auto const import_confirmation =
        pending_->prepared.import_source.has_value();
    log_event(pending_->correlation, "commit", ExecutionResult::failed,
              candidate_revision,
              import_confirmation
                  ? std::optional<std::string>{
                        "settings catalog import confirmation could not acquire occupancy"}
                  : std::optional<std::string>{lease.detail});
    if (pending_->prepared.import_source.has_value()) {
      auto const status = map_confirm_occupancy(lease.code);
      log_import_event(pending_->correlation, "debug-import-confirm",
                       ExecutionResult::failed,
                       *pending_->prepared.import_source,
                       std::string{confirm_status_name(status)},
                       "settings catalog confirmation could not acquire occupancy");
    }
    return {.status = map_confirm_occupancy(lease.code),
            .import_source = import_source,
            .detail = import_confirmation
                          ? "settings catalog import confirmation could not acquire occupancy"
                          : std::move(lease.detail)};
  }

  auto written = storage_.write(pending_->expected_revision,
                                pending_->next_state);
  auto released = occupancy_.release(*lease.lease);
  auto status = map_confirm_write(written.status);
  auto detail = std::move(written.detail);
  if (released.code != OccupancyResultCode::released) {
    if (!detail.empty()) {
      detail += "; ";
    }
    detail += "settings catalog occupancy release failed: " + released.detail;
  }
  log_event(pending_->correlation, "commit",
            status == ConfirmStatus::committed ? ExecutionResult::succeeded
                                               : ExecutionResult::failed,
            candidate_revision,
            detail.empty()
                ? std::nullopt
                : import_source.has_value() &&
                          status != ConfirmStatus::committed
                      ? std::optional<std::string>{
                            "settings catalog import confirmation did not commit"}
                      : std::optional<std::string>{detail});
  if (pending_->prepared.import_source.has_value()) {
    log_import_event(
        pending_->correlation, "debug-import-confirm",
        status == ConfirmStatus::committed ? ExecutionResult::succeeded
                                           : ExecutionResult::failed,
        *pending_->prepared.import_source,
        std::string{confirm_status_name(status)},
        status == ConfirmStatus::committed
            ? std::nullopt
            : std::optional<std::string>{
                  "settings catalog import confirmation did not commit"});
  }
  if (status == ConfirmStatus::committed ||
      status == ConfirmStatus::conflict) {
    pending_.reset();
  }
  auto returned_detail = std::move(detail);
  if (import_source.has_value() && status != ConfirmStatus::committed) {
    returned_detail = "settings catalog import confirmation did not commit";
  }
  return {.status = status,
          .active_revision = status == ConfirmStatus::committed
                                 ? candidate_revision
                                 : 0,
          .import_source = import_source,
          .detail = std::move(returned_detail)};
}

void SettingsCatalogLifecycle::log_event(
    CorrelationId const& correlation, std::string stage,
    ExecutionResult result, std::uint64_t revision,
    std::optional<std::string> error) {
  ExecutionEvent event{
      .kind = ExecutionEventKind::state_transition,
      .component = "settings-catalog",
      .stage = std::move(stage),
      .result = result,
      .fields = {{.key = "catalog_revision",
                  .value = std::to_string(revision),
                  .disposition = DiagnosticValueDisposition::retain}},
  };
  if (error.has_value() && !error->empty()) {
    event.error = ExecutionError{
        .source = "settings-catalog",
        .message = std::move(*error),
    };
  }
  static_cast<void>(log_.append(correlation, event));
}

void SettingsCatalogLifecycle::log_import_event(
    CorrelationId const& correlation, std::string stage,
    ExecutionResult result, CatalogImportSourceDescriptor const& source,
    std::string import_result, std::optional<std::string> error) {
  ExecutionEvent event{
      .kind = ExecutionEventKind::state_transition,
      .component = "settings-catalog",
      .stage = std::move(stage),
      .result = result,
      .fields = {
          {.key = "source_type",
           .value = std::string{import_source_type_name(source.type)},
           .disposition = DiagnosticValueDisposition::retain},
          {.key = "source_path",
           .value = source.redacted_path,
           .disposition = DiagnosticValueDisposition::retain},
          {.key = "import_result",
           .value = std::move(import_result),
           .disposition = DiagnosticValueDisposition::retain},
      },
  };
  if (error.has_value()) {
    event.error = ExecutionError{
        .source = "settings-catalog",
        .message = std::move(*error),
    };
  }
  static_cast<void>(log_.append(correlation, event));
}

}  // namespace azzs::application::settings_catalog
