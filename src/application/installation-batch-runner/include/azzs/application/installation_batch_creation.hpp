#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "azzs/application/installation_batch.hpp"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/domain/controlled_install_profiles.hpp"

namespace azzs::application::installation_batch {

// This is a read-only projection assembled by the unique composition root.
// It deliberately exposes no persistence, source resolution, download, or
// installer effect.  The creation service reads it once to freeze a plan.
struct InstallationBatchPlanningSnapshot final {
  software_catalog::SoftwareCatalogLifecycleSnapshot catalog;
  software_selection::SoftwareSelectionSnapshot selection;
  offline_package_cache::OfflinePackageCacheSnapshot cache;
};

class InstallationBatchPlanningStatePort {
 public:
  virtual ~InstallationBatchPlanningStatePort() = default;
  [[nodiscard]] virtual InstallationBatchPlanningSnapshot snapshot() = 0;
};

class LiveInstallationBatchPlanningState final
    : public InstallationBatchPlanningStatePort {
 public:
  LiveInstallationBatchPlanningState(
      software_catalog::SoftwareCatalogLifecycle const& catalogs,
      software_selection::SoftwareSelectionLifecycle const& selections,
      offline_package_cache::OfflinePackageCacheService& cache) noexcept;

  [[nodiscard]] InstallationBatchPlanningSnapshot snapshot() override;

 private:
  software_catalog::SoftwareCatalogLifecycle const& catalogs_;
  software_selection::SoftwareSelectionLifecycle const& selections_;
  offline_package_cache::OfflinePackageCacheService& cache_;
};

// Project-owned profile declarations are injected so a headless contract can
// supply a closed fixture.  Production wiring uses the initial built-in
// catalog; this is not a runtime third-party extension seam.
class ControlledInstallProfileCatalog {
 public:
  virtual ~ControlledInstallProfileCatalog() = default;
  [[nodiscard]] virtual std::span<domain::software_catalog::ControlledInstallProfile const>
  profiles() const noexcept = 0;
};

class InitialControlledInstallProfileCatalog final
    : public ControlledInstallProfileCatalog {
 public:
  [[nodiscard]] std::span<domain::software_catalog::ControlledInstallProfile const>
  profiles() const noexcept override;
};

// Only plans staged by InstallationBatchCreationService are admitted.  The
// registry is an asset ledger, not a second batch state machine: it records
// immutable source/package/profile/cache facts and rebuilds only from a
// durable plan supplied by InstallationBatchService during restore.
class FrozenBatchAssetRegistry final : public FrozenBatchPlanAdmissionPort {
 public:
  [[nodiscard]] bool stage_initial(
      domain::installation_batch::FrozenBatchPlan const& plan) noexcept;
  [[nodiscard]] bool stage_retry(
      domain::installation_batch::FrozenBatchPlan const& plan) noexcept;
  void discard(std::string const& batch_id) noexcept;
  void commit(std::string const& batch_id) noexcept;
  [[nodiscard]] std::optional<domain::installation_batch::FrozenBatchPlan>
  find(std::string const& batch_id) const;

  [[nodiscard]] FrozenBatchPlanAdmission admit(
      domain::installation_batch::FrozenBatchPlan const& plan) const override;
  [[nodiscard]] FrozenBatchPlanAdmission admit_retry(
      domain::installation_batch::FrozenBatchPlan const& plan) const override;
  void observe_restored(
      domain::installation_batch::FrozenBatchPlan const& plan) const noexcept override;

 private:
  enum class EntryKind { initial, retry, restored };
  struct Entry final {
    domain::installation_batch::FrozenBatchPlan plan;
    EntryKind kind{EntryKind::initial};
    bool committed{false};
  };

  [[nodiscard]] bool stage(domain::installation_batch::FrozenBatchPlan const& plan,
                           EntryKind kind) noexcept;
  [[nodiscard]] FrozenBatchPlanAdmission admit_kind(
      domain::installation_batch::FrozenBatchPlan const& plan,
      EntryKind expected) const;

  mutable std::vector<Entry> entries_;
};

struct InstallationPackageChoice final {
  std::string software_id;
  domain::software_catalog::SourcePurpose declared_purpose{
      domain::software_catalog::SourcePurpose::primary};
  std::string declared_address;
  std::string package_identity;
  domain::software_catalog::InteractionDisposition interaction_disposition{
      domain::software_catalog::InteractionDisposition::controlled_automatic};
  std::vector<domain::installation_batch::FrozenPreferenceChoice> preferences;
};

struct InstallationBatchCreateRequest final {
  std::string batch_id;
  std::string correlation_id;
  std::vector<InstallationPackageChoice> packages;
  std::int64_t frozen_at_milliseconds{};
};

struct InstallationBatchRetryRequest final {
  std::string batch_id;
  std::string correlation_id;
  std::int64_t frozen_at_milliseconds{};
};

enum class InstallationBatchCreationCode {
  ready,
  not_ready,
  invalid_request,
  selection_incomplete,
  source_unresolved,
  package_unavailable,
  profile_unavailable,
  cache_unavailable,
  dependency_incomplete,
  admission_rejected,
  no_retryable_item,
  batch_rejected,
};

struct InstallationBatchCreationAssessment final {
  InstallationBatchCreationCode code{InstallationBatchCreationCode::not_ready};
  std::size_t item_count{};
  std::string detail;

  [[nodiscard]] bool ready() const noexcept {
    return code == InstallationBatchCreationCode::ready;
  }
};

struct InstallationBatchCreationResult final {
  InstallationBatchCreationAssessment assessment;
  InstallationBatchActionResult batch;
};

// Freezes the complete execution closure before delegating the only business
// state transition to InstallationBatchService.  Neither UI callers nor this
// service may start a download or installer effect.
class InstallationBatchCreationService final {
 public:
  InstallationBatchCreationService(
      InstallationBatchService& batches, InstallationBatchPlanningStatePort& state,
      ControlledProfileReadinessPort& readiness,
      ControlledInstallProfileCatalog const& profiles,
      FrozenBatchAssetRegistry& assets) noexcept;

  [[nodiscard]] InstallationBatchCreationAssessment assess(
      InstallationBatchCreateRequest const& request);
  [[nodiscard]] InstallationBatchCreationResult create(
      InstallationBatchCreateRequest const& request);
  [[nodiscard]] InstallationBatchCreationResult retry_current(
      InstallationBatchRetryRequest const& request);

 private:
  struct PlanAssessment final {
    InstallationBatchCreationAssessment assessment;
    std::optional<domain::installation_batch::FrozenBatchPlan> plan;
  };

  [[nodiscard]] PlanAssessment assess_plan(
      InstallationBatchCreateRequest const& request);

  InstallationBatchService& batches_;
  InstallationBatchPlanningStatePort& state_;
  ControlledProfileReadinessPort& readiness_;
  ControlledInstallProfileCatalog const& profiles_;
  FrozenBatchAssetRegistry& assets_;
};

[[nodiscard]] char const* to_string(InstallationBatchCreationCode value) noexcept;

}  // namespace azzs::application::installation_batch
