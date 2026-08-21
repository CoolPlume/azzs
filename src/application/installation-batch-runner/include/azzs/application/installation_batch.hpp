#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/restart_resume.hpp"
#include "azzs/domain/installation_batch.hpp"

namespace azzs::application::software_catalog {
class SoftwareCatalogLifecycle;
}

namespace azzs::application::software_selection {
class SoftwareSelectionLifecycle;
}

namespace azzs::application::installation_batch {

namespace batch_domain = domain::installation_batch;

enum class InstallationDownloadCode {
  cached_ready,
  downloading,
  completed,
  waiting_network,
  source_invalid,
  paused,
  restart_required,
  stopped,
  failed,
};

struct InstallationDownloadProgress final {
  std::uint64_t downloaded_bytes{};
  std::optional<std::uint64_t> total_bytes;
  std::optional<std::uint64_t> bytes_per_second;
  std::optional<std::uint64_t> remaining_seconds;
  bool accessibility_announcement_due{false};
};

struct InstallationDownloadObservation final {
  InstallationDownloadCode code{InstallationDownloadCode::failed};
  InstallationDownloadProgress progress;
  std::string detail;
};

// The effect boundary deliberately excludes the source snapshot and selected
// package. A project-owned adapter can recover a cached artifact only from the
// opaque item handle, cache identity and controlled root; it never receives a
// URL, path, command line, selector, or catalog payload.
struct InstallationEffectTarget final {
  std::string item_id;
  domain::offline_package_cache::CacheAsset cache_asset;
  domain::offline_package_cache::ControlledCacheRoot cache_root;
  batch_domain::FrozenExecutionProfile execution_profile;
  std::string opaque_item_handle;

  [[nodiscard]] bool valid() const noexcept;
};

// No URL, file name or path crosses this seam. The port receives only the
// typed effect target derived from the frozen plan.
class InstallationDownloadPort {
 public:
  virtual ~InstallationDownloadPort() = default;
  [[nodiscard]] virtual InstallationDownloadObservation advance(
      InstallationEffectTarget const& target) = 0;
  [[nodiscard]] virtual InstallationDownloadObservation pause(
      InstallationEffectTarget const& target) = 0;
  [[nodiscard]] virtual InstallationDownloadObservation resume(
      InstallationEffectTarget const& target) = 0;
  [[nodiscard]] virtual InstallationDownloadObservation stop(
      InstallationEffectTarget const& target) = 0;
};

enum class InstallerLaunchCode {
  started,
  interaction_required,
  waiting_network,
  source_invalid,
  failed,
};

struct ControlledInstallerLaunch final {
  InstallationEffectTarget target;
};

struct ControlledInstallerObservation final {
  InstallerLaunchCode code{InstallerLaunchCode::failed};
  // This adapter fact is not a process path, command line, handle, URL or UI
  // selector. It is retained only for later project-owned verification.
  std::optional<std::string> opaque_operation_handle;
  std::string detail;
};

enum class InstallerCompletionCode {
  running,
  completed,
  interaction_required,
  unknown,
  failed,
};

// A process exit is not sufficient when the frozen profile declares a
// controlled post-install behavior. That behavior supplies its own fact.
enum class PostInstallCompletionCode {
  not_required,
  completed,
  pending,
  unknown,
  failed,
};

// Launch and completion are deliberately separate facts. A later observation
// must never repeat the launch effect, and result verification is authorized
// only after the project-owned executor has reported completed.
struct ControlledInstallerCompletionRequest final {
  InstallationEffectTarget target;
  std::optional<std::string> opaque_operation_handle;
};

struct ControlledInstallerCompletionObservation final {
  InstallerCompletionCode code{InstallerCompletionCode::unknown};
  PostInstallCompletionCode post_install{PostInstallCompletionCode::unknown};
  std::string detail;
};

enum class InstallerTerminationCode {
  terminated,
  still_running,
  unavailable,
  unknown,
  failed,
};

// The executor receives only the operation handle it created. In particular,
// a caller cannot request a PID/name/path-based termination of another process.
struct ControlledInstallerTerminationRequest final {
  InstallationEffectTarget target;
  std::optional<std::string> opaque_operation_handle;
};

struct ControlledInstallerTerminationObservation final {
  InstallerTerminationCode code{InstallerTerminationCode::unknown};
  std::string detail;
};

class ControlledInstallerExecutor {
 public:
  virtual ~ControlledInstallerExecutor() = default;
  [[nodiscard]] virtual ControlledInstallerObservation launch(
      ControlledInstallerLaunch const& request) = 0;
  [[nodiscard]] virtual ControlledInstallerCompletionObservation observe_completion(
      ControlledInstallerCompletionRequest const& request) = 0;
  [[nodiscard]] virtual ControlledInstallerTerminationObservation force_terminate(
      ControlledInstallerTerminationRequest const& request) = 0;
};

enum class ControlledProfileReadinessCode {
  registered,
  unavailable,
  failed,
};

struct ControlledProfileReadiness final {
  ControlledProfileReadinessCode code{
      ControlledProfileReadinessCode::unavailable};
  std::string detail;
};

// The static catalog profile is declarative. This separate project-owned port
// reports whether the matching executor is actually registered for its frozen
// profile and baseline; it cannot accept any execution text or path.
class ControlledProfileReadinessPort {
 public:
  virtual ~ControlledProfileReadinessPort() = default;
  [[nodiscard]] virtual ControlledProfileReadiness observe(
      batch_domain::FrozenExecutionProfile const& profile) = 0;
};

enum class InstallVerificationPhase {
  before_launch,
  after_process_exit,
  recovery_read_only,
  after_restart_read_only,
};

enum class InstallVerificationCode {
  installed,
  absent,
  post_action_pending,
  restart_required,
  unknown,
  failed,
};

struct InstallVerificationRequest final {
  InstallationEffectTarget target;
  InstallVerificationPhase phase{InstallVerificationPhase::before_launch};
  std::optional<std::string> opaque_operation_handle;
};

struct InstallVerificationObservation final {
  InstallVerificationCode code{InstallVerificationCode::unknown};
  std::string detail;
};

// Completion is supplied exclusively by this project-owned verifier. Process
// exit is merely an observation phase, never a successful result by itself.
class InstallResultVerifier {
 public:
  virtual ~InstallResultVerifier() = default;
  [[nodiscard]] virtual InstallVerificationObservation verify(
      InstallVerificationRequest const& request) = 0;
};

enum class InstallationFactKind {
  batch_created,
  state_persisted,
  launch_requested,
  verification_observed,
  batch_paused,
  download_paused,
  download_resumed,
  normal_stop_requested,
  forced_termination_confirmation_requested,
  forced_termination_confirmation_cancelled,
  forced_termination_observed,
  normal_close_requested,
  recovery_continued,
  recovery_observed,
  coverage_gap,
};

struct InstallationFact final {
  InstallationFactKind kind{InstallationFactKind::state_persisted};
  std::string batch_id;
  std::string item_id;
  batch_domain::InstallationItemState item_state{
      batch_domain::InstallationItemState::pending};
  ExecutionResult result{ExecutionResult::unknown};
};

// An optional structured fact consumer lets a platform adapter record bounded,
// non-sensitive facts. The service still treats its own ExecutionLog receipt
// as authoritative for durable outcome decisions.
class InstallationFactSink {
 public:
  virtual ~InstallationFactSink() = default;
  virtual void observe(InstallationFact const& fact) noexcept = 0;
};

enum class FrozenBatchPlanAdmissionCode {
  accepted,
  rejected,
};

struct FrozenBatchPlanAdmission final {
  FrozenBatchPlanAdmissionCode code{FrozenBatchPlanAdmissionCode::rejected};
  std::string detail;
};

// Initial and retry batches have distinct admission paths. A retry derives
// from a prior durable plan and must never trigger a live source resolution.
class FrozenBatchPlanAdmissionPort {
 public:
  virtual ~FrozenBatchPlanAdmissionPort() = default;
  [[nodiscard]] virtual FrozenBatchPlanAdmission admit(
      batch_domain::FrozenBatchPlan const& plan) const = 0;
  [[nodiscard]] virtual FrozenBatchPlanAdmission admit_retry(
      batch_domain::FrozenBatchPlan const& plan) const = 0;
};

enum class InstallationBatchActionCode {
  succeeded,
  not_restored,
  rejected,
  occupied,
  read_only,
  persistence_failed,
  outcome_unknown,
  confirmation_required,
  no_active_batch,
  recovery_required,
  blocked,
};

struct InstallationBatchActionResult final {
  InstallationBatchActionCode code{InstallationBatchActionCode::rejected};
  batch_domain::InstallationBatchSnapshot snapshot;
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == InstallationBatchActionCode::succeeded;
  }
};

// This service is the single writer of installation batch business state. It
// holds the durable shared-operation lease over persistence, the effect and
// its committed observation; recovery is deliberately a separate read-only
// command that never starts another item.
class InstallationBatchService final {
 public:
  InstallationBatchService(
      DeviceStateStore& states, SharedOperationOccupancy& occupancy,
      ExecutionLog& log, InstallationDownloadPort& download,
      ControlledInstallerExecutor& executor,
      ControlledProfileReadinessPort& readiness,
      InstallResultVerifier& verifier, InstallationFactSink& facts,
      software_catalog::SoftwareCatalogLifecycle const& catalogs,
      software_selection::SoftwareSelectionLifecycle const& selections,
      restart_resume::RestartResumeService* restart_resume = nullptr);

  InstallationBatchService(
      DeviceStateStore& states, SharedOperationOccupancy& occupancy,
      ExecutionLog& log, InstallationDownloadPort& download,
      ControlledInstallerExecutor& executor,
      ControlledProfileReadinessPort& readiness,
      InstallResultVerifier& verifier, InstallationFactSink& facts,
      FrozenBatchPlanAdmissionPort const& admission,
      restart_resume::RestartResumeService* restart_resume = nullptr);

  ~InstallationBatchService();

  [[nodiscard]] InstallationBatchActionResult restore();
  [[nodiscard]] batch_domain::InstallationBatchSnapshot snapshot() const;
  [[nodiscard]] InstallationBatchActionResult create(
      batch_domain::FrozenBatchPlan plan);
  [[nodiscard]] InstallationBatchActionResult advance();
  [[nodiscard]] InstallationBatchActionResult retry_current(
      batch_domain::FrozenBatchPlan retry_plan);
  [[nodiscard]] InstallationBatchActionResult complete_current_installer_interaction();
  [[nodiscard]] InstallationBatchActionResult confirm_current_complete();
  [[nodiscard]] InstallationBatchActionResult pause_current_download();
  [[nodiscard]] InstallationBatchActionResult resume_current_download();
  [[nodiscard]] InstallationBatchActionResult stop_current();
  [[nodiscard]] InstallationBatchActionResult request_force_termination();
  [[nodiscard]] InstallationBatchActionResult confirm_force_termination();
  [[nodiscard]] InstallationBatchActionResult cancel_force_termination();
  [[nodiscard]] InstallationBatchActionResult request_close();
  [[nodiscard]] InstallationBatchActionResult recover_read_only();
  [[nodiscard]] InstallationBatchActionResult continue_after_recovery();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] char const* to_string(InstallationBatchActionCode value) noexcept;

}  // namespace azzs::application::installation_batch
