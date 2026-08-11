#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/settings_catalog/settings_catalog.hpp"

namespace azzs::application::settings_catalog {

namespace catalog_domain = domain::settings_catalog;

struct SettingsCatalogState final {
  catalog_domain::SettingsCatalog current;
  std::optional<catalog_domain::SettingsCatalog> previous;

  friend bool operator==(SettingsCatalogState const&,
                         SettingsCatalogState const&) = default;
};

enum class CatalogStorageReadStatus {
  uninitialized,
  writable,
  recovered_read_only,
  read_only,
  busy,
  failed,
};

struct CatalogStorageRead final {
  CatalogStorageReadStatus status{CatalogStorageReadStatus::failed};
  std::optional<SettingsCatalogState> state;
  std::optional<domain::RevisionToken> revision;
  std::string detail;
};

enum class CatalogStorageWriteStatus {
  committed,
  conflict,
  read_only,
  busy,
  failed,
  outcome_unknown,
};

struct CatalogStorageWrite final {
  CatalogStorageWriteStatus status{CatalogStorageWriteStatus::failed};
  std::optional<domain::RevisionToken> revision;
  std::string detail;
};

class SettingsCatalogStateStorage {
 public:
  virtual ~SettingsCatalogStateStorage() = default;

  [[nodiscard]] virtual CatalogStorageRead read() = 0;
  [[nodiscard]] virtual CatalogStorageWrite write(
      std::optional<domain::RevisionToken> expected_revision,
      SettingsCatalogState state) = 0;
};

enum class CatalogImportStatus {
  loaded,
  not_found,
  invalid,
  failed,
};

struct CatalogImportRead final {
  CatalogImportStatus status{CatalogImportStatus::failed};
  std::optional<catalog_domain::SettingsCatalog> catalog;
  std::string source_path;
  std::string detail;
};

class SettingsCatalogImportSource {
 public:
  virtual ~SettingsCatalogImportSource() = default;
  [[nodiscard]] virtual CatalogImportRead read_import(
      std::string const& path) = 0;
};

enum class CatalogSnapshotStatus {
  available,
  unavailable,
  recovered_read_only,
  read_only,
  busy,
  failed,
};

struct CatalogSnapshot final {
  CatalogSnapshotStatus status{CatalogSnapshotStatus::failed};
  std::optional<catalog_domain::ValidatedSettingsCatalog> current;
  std::optional<catalog_domain::ValidatedSettingsCatalog> previous;
  std::string detail;
};

enum class InitializeStatus {
  initialized,
  already_initialized,
  rejected,
  occupied,
  conflict,
  read_only,
  busy,
  failed,
};

struct InitializeResult final {
  InitializeStatus status{InitializeStatus::failed};
  std::vector<catalog_domain::CatalogProblem> problems;
  std::string detail;
};

enum class CatalogChangeOrigin {
  update,
  debug_import,
  rollback,
};

enum class PrepareStatus {
  ready,
  unavailable,
  debug_mode_required,
  rejected,
  downgrade_requires_debug_import,
  no_change,
  no_previous,
  read_only,
  busy,
  failed,
};

struct PreparedCatalogChange final {
  std::string confirmation_token;
  CatalogChangeOrigin origin{CatalogChangeOrigin::update};
  catalog_domain::CatalogChangePreview changes;
  std::optional<std::string> source_path;
  std::size_t setting_count{0};
  std::size_t plan_count{0};
};

struct PrepareResult final {
  PrepareStatus status{PrepareStatus::failed};
  std::optional<PreparedCatalogChange> prepared;
  std::vector<catalog_domain::CatalogProblem> problems;
  std::string detail;
};

enum class ConfirmStatus {
  committed,
  confirmation_required,
  stale_preview,
  occupied,
  conflict,
  read_only,
  busy,
  failed,
  outcome_unknown,
};

struct ConfirmResult final {
  ConfirmStatus status{ConfirmStatus::failed};
  std::uint64_t active_revision{0};
  std::string detail;
};

class SettingsCatalogLifecycle final {
 public:
  SettingsCatalogLifecycle(
      SettingsCatalogStateStorage& storage,
      SettingsCatalogImportSource& imports, ExecutionLog& log,
      SharedOperationOccupancy& occupancy,
      catalog_domain::SupportedCapabilities capabilities) noexcept;

  [[nodiscard]] CatalogSnapshot snapshot();
  [[nodiscard]] InitializeResult initialize_builtin(
      catalog_domain::SettingsCatalog catalog);
  [[nodiscard]] PrepareResult prepare_update(
      catalog_domain::SettingsCatalog candidate);
  [[nodiscard]] PrepareResult prepare_debug_import(std::string path,
                                                   bool debug_mode_enabled);
  [[nodiscard]] PrepareResult prepare_rollback();
  [[nodiscard]] ConfirmResult confirm(std::string_view confirmation_token);

 private:
  struct LoadedState;
  struct PendingChange final {
    PreparedCatalogChange prepared;
    SettingsCatalogState next_state;
    domain::RevisionToken expected_revision;
    CorrelationId correlation;
  };

  [[nodiscard]] LoadedState load();
  [[nodiscard]] PrepareResult prepare_candidate(
      catalog_domain::SettingsCatalog candidate, CatalogChangeOrigin origin,
      std::optional<std::string> source_path);
  [[nodiscard]] InitializeResult initialize_validated(
      catalog_domain::ValidatedSettingsCatalog catalog);
  void log_event(CorrelationId const& correlation, std::string stage,
                 ExecutionResult result, std::uint64_t revision,
                 std::optional<std::string> error = std::nullopt);

  SettingsCatalogStateStorage& storage_;
  SettingsCatalogImportSource& imports_;
  ExecutionLog& log_;
  SharedOperationOccupancy& occupancy_;
  catalog_domain::SupportedCapabilities capabilities_;
  std::uint64_t next_confirmation_{1};
  std::optional<PendingChange> pending_;
};

}  // namespace azzs::application::settings_catalog
