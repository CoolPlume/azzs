#include "azzs/settings_catalog/settings_catalog_lifecycle.hpp"

#include <optional>
#include <string>
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

}  // namespace

struct SettingsCatalogLifecycle::LoadedState final {
  CatalogStorageReadStatus storage_status{CatalogStorageReadStatus::failed};
  std::optional<catalog_domain::ValidatedSettingsCatalog> current;
  std::optional<catalog_domain::ValidatedSettingsCatalog> previous;
  std::optional<domain::RevisionToken> revision;
  std::string detail;
};

SettingsCatalogLifecycle::SettingsCatalogLifecycle(
    SettingsCatalogStateStorage& storage,
    SettingsCatalogImportSource& imports, ExecutionLog& log,
    SharedOperationOccupancy& occupancy,
    catalog_domain::SupportedCapabilities capabilities) noexcept
    : storage_(storage),
      imports_(imports),
      log_(log),
      occupancy_(occupancy),
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
      SettingsCatalogState{.current = std::move(catalog.catalog)});
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
                           std::nullopt);
}

PrepareResult SettingsCatalogLifecycle::prepare_debug_import(
    std::string path, bool debug_mode_enabled) {
  if (!debug_mode_enabled) {
    return {.status = PrepareStatus::debug_mode_required,
            .detail =
                "manual settings catalog import requires debug mode"};
  }
  auto imported = imports_.read_import(path);
  if (imported.status != CatalogImportStatus::loaded ||
      !imported.catalog.has_value()) {
    return {.status = PrepareStatus::rejected,
            .detail = imported.detail.empty()
                          ? "settings catalog import could not be loaded"
                          : std::move(imported.detail)};
  }
  return prepare_candidate(std::move(*imported.catalog),
                           CatalogChangeOrigin::debug_import,
                           std::move(imported.source_path));
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
                     .previous = loaded.current->catalog},
      .expected_revision = *loaded.revision,
      .correlation = correlation,
  };
  log_event(correlation, "preview-rollback", ExecutionResult::succeeded,
            candidate_revision);
  return {.status = PrepareStatus::ready, .prepared = std::move(prepared)};
}

PrepareResult SettingsCatalogLifecycle::prepare_candidate(
    catalog_domain::SettingsCatalog candidate, CatalogChangeOrigin origin,
    std::optional<std::string> source_path) {
  auto loaded = load();
  auto status = map_prepare_status(loaded.storage_status);
  if (status != PrepareStatus::ready || !loaded.current.has_value() ||
      !loaded.revision.has_value()) {
    return {.status = status, .detail = std::move(loaded.detail)};
  }

  auto validation = catalog_domain::validate(std::move(candidate),
                                             capabilities_);
  if (!validation.validated.has_value()) {
    return {.status = PrepareStatus::rejected,
            .problems = std::move(validation.problems),
            .detail = "candidate settings catalog failed validation"};
  }
  auto transition = catalog_domain::validate_transition(
      *loaded.current, *validation.validated);
  if (!transition.empty()) {
    return {.status = PrepareStatus::rejected,
            .problems = std::move(transition),
            .detail = "candidate reuses a stable identifier for another target"};
  }

  auto const current_revision = loaded.current->catalog.revision;
  auto const candidate_revision = validation.validated->catalog.revision;
  if (validation.validated->catalog == loaded.current->catalog) {
    return {.status = PrepareStatus::no_change,
            .detail = "candidate settings catalog is already active"};
  }
  if (candidate_revision == current_revision) {
    return {.status = PrepareStatus::rejected,
            .detail =
                "one settings catalog revision cannot identify different content"};
  }
  if (candidate_revision < current_revision &&
      origin == CatalogChangeOrigin::update) {
    return {.status = PrepareStatus::downgrade_requires_debug_import,
            .detail =
                "a lower settings catalog revision requires debug import or rollback"};
  }

  auto correlation = log_.begin_correlation();
  auto preview = catalog_domain::preview_changes(*loaded.current,
                                                  *validation.validated);
  PreparedCatalogChange prepared{
      .confirmation_token = "settings-catalog-preview-" +
                            std::to_string(next_confirmation_++),
      .origin = origin,
      .changes = std::move(preview),
      .source_path = std::move(source_path),
      .setting_count = validation.validated->catalog.settings.size(),
      .plan_count = validation.validated->catalog.plans.size(),
  };
  pending_ = PendingChange{
      .prepared = prepared,
      .next_state = {.current = validation.validated->catalog,
                     .previous = loaded.current->catalog},
      .expected_revision = *loaded.revision,
      .correlation = correlation,
  };
  log_event(correlation,
            origin == CatalogChangeOrigin::debug_import
                ? "preview-debug-import"
                : "preview-update",
            ExecutionResult::succeeded, candidate_revision);
  return {.status = PrepareStatus::ready, .prepared = std::move(prepared)};
}

ConfirmResult SettingsCatalogLifecycle::confirm(
    std::string_view confirmation_token) {
  if (!pending_.has_value() || confirmation_token.empty()) {
    return {.status = ConfirmStatus::confirmation_required,
            .detail = "a prepared catalog preview must be confirmed"};
  }
  if (pending_->prepared.confirmation_token != confirmation_token) {
    return {.status = ConfirmStatus::stale_preview,
            .detail = "catalog confirmation token does not match the preview"};
  }

  auto const candidate_revision =
      pending_->prepared.changes.candidate_revision;
  log_event(pending_->correlation, "commit", ExecutionResult::started,
            candidate_revision);
  auto lease = occupancy_.try_acquire(OperationIdentity{
      .kind = "settings-catalog",
      .operation_id = pending_->prepared.confirmation_token,
      .correlation_id = pending_->correlation.value,
  });
  if (!acquired(lease)) {
    log_event(pending_->correlation, "commit", ExecutionResult::failed,
              candidate_revision, lease.detail);
    return {.status = map_confirm_occupancy(lease.code),
            .detail = std::move(lease.detail)};
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
            detail.empty() ? std::nullopt
                           : std::optional<std::string>{detail});
  if (status == ConfirmStatus::committed ||
      status == ConfirmStatus::conflict) {
    pending_.reset();
  }
  return {.status = status,
          .active_revision = status == ConfirmStatus::committed
                                 ? candidate_revision
                                 : 0,
          .detail = std::move(detail)};
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

}  // namespace azzs::application::settings_catalog
