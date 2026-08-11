#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/domain/device_state.hpp"
#include "azzs/domain/software_catalog.hpp"

namespace azzs::application::software_catalog {

struct CatalogDecodeResult final {
  std::optional<domain::software_catalog::SoftwareCatalogDocument> document;
  std::vector<domain::software_catalog::CatalogIssue> issues;
};

// Application-owned codec seam. TOML is an adapter detail; every maintenance
// entry point returns the same domain document and the same diagnostic type.
class SoftwareCatalogCodec {
 public:
  virtual ~SoftwareCatalogCodec() = default;

  [[nodiscard]] virtual CatalogDecodeResult decode(
      std::string_view bytes) const = 0;
  [[nodiscard]] virtual std::string encode(
      domain::software_catalog::SoftwareCatalogDocument const& document) const =
      0;
};

struct CatalogFileRead final {
  bool succeeded{false};
  std::string path;
  std::string bytes;
  std::string error;
};

// The source adapter owns which bytes are built in or a trusted update. Only
// a manual import accepts a user-selected path at the application seam.
class SoftwareCatalogFileReader {
 public:
  virtual ~SoftwareCatalogFileReader() = default;
  [[nodiscard]] virtual CatalogFileRead read_built_in() const = 0;
  [[nodiscard]] virtual CatalogFileRead read_update() const = 0;
  [[nodiscard]] virtual CatalogFileRead read_manual_import(
      std::string const& path) const = 0;
};

enum class CatalogCandidateOrigin {
  built_in,
  update,
  manual_import,
  saved_draft,
  rollback,
};

enum class CatalogEditorAccess {
  unavailable,
  debug_mode,
  temporary_close_recovery,
};

// The caller cannot manufacture debug authority or a temporary recovery
// session. The core queries this injected, trusted state owner for each use
// case so a stale UI snapshot cannot authorize a write.
class CatalogMaintenanceAccess {
 public:
  virtual ~CatalogMaintenanceAccess() = default;
  [[nodiscard]] virtual CatalogEditorAccess editor_access() const noexcept = 0;
};

enum class EffectiveCatalogIdentity {
  released,
  local_trial,
};

enum class CatalogSelectionImpactReason {
  added,
  removed,
  execution_semantics_changed,
  runtime_available,
  runtime_unavailable,
};

enum class CatalogSelectionItemKind {
  software,
  driver,
};

struct CatalogSelectionImpactItem final {
  std::string id;
  CatalogSelectionItemKind kind{CatalogSelectionItemKind::software};
  CatalogSelectionImpactReason reason{CatalogSelectionImpactReason::added};

  auto operator<=>(CatalogSelectionImpactItem const&) const = default;
};

struct CatalogSelectionImpact final {
  std::vector<std::string> added;
  std::vector<std::string> removed;
  std::vector<std::string> changed;
  std::vector<std::string> disabled;
  std::vector<CatalogSelectionImpactItem> items;

  auto operator<=>(CatalogSelectionImpact const&) const = default;
};

struct CatalogCandidatePreview final {
  bool ready{false};
  std::string confirmation_token;
  CatalogCandidateOrigin origin{CatalogCandidateOrigin::built_in};
  std::string path;
  std::string content_identity;
  std::uint64_t revision{0};
  std::size_t item_count{0};
  bool downgrade{false};
  CatalogSelectionImpact selection_impact;
  domain::software_catalog::RuntimeCatalogLoad runtime;
  domain::software_catalog::SoftwareCatalogReleaseGate release_gate;
  bool log_persisted{false};
  std::string log_error;
  std::string error;
};

struct ActiveCatalogInfo final {
  std::uint64_t revision{0};
  std::size_t item_count{0};
  CatalogCandidateOrigin origin{CatalogCandidateOrigin::built_in};
  EffectiveCatalogIdentity identity{EffectiveCatalogIdentity::local_trial};
  std::string content_identity;
  std::string application_id;
  std::vector<domain::software_catalog::CatalogIssue> local_issues;
  std::vector<domain::software_catalog::CatalogIssue> release_issues;

  auto operator<=>(ActiveCatalogInfo const&) const = default;
};

enum class DraftWorkState {
  none,
  saved_not_applied,
  unsaved_changes,
  recovered_unsaved,
};

struct CatalogDraftInfo final {
  DraftWorkState state{DraftWorkState::none};
  bool saved_present{false};
  bool validation_failed{false};
  bool cleanup_pending{false};
  bool checkpoint_cleanup_pending{false};
  std::optional<std::string> toml_bytes;
  std::optional<domain::software_catalog::SoftwareCatalogDocument> document;
  std::optional<std::uint64_t> candidate_revision;
  std::vector<domain::software_catalog::CatalogIssue> runtime_issues;
  std::vector<domain::software_catalog::CatalogIssue> release_issues;

  auto operator<=>(CatalogDraftInfo const&) const = default;
};

enum class CatalogLifecycleMode {
  not_restored,
  ready,
  read_only,
  failed,
};

enum class CatalogAggregateAccess {
  not_restored,
  writable,
  read_only,
  occupied,
  failed,
};

struct SoftwareCatalogLifecycleSnapshot final {
  CatalogLifecycleMode mode{CatalogLifecycleMode::not_restored};
  CatalogAggregateAccess machine_access{CatalogAggregateAccess::not_restored};
  CatalogAggregateAccess draft_access{CatalogAggregateAccess::not_restored};
  std::optional<ActiveCatalogInfo> current;
  std::optional<ActiveCatalogInfo> previous;
  std::optional<std::string> current_toml_bytes;
  std::optional<std::string> retained_unreadable_current_toml_bytes;
  std::optional<domain::software_catalog::SoftwareCatalogDocument>
      current_document;
  std::optional<domain::software_catalog::RuntimeSoftwareCatalog>
      current_catalog;
  std::optional<domain::software_catalog::RuntimeSoftwareCatalog>
      previous_catalog;
  CatalogDraftInfo draft;
  std::string error;
};

enum class CatalogActionCode {
  succeeded,
  no_change,
  rejected,
  debug_mode_required,
  not_restored,
  unavailable,
  occupied,
  conflict,
  read_only,
  persistence_failed,
  outcome_unknown,
  saved_cleanup_pending,
  applied_cleanup_pending,
  applied_log_incomplete,
  returned_to_editor,
};

struct CatalogActionResult final {
  CatalogActionCode code{CatalogActionCode::rejected};
  bool current_changed{false};
  bool draft_changed{false};
  domain::software_catalog::RuntimeCatalogLoad runtime;
  domain::software_catalog::SoftwareCatalogReleaseGate release_gate;
  CatalogSelectionImpact selection_impact;
  bool log_persisted{false};
  std::string log_error;
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == CatalogActionCode::succeeded ||
           code == CatalogActionCode::no_change ||
           code == CatalogActionCode::saved_cleanup_pending ||
           code == CatalogActionCode::applied_cleanup_pending ||
           code == CatalogActionCode::applied_log_incomplete;
  }
};

enum class CatalogCloseChoice {
  save_draft_and_close,
  discard_unsaved_and_close,
  return_to_editor,
};

// Owns the current/previous machine aggregate and the subject-owned draft and
// unsaved checkpoint. It deliberately exposes use cases instead of persistence
// steps, keeping cross-aggregate cleanup and outcome-unknown recovery private.
class SoftwareCatalogLifecycle final {
 public:
  SoftwareCatalogLifecycle(
      DeviceStateStore& states, ExecutionLog& log,
      SharedOperationOccupancy& occupancy, SoftwareCatalogFileReader& files,
      SoftwareCatalogCodec& codec,
      domain::software_catalog::SoftwareCatalogPolicy policy,
      CatalogMaintenanceAccess& maintenance_access,
      domain::StateSubject state_subject);
  ~SoftwareCatalogLifecycle();

  SoftwareCatalogLifecycle(SoftwareCatalogLifecycle const&) = delete;
  SoftwareCatalogLifecycle& operator=(SoftwareCatalogLifecycle const&) = delete;
  SoftwareCatalogLifecycle(SoftwareCatalogLifecycle&&) = delete;
  SoftwareCatalogLifecycle& operator=(SoftwareCatalogLifecycle&&) = delete;

  [[nodiscard]] CatalogActionResult restore();
  [[nodiscard]] SoftwareCatalogLifecycleSnapshot snapshot() const;

  [[nodiscard]] CatalogCandidatePreview preview_built_in();
  [[nodiscard]] CatalogCandidatePreview preview_update();
  [[nodiscard]] CatalogCandidatePreview preview_manual_import(std::string path);
  [[nodiscard]] CatalogCandidatePreview preview_rollback();
  [[nodiscard]] CatalogActionResult apply_preview(
      std::string_view confirmation_token);

  [[nodiscard]] CatalogActionResult edit(std::string toml_bytes);
  [[nodiscard]] CatalogActionResult edit_document(
      domain::software_catalog::SoftwareCatalogDocument document);
  [[nodiscard]] CatalogActionResult checkpoint_unsaved();
  [[nodiscard]] CatalogActionResult save_draft();
  [[nodiscard]] CatalogActionResult delete_saved_draft();
  [[nodiscard]] CatalogActionResult discard_unsaved();
  [[nodiscard]] CatalogActionResult apply_saved_draft();
  [[nodiscard]] CatalogActionResult handle_close(CatalogCloseChoice choice);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azzs::application::software_catalog
