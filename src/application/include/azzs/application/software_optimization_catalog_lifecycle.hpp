#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/domain/software_optimization_catalog.hpp"

namespace azzs::application {

struct SoftwareOptimizationCatalogLocalImportRead final {
  bool succeeded{false};
  std::string source;
  std::string error;
};

// This adapter is intentionally limited to explicit local debug imports.
// Trusted updates arrive as typed bytes and cannot route arbitrary paths
// through this file boundary.
class SoftwareOptimizationCatalogLocalImportFile {
 public:
  virtual ~SoftwareOptimizationCatalogLocalImportFile() = default;

  [[nodiscard]] virtual SoftwareOptimizationCatalogLocalImportRead read(
      std::string_view path) = 0;
};

class SoftwareOptimizationCatalogDebugAuthorization {
 public:
  virtual ~SoftwareOptimizationCatalogDebugAuthorization() = default;

  [[nodiscard]] virtual bool local_import_allowed() const noexcept = 0;
};

struct TrustedSoftwareOptimizationCatalogUpdate final {
  std::string source;
  std::string source_reference;
};

enum class SoftwareOptimizationCatalogSourceKind : std::uint8_t {
  embedded_builtin = 1,
  trusted_update = 2,
  local_debug_import = 3,
  legacy_unclassified = 4,
};

struct SoftwareOptimizationCatalogProvenance final {
  SoftwareOptimizationCatalogSourceKind kind{
      SoftwareOptimizationCatalogSourceKind::legacy_unclassified};
  bool local_trial{true};
  std::string redacted_source;

  auto operator<=>(SoftwareOptimizationCatalogProvenance const&) const =
      default;
};

enum class SoftwareOptimizationCatalogStateMode {
  unavailable,
  available,
  read_only,
  failed,
};

struct SoftwareOptimizationCatalogSnapshot final {
  SoftwareOptimizationCatalogStateMode mode{
      SoftwareOptimizationCatalogStateMode::failed};
  std::optional<domain::software_optimization_catalog::SoftwareOptimizationCatalog>
      current;
  std::optional<SoftwareOptimizationCatalogProvenance> current_provenance;
  bool previous_available{false};
  std::optional<SoftwareOptimizationCatalogProvenance> previous_provenance;
  std::vector<domain::software_optimization_catalog::StableIdentityRecord>
      identity_history;
  std::string error;
};

enum class SoftwareOptimizationCatalogLifecycleCode {
  preview_ready,
  applied,
  downgraded,
  rolled_back,
  unchanged,
  rejected,
  debug_mode_required,
  confirmation_required,
  preview_stale,
  occupied,
  read_only,
  file_failed,
  persistence_failed,
  logging_failed,
  no_previous,
};

struct SoftwareOptimizationCatalogLifecycleResult final {
  SoftwareOptimizationCatalogLifecycleCode code{
      SoftwareOptimizationCatalogLifecycleCode::rejected};
  bool state_changed{false};
  std::optional<domain::software_optimization_catalog::CatalogSummary> active;
  std::vector<domain::software_optimization_catalog::CatalogIssue> issues;
  std::string error;
  std::string logging_error;
  std::string occupancy_error;
};

struct SoftwareOptimizationCatalogImportPreview final {
  SoftwareOptimizationCatalogLifecycleCode code{
      SoftwareOptimizationCatalogLifecycleCode::rejected};
  std::string path;
  // Non-security token that binds confirmation to the exact previewed bytes.
  // It is not a signature, provenance claim, or published integrity hash.
  std::string preview_token;
  std::optional<domain::software_optimization_catalog::CatalogSummary>
      candidate;
  bool downgrade{false};
  std::vector<domain::software_optimization_catalog::StableId>
      lost_or_changed_schemes;
  std::vector<domain::software_optimization_catalog::CatalogIssue> issues;
  std::string error;
};

struct SoftwareOptimizationCatalogRollbackPreview final {
  SoftwareOptimizationCatalogLifecycleCode code{
      SoftwareOptimizationCatalogLifecycleCode::rejected};
  std::string preview_token;
  std::optional<domain::software_optimization_catalog::CatalogSummary> current;
  std::optional<domain::software_optimization_catalog::CatalogSummary> candidate;
  std::optional<SoftwareOptimizationCatalogProvenance> candidate_provenance;
  std::vector<domain::software_optimization_catalog::CatalogIssue> issues;
  std::string error;
};

// Owns the current/previous software optimization catalog lifecycle. The class
// consumes the existing issue-02 state, structured-log, and occupancy seams;
// it does not modify their storage model and has no optimization executor.
class SoftwareOptimizationCatalogLifecycle final {
 public:
  SoftwareOptimizationCatalogLifecycle(
      DeviceStateStore& states, ExecutionLog& log,
      SharedOperationOccupancy& occupancy,
      SoftwareOptimizationCatalogLocalImportFile& local_import_files,
      SoftwareOptimizationCatalogDebugAuthorization const& debug_authorization,
      std::span<domain::software_optimization_catalog::BuiltInRuleDefinition
                    const>
          built_in_rules,
      std::span<domain::software_optimization_catalog::
                    SoftwareCatalogInstallerBaseline const>
          installer_baselines) noexcept;

  [[nodiscard]] SoftwareOptimizationCatalogSnapshot snapshot();

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult ensure_builtin(
      std::string_view source, std::string operation_id);

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult apply_update(
      TrustedSoftwareOptimizationCatalogUpdate update,
      std::string operation_id);

  [[nodiscard]] SoftwareOptimizationCatalogImportPreview preview_manual_import(
      std::string_view path);

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult apply_manual_import(
      std::string_view path, std::string_view expected_preview_token,
      bool confirmed, std::string operation_id);

  [[nodiscard]] SoftwareOptimizationCatalogRollbackPreview preview_rollback();

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult rollback(
      std::string_view expected_preview_token, bool confirmed,
      std::string operation_id);

 private:
  enum class CandidateKind {
    built_in,
    update,
    manual_import,
  };

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult apply_source(
      std::string source, CandidateKind kind, std::string operation_id,
      bool allow_downgrade, std::string source_reference,
      SoftwareOptimizationCatalogProvenance provenance,
      std::optional<std::string> expected_preview_token = std::nullopt);

  DeviceStateStore& states_;
  ExecutionLog& log_;
  SharedOperationOccupancy& occupancy_;
  SoftwareOptimizationCatalogLocalImportFile& local_import_files_;
  SoftwareOptimizationCatalogDebugAuthorization const& debug_authorization_;
  std::vector<domain::software_optimization_catalog::BuiltInRuleDefinition>
      built_in_rules_;
  std::vector<domain::software_optimization_catalog::
                  SoftwareCatalogInstallerBaseline>
      installer_baselines_;
};

}  // namespace azzs::application
