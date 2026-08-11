#pragma once

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

struct SoftwareOptimizationCatalogFileRead final {
  bool succeeded{false};
  std::string source;
  std::string error;
};

// Infrastructure adapters read candidate bytes only. Parsing, validation,
// lifecycle state, and apply decisions remain owned by the application/domain
// module and never move into the file adapter.
class SoftwareOptimizationCatalogFile {
 public:
  virtual ~SoftwareOptimizationCatalogFile() = default;

  [[nodiscard]] virtual SoftwareOptimizationCatalogFileRead read(
      std::string_view path) = 0;
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
  bool previous_available{false};
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

// Owns the current/previous software optimization catalog lifecycle. The class
// consumes the existing issue-02 state, structured-log, and occupancy seams;
// it does not modify their storage model and has no optimization executor.
class SoftwareOptimizationCatalogLifecycle final {
 public:
  SoftwareOptimizationCatalogLifecycle(
      DeviceStateStore& states, ExecutionLog& log,
      SharedOperationOccupancy& occupancy,
      SoftwareOptimizationCatalogFile& files,
      std::span<domain::software_optimization_catalog::BuiltInRuleDefinition
                    const>
          built_in_rules) noexcept;

  [[nodiscard]] SoftwareOptimizationCatalogSnapshot snapshot();

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult ensure_builtin(
      std::string_view source, std::string operation_id);

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult apply_update(
      std::string_view path, std::string operation_id);

  [[nodiscard]] SoftwareOptimizationCatalogImportPreview preview_manual_import(
      std::string_view path, bool debug_mode);

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult apply_manual_import(
      std::string_view path, std::string_view expected_preview_token,
      bool confirmed, bool debug_mode, std::string operation_id);

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult rollback(
      std::string operation_id);

 private:
  enum class CandidateKind {
    built_in,
    update,
    manual_import,
  };

  [[nodiscard]] SoftwareOptimizationCatalogLifecycleResult apply_source(
      std::string source, CandidateKind kind, std::string operation_id,
      bool allow_downgrade, std::string source_path);

  DeviceStateStore& states_;
  ExecutionLog& log_;
  SharedOperationOccupancy& occupancy_;
  SoftwareOptimizationCatalogFile& files_;
  std::vector<domain::software_optimization_catalog::BuiltInRuleDefinition>
      built_in_rules_;
};

}  // namespace azzs::application
