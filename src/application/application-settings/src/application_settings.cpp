#include "azzs/application/application_settings.hpp"

#include <string>
#include <utility>
#include <vector>

namespace azzs::application {
namespace {

[[nodiscard]] ApplicationSettingsActionCode settings_prepare_code(
    settings_catalog::PrepareStatus status) noexcept {
  switch (status) {
    case settings_catalog::PrepareStatus::ready:
      return ApplicationSettingsActionCode::confirmation_required;
    case settings_catalog::PrepareStatus::no_change:
    case settings_catalog::PrepareStatus::no_previous:
      return ApplicationSettingsActionCode::no_change;
    case settings_catalog::PrepareStatus::unavailable:
      return ApplicationSettingsActionCode::unavailable;
    case settings_catalog::PrepareStatus::debug_mode_required:
    case settings_catalog::PrepareStatus::downgrade_requires_debug_import:
      return ApplicationSettingsActionCode::protected_operation;
    case settings_catalog::PrepareStatus::rejected:
      return ApplicationSettingsActionCode::rejected;
    case settings_catalog::PrepareStatus::read_only:
    case settings_catalog::PrepareStatus::busy:
    case settings_catalog::PrepareStatus::failed:
      return ApplicationSettingsActionCode::failed;
  }
  return ApplicationSettingsActionCode::failed;
}

[[nodiscard]] ApplicationSettingsActionCode settings_confirm_code(
    settings_catalog::ConfirmStatus status) noexcept {
  switch (status) {
    case settings_catalog::ConfirmStatus::committed:
      return ApplicationSettingsActionCode::completed;
    case settings_catalog::ConfirmStatus::confirmation_required:
      return ApplicationSettingsActionCode::confirmation_required;
    case settings_catalog::ConfirmStatus::stale_preview:
      return ApplicationSettingsActionCode::rejected;
    case settings_catalog::ConfirmStatus::occupied:
    case settings_catalog::ConfirmStatus::conflict:
    case settings_catalog::ConfirmStatus::read_only:
    case settings_catalog::ConfirmStatus::busy:
    case settings_catalog::ConfirmStatus::failed:
    case settings_catalog::ConfirmStatus::outcome_unknown:
      return ApplicationSettingsActionCode::failed;
  }
  return ApplicationSettingsActionCode::failed;
}

[[nodiscard]] bool software_optimization_succeeded(
    SoftwareOptimizationCatalogLifecycleCode code) noexcept {
  switch (code) {
    case SoftwareOptimizationCatalogLifecycleCode::applied:
    case SoftwareOptimizationCatalogLifecycleCode::downgraded:
    case SoftwareOptimizationCatalogLifecycleCode::rolled_back:
    case SoftwareOptimizationCatalogLifecycleCode::unchanged:
      return true;
    case SoftwareOptimizationCatalogLifecycleCode::preview_ready:
    case SoftwareOptimizationCatalogLifecycleCode::rejected:
    case SoftwareOptimizationCatalogLifecycleCode::debug_mode_required:
    case SoftwareOptimizationCatalogLifecycleCode::confirmation_required:
    case SoftwareOptimizationCatalogLifecycleCode::preview_stale:
    case SoftwareOptimizationCatalogLifecycleCode::occupied:
    case SoftwareOptimizationCatalogLifecycleCode::read_only:
    case SoftwareOptimizationCatalogLifecycleCode::file_failed:
    case SoftwareOptimizationCatalogLifecycleCode::persistence_failed:
    case SoftwareOptimizationCatalogLifecycleCode::logging_failed:
    case SoftwareOptimizationCatalogLifecycleCode::no_previous:
      return false;
  }
  return false;
}

[[nodiscard]] ApplicationSettingsActionCode
software_optimization_result_code(SoftwareOptimizationCatalogLifecycleCode code)
    noexcept {
  if (software_optimization_succeeded(code)) {
    return code == SoftwareOptimizationCatalogLifecycleCode::unchanged
               ? ApplicationSettingsActionCode::no_change
               : ApplicationSettingsActionCode::completed;
  }
  switch (code) {
    case SoftwareOptimizationCatalogLifecycleCode::no_previous:
      return ApplicationSettingsActionCode::no_change;
    case SoftwareOptimizationCatalogLifecycleCode::debug_mode_required:
      return ApplicationSettingsActionCode::protected_operation;
    case SoftwareOptimizationCatalogLifecycleCode::confirmation_required:
      return ApplicationSettingsActionCode::confirmation_required;
    case SoftwareOptimizationCatalogLifecycleCode::rejected:
    case SoftwareOptimizationCatalogLifecycleCode::preview_stale:
      return ApplicationSettingsActionCode::rejected;
    case SoftwareOptimizationCatalogLifecycleCode::preview_ready:
    case SoftwareOptimizationCatalogLifecycleCode::occupied:
    case SoftwareOptimizationCatalogLifecycleCode::read_only:
    case SoftwareOptimizationCatalogLifecycleCode::file_failed:
    case SoftwareOptimizationCatalogLifecycleCode::persistence_failed:
    case SoftwareOptimizationCatalogLifecycleCode::logging_failed:
      return ApplicationSettingsActionCode::failed;
    case SoftwareOptimizationCatalogLifecycleCode::applied:
    case SoftwareOptimizationCatalogLifecycleCode::downgraded:
    case SoftwareOptimizationCatalogLifecycleCode::rolled_back:
    case SoftwareOptimizationCatalogLifecycleCode::unchanged:
      return ApplicationSettingsActionCode::failed;
  }
  return ApplicationSettingsActionCode::failed;
}

[[nodiscard]] std::string nonempty_detail(std::string detail,
                                          std::string_view fallback) {
  return detail.empty() ? std::string{fallback} : std::move(detail);
}

}  // namespace

ArchitecturePreferences::ArchitecturePreferences(
    std::shared_ptr<ArchitecturePreferenceStore> store)
    : store_(std::move(store)) {
  if (!store_) {
    return;
  }
  auto const read = store_->read_architecture_preference();
  if (read.status == ArchitecturePreferenceReadStatus::loaded) {
    preference_ = read.preference;
  }
}

domain::architecture_selection::ArchitecturePreference
ArchitecturePreferences::preference() const noexcept {
  return preference_;
}

domain::architecture_selection::ArchitecturePreference
ArchitecturePreferences::set_preference(
    domain::architecture_selection::ArchitecturePreference preference) {
  if (!store_ ||
      store_->write_architecture_preference(preference) !=
          ArchitecturePreferenceWriteStatus::saved) {
    return preference_;
  }
  preference_ = preference;
  return preference_;
}

CacheRetentionPreferences::CacheRetentionPreferences(
    std::shared_ptr<CacheRetentionPreferenceStore> store)
    : store_(std::move(store)) {
  if (!store_) {
    return;
  }
  auto const read = store_->read_cache_retention();
  if (read.status == CacheRetentionPreferenceReadStatus::loaded) {
    retention_ = read.retention;
  }
}

domain::offline_package_cache::CacheRetentionPolicy
CacheRetentionPreferences::retention() const noexcept {
  return retention_;
}

domain::offline_package_cache::CacheRetentionPolicy
CacheRetentionPreferences::set_retention(
    domain::offline_package_cache::CacheRetentionPolicy retention) {
  if (!store_ ||
      store_->write_cache_retention(retention) !=
          CacheRetentionPreferenceWriteStatus::saved) {
    return retention_;
  }
  retention_ = retention;
  return retention_;
}

ApplicationSettingsService::ApplicationSettingsService(
    architecture_selection::ArchitectureSelectionLifecycle&
        architecture_selection,
    ArchitecturePreferences& architecture_preferences,
    offline_package_cache::OfflinePackageCacheService& package_cache,
    offline_package_cache::OfflinePackageCacheService& batch_package_cache,
    CacheRetentionPreferences& cache_retention_preferences,
    HistoryAndLogsService& history_and_logs,
    SystemSettingsApplyService& system_settings,
    software_catalog::SoftwareCatalogLifecycle& software_catalog,
    settings_catalog::SettingsCatalogLifecycle& settings_catalog,
    SoftwareOptimizationCatalogLifecycle& software_optimization_catalog,
    software_selection::SoftwareSelectionLifecycle& software_selection,
    software_optimization_discovery::SoftwareOptimizationDiscoveryService&
        software_optimization_discovery,
    TrustedSettingsCatalogUpdateSource* settings_catalog_updates,
    TrustedSoftwareOptimizationCatalogUpdateSource*
        software_optimization_catalog_updates,
    ApplicationSettingsDebugProvider* debug) noexcept
    : architecture_selection_(architecture_selection),
      architecture_preferences_(architecture_preferences),
      package_cache_(package_cache),
      batch_package_cache_(batch_package_cache),
      cache_retention_preferences_(cache_retention_preferences),
      history_and_logs_(history_and_logs),
      system_settings_(system_settings),
      software_catalog_(software_catalog),
      settings_catalog_(settings_catalog),
      software_optimization_catalog_(software_optimization_catalog),
      software_selection_(software_selection),
      software_optimization_discovery_(software_optimization_discovery),
      settings_catalog_updates_(settings_catalog_updates),
      software_optimization_catalog_updates_(software_optimization_catalog_updates),
      debug_(debug) {}

ApplicationSettingsSnapshot ApplicationSettingsService::snapshot() {
  ApplicationSettingsSnapshot current;
  current.architecture_preference = architecture_preferences_.preference();
  current.cache = package_cache_.snapshot();
  current.history_and_logs = history_and_logs_.refresh();
  current.system_settings = system_settings_.snapshot();
  current.recovery_records = system_settings_.recovery_records();
  current.software_catalog = software_catalog_.snapshot();
  current.settings_catalog = settings_catalog_.snapshot();
  current.software_optimization_catalog =
      software_optimization_catalog_.snapshot();
  current.debug = debug_ ? debug_->snapshot()
                         : ApplicationSettingsDebugSnapshot{
                               .detail = "the debug settings provider is unavailable"};
  return current;
}

ApplicationSettingsActionResult
ApplicationSettingsService::set_architecture_preference(
    domain::architecture_selection::ArchitecturePreference preference) {
  auto const persisted = architecture_preferences_.set_preference(preference);
  if (persisted != preference) {
    return result(ApplicationSettingsActionCode::failed,
                  "the architecture preference could not be saved");
  }
  architecture_selection_.set_preference(persisted);
  return result(ApplicationSettingsActionCode::completed);
}

ApplicationSettingsActionResult ApplicationSettingsService::set_cache_retention(
    domain::offline_package_cache::CacheRetentionPolicy retention) {
  auto const persisted = cache_retention_preferences_.set_retention(retention);
  if (persisted != retention) {
    return result(ApplicationSettingsActionCode::failed,
                  "the cache retention preference could not be saved");
  }
  package_cache_.set_retention(persisted);
  batch_package_cache_.set_retention(persisted);
  return result(ApplicationSettingsActionCode::completed);
}

ApplicationSettingsActionResult ApplicationSettingsService::clear_cache(
    bool confirmed) {
  if (requires_explicit_confirmation(
          ApplicationSettingsConfirmationAction::clear_cache) &&
      !confirmed) {
    return result(ApplicationSettingsActionCode::confirmation_required,
                  "clearing completed cache entries requires confirmation");
  }
  auto const cleaned = package_cache_.clear_completed();
  switch (cleaned.code) {
    case offline_package_cache::CacheCleanupCode::completed:
      return result(cleaned.detail.empty()
                        ? ApplicationSettingsActionCode::completed
                        : ApplicationSettingsActionCode::failed,
                    cleaned.detail);
    case offline_package_cache::CacheCleanupCode::rejected_after_shutdown:
      return result(ApplicationSettingsActionCode::rejected, cleaned.detail);
    case offline_package_cache::CacheCleanupCode::failed:
      return result(ApplicationSettingsActionCode::failed, cleaned.detail);
  }
  return result(ApplicationSettingsActionCode::failed, cleaned.detail);
}

ApplicationSettingsActionResult ApplicationSettingsService::clear_logs(
    bool confirmed) {
  if (requires_explicit_confirmation(
          ApplicationSettingsConfirmationAction::clear_logs) &&
      !confirmed) {
    return result(ApplicationSettingsActionCode::confirmation_required,
                  "clearing execution logs requires confirmation");
  }
  auto const cleared = history_and_logs_.clear_logs();
  return result(cleared.code == HistoryAndLogsActionCode::succeeded
                    ? ApplicationSettingsActionCode::completed
                    : ApplicationSettingsActionCode::failed,
                cleared.message);
}

ApplicationSettingsActionResult ApplicationSettingsService::export_diagnostic() {
  auto const exported = history_and_logs_.export_diagnostic();
  return result(exported.code == HistoryAndLogsActionCode::succeeded
                    ? ApplicationSettingsActionCode::completed
                    : ApplicationSettingsActionCode::failed,
                exported.message);
}

ApplicationSettingsActionResult
ApplicationSettingsService::delete_recovery_record(std::uint64_t record_id,
                                                    bool confirmed) {
  auto const deleted = system_settings_.delete_recovery_record(
      record_id,
      !requires_explicit_confirmation(
          ApplicationSettingsConfirmationAction::delete_recovery_record) ||
          confirmed);
  switch (deleted.status) {
    case RecoveryRecordDeleteStatus::deleted:
      return result(ApplicationSettingsActionCode::completed, deleted.detail);
    case RecoveryRecordDeleteStatus::confirmation_required:
      return result(ApplicationSettingsActionCode::confirmation_required,
                    deleted.detail);
    case RecoveryRecordDeleteStatus::blocked:
      return result(ApplicationSettingsActionCode::protected_operation,
                    deleted.detail);
    case RecoveryRecordDeleteStatus::not_found:
      return result(ApplicationSettingsActionCode::no_change, deleted.detail);
    case RecoveryRecordDeleteStatus::failed:
      return result(ApplicationSettingsActionCode::failed, deleted.detail);
  }
  return result(ApplicationSettingsActionCode::failed, deleted.detail);
}

ApplicationSettingsActionResult ApplicationSettingsService::set_debug_enabled(
    bool enabled) {
  if (debug_ == nullptr) {
    return result(ApplicationSettingsActionCode::unavailable,
                  "the debug settings provider is unavailable");
  }
  auto updated = debug_->set_enabled(enabled);
  switch (updated.code) {
    case ApplicationSettingsDebugActionCode::updated:
      return result(ApplicationSettingsActionCode::completed, updated.detail);
    case ApplicationSettingsDebugActionCode::unavailable:
      return result(ApplicationSettingsActionCode::unavailable, updated.detail);
    case ApplicationSettingsDebugActionCode::rejected:
      return result(ApplicationSettingsActionCode::rejected, updated.detail);
  }
  return result(ApplicationSettingsActionCode::failed, updated.detail);
}

ApplicationSettingsActionResult ApplicationSettingsService::prepare_catalog_change(
    ApplicationSettingsCatalog catalog, ApplicationSettingsCatalogAction action) {
  static_assert(requires_explicit_confirmation(
      ApplicationSettingsConfirmationAction::catalog_change));
  pending_catalog_change_.reset();

  auto change = ApplicationSettingsCatalogChange{
      .catalog = catalog,
      .action = action,
      .confirmation_token =
          "application-settings-catalog-" + std::to_string(next_confirmation_++),
  };

  if (catalog == ApplicationSettingsCatalog::software_and_drivers) {
    auto preview = action == ApplicationSettingsCatalogAction::update
                       ? software_catalog_.preview_update()
                       : software_catalog_.preview_rollback();
    if (!preview.ready) {
      return result(ApplicationSettingsActionCode::failed,
                    nonempty_detail(std::move(preview.error),
                                    "the software and driver catalog change is unavailable"));
    }
    change.detail = "the software and driver catalog change is ready for confirmation";
    pending_catalog_change_ = PendingCatalogChange{
        .kind = PendingCatalogKind::software_catalog,
        .change = change,
        .owner_token = std::move(preview.confirmation_token),
    };
    return result(ApplicationSettingsActionCode::confirmation_required,
                  change.detail, std::move(change));
  }

  if (catalog == ApplicationSettingsCatalog::system_settings) {
    settings_catalog::PrepareResult prepared;
    if (action == ApplicationSettingsCatalogAction::update) {
      if (settings_catalog_updates_ == nullptr) {
        return result(ApplicationSettingsActionCode::unavailable,
                      "the trusted system settings catalog update source is unavailable");
      }
      auto update = settings_catalog_updates_->read_update();
      if (!update.candidate.has_value()) {
        return result(ApplicationSettingsActionCode::unavailable,
                      nonempty_detail(std::move(update.detail),
                                      "the trusted system settings catalog update is unavailable"));
      }
      prepared = settings_catalog_.prepare_update(std::move(*update.candidate));
    } else {
      prepared = settings_catalog_.prepare_rollback();
    }
    auto const code = settings_prepare_code(prepared.status);
    if (code != ApplicationSettingsActionCode::confirmation_required ||
        !prepared.prepared.has_value()) {
      return result(code, prepared.detail);
    }
    change.detail = "the system settings catalog change is ready for confirmation";
    pending_catalog_change_ = PendingCatalogChange{
        .kind = PendingCatalogKind::settings_catalog,
        .change = change,
        .owner_token = std::move(prepared.prepared->confirmation_token),
    };
    return result(ApplicationSettingsActionCode::confirmation_required,
                  change.detail, std::move(change));
  }

  if (action == ApplicationSettingsCatalogAction::update) {
    if (software_optimization_catalog_updates_ == nullptr) {
      return result(ApplicationSettingsActionCode::unavailable,
                    "the trusted software optimization catalog update source is unavailable");
    }
    auto update = software_optimization_catalog_updates_->read_update();
    if (!update.update.has_value()) {
      return result(ApplicationSettingsActionCode::unavailable,
                    nonempty_detail(std::move(update.detail),
                                    "the trusted software optimization catalog update is unavailable"));
    }
    change.detail =
        "the software optimization catalog will be validated again when confirmed";
    pending_catalog_change_ = PendingCatalogChange{
        .kind = PendingCatalogKind::software_optimization_update,
        .change = change,
        .software_optimization_update = std::move(*update.update),
    };
    return result(ApplicationSettingsActionCode::confirmation_required,
                  change.detail, std::move(change));
  }

  auto preview = software_optimization_catalog_.preview_rollback();
  if (preview.code != SoftwareOptimizationCatalogLifecycleCode::preview_ready) {
    return result(software_optimization_result_code(preview.code),
                  nonempty_detail(std::move(preview.error),
                                  "the software optimization catalog rollback is unavailable"));
  }
  change.detail = "the software optimization catalog rollback is ready for confirmation";
  pending_catalog_change_ = PendingCatalogChange{
      .kind = PendingCatalogKind::software_optimization_rollback,
      .change = change,
      .owner_token = std::move(preview.preview_token),
  };
  return result(ApplicationSettingsActionCode::confirmation_required,
                change.detail, std::move(change));
}

ApplicationSettingsActionResult ApplicationSettingsService::confirm_catalog_change(
    std::string_view confirmation_token) {
  if (!pending_catalog_change_.has_value() || confirmation_token.empty() ||
      pending_catalog_change_->change.confirmation_token != confirmation_token) {
    return result(ApplicationSettingsActionCode::rejected,
                  "the catalog change confirmation is stale or unavailable");
  }

  auto pending = std::move(*pending_catalog_change_);
  pending_catalog_change_.reset();
  switch (pending.kind) {
    case PendingCatalogKind::software_catalog: {
      auto applied = software_catalog_.apply_preview(pending.owner_token);
      if (!applied.succeeded()) {
        return result(ApplicationSettingsActionCode::failed, applied.message);
      }
      if (applied.current_changed) {
        synchronize_software_catalog_consumers();
      }
      return result(ApplicationSettingsActionCode::completed, applied.message);
    }
    case PendingCatalogKind::settings_catalog: {
      auto confirmed = settings_catalog_.confirm(pending.owner_token);
      auto const code = settings_confirm_code(confirmed.status);
      if (code == ApplicationSettingsActionCode::completed) {
        static_cast<void>(system_settings_.refresh());
      }
      return result(code, confirmed.detail);
    }
    case PendingCatalogKind::software_optimization_update: {
      if (!pending.software_optimization_update.has_value()) {
        return result(ApplicationSettingsActionCode::failed,
                      "the trusted software optimization catalog update is missing");
      }
      auto applied = software_optimization_catalog_.apply_update(
          std::move(*pending.software_optimization_update),
          "application-settings-update");
      auto const code = software_optimization_result_code(applied.code);
      if (applied.state_changed) {
        static_cast<void>(software_optimization_discovery_.refresh());
      }
      return result(code, applied.error);
    }
    case PendingCatalogKind::software_optimization_rollback: {
      auto rolled_back = software_optimization_catalog_.rollback(
          pending.owner_token, true, "application-settings-rollback");
      auto const code = software_optimization_result_code(rolled_back.code);
      if (rolled_back.state_changed) {
        static_cast<void>(software_optimization_discovery_.refresh());
      }
      return result(code, rolled_back.error);
    }
  }
  return result(ApplicationSettingsActionCode::failed,
                "the catalog change kind is unsupported");
}

ApplicationSettingsActionResult ApplicationSettingsService::result(
    ApplicationSettingsActionCode code, std::string detail,
    std::optional<ApplicationSettingsCatalogChange> catalog_change) {
  return {.code = code,
          .snapshot = snapshot(),
          .catalog_change = std::move(catalog_change),
          .detail = std::move(detail)};
}

void ApplicationSettingsService::synchronize_software_catalog_consumers() {
  auto const catalog = software_catalog_.snapshot();
  if (catalog.mode != software_catalog::CatalogLifecycleMode::ready ||
      !catalog.current.has_value() || !catalog.current_catalog.has_value()) {
    return;
  }
  static_cast<void>(software_selection_.on_catalog_replaced(
      {.runtime = *catalog.current_catalog,
       .active = *catalog.current,
       .impact = {}}));
  synchronize_cache_assets();
}

void ApplicationSettingsService::synchronize_cache_assets() {
  auto const selection = software_selection_.snapshot();
  std::vector<offline_package_cache::CacheAsset> assets;
  for (auto const& source : selection.sources) {
    for (auto const& package : source.packages) {
      if (auto asset = offline_package_cache::make_cache_asset(source, package)) {
        assets.push_back(std::move(*asset));
      }
    }
  }
  package_cache_.synchronize_assets(std::move(assets));
}

}  // namespace azzs::application
