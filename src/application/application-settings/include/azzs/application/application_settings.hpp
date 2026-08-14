#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/application_settings_preferences.hpp"
#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/history_and_logs.hpp"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_discovery.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/settings_catalog/settings_catalog_lifecycle.hpp"

namespace azzs::application {

struct ApplicationSettingsDebugSnapshot final {
  bool available{false};
  bool enabled{false};
  bool catalog_editor_available{false};
  bool manual_catalog_import_available{false};
  std::string detail;
};

enum class ApplicationSettingsDebugActionCode {
  updated,
  unavailable,
  rejected,
};

struct ApplicationSettingsDebugActionResult final {
  ApplicationSettingsDebugActionCode code{
      ApplicationSettingsDebugActionCode::unavailable};
  ApplicationSettingsDebugSnapshot snapshot;
  std::string detail;
};

// The debug owner is intentionally injected. Settings only projects and sends
// the typed toggle intent; it cannot manufacture debug authority or editor
// state while issue 32 remains the owner of those capabilities.
class ApplicationSettingsDebugProvider {
 public:
  virtual ~ApplicationSettingsDebugProvider() = default;

  [[nodiscard]] virtual ApplicationSettingsDebugSnapshot snapshot() = 0;
  [[nodiscard]] virtual ApplicationSettingsDebugActionResult set_enabled(
      bool enabled) = 0;
};

struct TrustedSettingsCatalogUpdateRead final {
  std::optional<settings_catalog::catalog_domain::SettingsCatalog> candidate;
  std::string detail;
};

class TrustedSettingsCatalogUpdateSource {
 public:
  virtual ~TrustedSettingsCatalogUpdateSource() = default;

  [[nodiscard]] virtual TrustedSettingsCatalogUpdateRead read_update() = 0;
};

struct TrustedSoftwareOptimizationCatalogUpdateRead final {
  std::optional<TrustedSoftwareOptimizationCatalogUpdate> update;
  std::string detail;
};

class TrustedSoftwareOptimizationCatalogUpdateSource {
 public:
  virtual ~TrustedSoftwareOptimizationCatalogUpdateSource() = default;

  [[nodiscard]] virtual TrustedSoftwareOptimizationCatalogUpdateRead
  read_update() = 0;
};

enum class ApplicationSettingsCatalog {
  software_and_drivers,
  system_settings,
  software_optimization,
};

enum class ApplicationSettingsCatalogAction {
  update,
  rollback,
};

struct ApplicationSettingsCatalogChange final {
  ApplicationSettingsCatalog catalog{
      ApplicationSettingsCatalog::software_and_drivers};
  ApplicationSettingsCatalogAction action{
      ApplicationSettingsCatalogAction::update};
  std::string confirmation_token;
  std::string detail;
};

struct ApplicationSettingsSnapshot final {
  domain::architecture_selection::ArchitecturePreference
      architecture_preference{
          domain::architecture_selection::ArchitecturePreference::
              prefer_arm64_prompt_fallback};
  offline_package_cache::OfflinePackageCacheSnapshot cache;
  HistoryAndLogsSnapshot history_and_logs;
  SystemSettingsApplySnapshot system_settings;
  std::vector<SystemSettingsRecoveryRecord> recovery_records;
  software_catalog::SoftwareCatalogLifecycleSnapshot software_catalog;
  settings_catalog::CatalogSnapshot settings_catalog;
  SoftwareOptimizationCatalogSnapshot software_optimization_catalog;
  ApplicationSettingsDebugSnapshot debug;
};

enum class ApplicationSettingsActionCode {
  completed,
  confirmation_required,
  no_change,
  unavailable,
  protected_operation,
  rejected,
  failed,
};

enum class ApplicationSettingsConfirmationAction {
  clear_cache,
  clear_logs,
  delete_recovery_record,
  catalog_change,
};

// Destructive settings operations must retain an explicit confirmation gate.
[[nodiscard]] constexpr bool requires_explicit_confirmation(
    ApplicationSettingsConfirmationAction action) noexcept {
  switch (action) {
    case ApplicationSettingsConfirmationAction::clear_cache:
    case ApplicationSettingsConfirmationAction::clear_logs:
    case ApplicationSettingsConfirmationAction::delete_recovery_record:
    case ApplicationSettingsConfirmationAction::catalog_change:
      return true;
  }
  return true;
}

struct ApplicationSettingsActionResult final {
  ApplicationSettingsActionCode code{ApplicationSettingsActionCode::failed};
  ApplicationSettingsSnapshot snapshot;
  std::optional<ApplicationSettingsCatalogChange> catalog_change;
  std::string detail;
};

// This is the only application-level settings coordinator. It composes the
// owner snapshots and commands without taking ownership of their persistence
// formats or state. Catalog changes refresh their declared consumers only
// after the owning lifecycle reports a committed state transition.
class ApplicationSettingsService final {
 public:
  ApplicationSettingsService(
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
      TrustedSettingsCatalogUpdateSource* settings_catalog_updates = nullptr,
      TrustedSoftwareOptimizationCatalogUpdateSource*
          software_optimization_catalog_updates = nullptr,
      ApplicationSettingsDebugProvider* debug = nullptr) noexcept;

  [[nodiscard]] ApplicationSettingsSnapshot snapshot();
  [[nodiscard]] ApplicationSettingsActionResult set_architecture_preference(
      domain::architecture_selection::ArchitecturePreference preference);
  [[nodiscard]] ApplicationSettingsActionResult set_cache_retention(
      domain::offline_package_cache::CacheRetentionPolicy retention);
  [[nodiscard]] ApplicationSettingsActionResult clear_cache(bool confirmed);
  [[nodiscard]] ApplicationSettingsActionResult clear_logs(bool confirmed);
  [[nodiscard]] ApplicationSettingsActionResult export_diagnostic();
  [[nodiscard]] ApplicationSettingsActionResult delete_recovery_record(
      std::uint64_t record_id, bool confirmed);
  [[nodiscard]] ApplicationSettingsActionResult set_debug_enabled(bool enabled);
  [[nodiscard]] ApplicationSettingsActionResult prepare_catalog_change(
      ApplicationSettingsCatalog catalog, ApplicationSettingsCatalogAction action);
  [[nodiscard]] ApplicationSettingsActionResult confirm_catalog_change(
      std::string_view confirmation_token);

 private:
  enum class PendingCatalogKind {
    software_catalog,
    settings_catalog,
    software_optimization_update,
    software_optimization_rollback,
  };

  struct PendingCatalogChange final {
    PendingCatalogKind kind{PendingCatalogKind::software_catalog};
    ApplicationSettingsCatalogChange change;
    std::string owner_token;
    std::optional<TrustedSoftwareOptimizationCatalogUpdate>
        software_optimization_update;
  };

  [[nodiscard]] ApplicationSettingsActionResult result(
      ApplicationSettingsActionCode code, std::string detail = {},
      std::optional<ApplicationSettingsCatalogChange> catalog_change =
          std::nullopt);
  void synchronize_software_catalog_consumers();
  void synchronize_cache_assets();

  architecture_selection::ArchitectureSelectionLifecycle&
      architecture_selection_;
  ArchitecturePreferences& architecture_preferences_;
  offline_package_cache::OfflinePackageCacheService& package_cache_;
  offline_package_cache::OfflinePackageCacheService& batch_package_cache_;
  CacheRetentionPreferences& cache_retention_preferences_;
  HistoryAndLogsService& history_and_logs_;
  SystemSettingsApplyService& system_settings_;
  software_catalog::SoftwareCatalogLifecycle& software_catalog_;
  settings_catalog::SettingsCatalogLifecycle& settings_catalog_;
  SoftwareOptimizationCatalogLifecycle& software_optimization_catalog_;
  software_selection::SoftwareSelectionLifecycle& software_selection_;
  software_optimization_discovery::SoftwareOptimizationDiscoveryService&
      software_optimization_discovery_;
  TrustedSettingsCatalogUpdateSource* settings_catalog_updates_{};
  TrustedSoftwareOptimizationCatalogUpdateSource*
      software_optimization_catalog_updates_{};
  ApplicationSettingsDebugProvider* debug_{};
  std::optional<PendingCatalogChange> pending_catalog_change_;
  std::uint64_t next_confirmation_{1};
};

}  // namespace azzs::application
