#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/clock.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/domain/application_update.hpp"

namespace azzs::application {

using ApplicationBuildIdentity = domain::application_update::BuildIdentity;
using ApplicationReleaseChannel = domain::application_update::ReleaseChannel;
using ApplicationReleaseEdition = domain::application_update::ReleaseEdition;
using ApplicationReleaseForm = domain::application_update::ReleaseForm;
using GithubApplicationAsset = domain::application_update::GithubApplicationAsset;
using GithubApplicationRelease = domain::application_update::GithubApplicationRelease;
using ApplicationUpdateCandidate = domain::application_update::Candidate;

[[nodiscard]] constexpr std::optional<ApplicationReleaseChannel>
parse_application_release_channel(std::string_view value) noexcept {
  return domain::application_update::parse_release_channel(value);
}

enum class GithubReleaseQueryResultCode {
  succeeded,
  unavailable,
  failed,
};

struct GithubReleaseQueryResult final {
  GithubReleaseQueryResultCode code{GithubReleaseQueryResultCode::failed};
  std::vector<GithubApplicationRelease> releases;
  std::string detail;
};

enum class InitializationOperationPhase {
  none,
  active,
  waiting_confirmation,
  waiting_restart,
  pending_recovery,
  manual_source_handoff,
  observation_unavailable,
};

struct InitializationOperationActivitySnapshot final {
  InitializationOperationPhase phase{InitializationOperationPhase::none};
  std::string operation_id;
  std::string detail;
};

class InitializationOperationActivity {
 public:
  virtual ~InitializationOperationActivity() = default;
  [[nodiscard]] virtual InitializationOperationActivitySnapshot observe() = 0;
};

enum class ApplicationUpdateHealthPhase {
  candidate_pending_start_health,
  candidate_start_failed,
  previous_pending_start_health,
  previous_start_failed,
};

struct ApplicationUpdateHealthRecord final {
  ApplicationUpdateHealthPhase phase{
      ApplicationUpdateHealthPhase::candidate_pending_start_health};
  ApplicationBuildIdentity previous;
  ApplicationBuildIdentity target;
  bool retry_attempted{false};
  WallClockTime started_at;

  friend bool operator==(ApplicationUpdateHealthRecord const&,
                         ApplicationUpdateHealthRecord const&) = default;
};

enum class UpdatePlatformResultCode {
  succeeded,
  unavailable,
  failed,
};

struct UpdatePlatformResult final {
  UpdatePlatformResultCode code{UpdatePlatformResultCode::failed};
  std::string detail;
};

struct ApplicationUpdateEffect final {
  UpdatePlatformResultCode code{UpdatePlatformResultCode::failed};
  std::optional<ApplicationUpdateHealthRecord> health;
  std::string detail;
};

struct ApplicationUpdateHealthRead final {
  UpdatePlatformResultCode code{UpdatePlatformResultCode::failed};
  std::optional<ApplicationUpdateHealthRecord> record;
  std::string detail;
};

struct ApplicationUpdateStartHealth final {
  UpdatePlatformResultCode code{UpdatePlatformResultCode::failed};
  bool records_remain_visible{false};
  std::string detail;
};

enum class ManualApplicationDownloadRoute {
  matching_stable_release,
  all_github_releases,
};

struct ManualApplicationDownloadRequest final {
  ManualApplicationDownloadRoute route{
      ManualApplicationDownloadRoute::all_github_releases};
  std::optional<ApplicationUpdateCandidate> candidate;
};

// Automatic checks only read release metadata. They never authorize a
// download, replacement, or channel switch.
enum class ApplicationUpdateCheckSchedule {
  disabled,
  startup,
  daily,
  weekly,
};

enum class ApplicationUpdateCheckOutcome {
  never,
  succeeded_no_update,
  update_available,
  unavailable,
  deferred,
};

struct ApplicationUpdateCheckState final {
  ApplicationUpdateCheckSchedule schedule{
      ApplicationUpdateCheckSchedule::startup};
  std::optional<WallClockTime> last_checked_at;
  ApplicationUpdateCheckOutcome outcome{ApplicationUpdateCheckOutcome::never};
  std::optional<ApplicationUpdateCandidate> candidate;
  std::string detail;

  friend bool operator==(ApplicationUpdateCheckState const&,
                         ApplicationUpdateCheckState const&) = default;
};

struct ApplicationUpdateCheckRead final {
  UpdatePlatformResultCode code{UpdatePlatformResultCode::failed};
  std::optional<ApplicationUpdateCheckState> state;
  std::string detail;
};

class ApplicationUpdateCheckStorage {
 public:
  virtual ~ApplicationUpdateCheckStorage() = default;
  [[nodiscard]] virtual ApplicationUpdateCheckRead read() = 0;
  [[nodiscard]] virtual UpdatePlatformResult write(
      ApplicationUpdateCheckState const& state) = 0;
};

class ApplicationUpdateHealthStorage {
 public:
  virtual ~ApplicationUpdateHealthStorage() = default;
  [[nodiscard]] virtual ApplicationUpdateHealthRead read() = 0;
  [[nodiscard]] virtual UpdatePlatformResult write(
      ApplicationUpdateHealthRecord const& record) = 0;
  [[nodiscard]] virtual UpdatePlatformResult clear() = 0;
};

class ApplicationUpdatePlatform {
 public:
  virtual ~ApplicationUpdatePlatform() = default;

  [[nodiscard]] virtual ApplicationBuildIdentity current_build()
      const noexcept = 0;
  [[nodiscard]] virtual GithubReleaseQueryResult query_releases() = 0;
  [[nodiscard]] virtual ApplicationUpdateEffect download_and_replace(
      ApplicationUpdateCandidate const& candidate) = 0;
  [[nodiscard]] virtual ApplicationUpdateEffect retry_start(
      ApplicationUpdateHealthRecord const& record) = 0;
  [[nodiscard]] virtual ApplicationUpdateEffect rollback(
      ApplicationUpdateHealthRecord const& record) = 0;
  [[nodiscard]] virtual ApplicationUpdateHealthRead read_health_record() = 0;
  [[nodiscard]] virtual UpdatePlatformResult write_health_record(
      ApplicationUpdateHealthRecord const& record) = 0;
  [[nodiscard]] virtual UpdatePlatformResult clear_health_record() = 0;
  [[nodiscard]] virtual ApplicationUpdateStartHealth confirm_started_healthy(
      ApplicationUpdateHealthRecord const& record) = 0;
  [[nodiscard]] virtual UpdatePlatformResult open_manual_download(
      ManualApplicationDownloadRequest const& request) = 0;
};

enum class UpdateState {
  idle,
  latest_stable,
  update_available,
  stable_switch_available,
  no_matching_stable_asset,
  awaiting_user_confirmation,
  deferred_initialization_operation,
  update_unavailable,
  update_failed_restored,
  candidate_pending_start_health,
  awaiting_start_recovery_choice,
  previous_pending_start_health,
  recovery_read_only,
};

struct UpdateSnapshot final {
  UpdateState state{UpdateState::idle};
  ApplicationBuildIdentity current;
  std::optional<ApplicationUpdateCandidate> candidate;
  std::optional<ApplicationUpdateHealthRecord> health;
  std::string detail;
  bool unsigned_artifact_warning{true};
  bool read_only{false};
  bool manual_download_available{true};
  bool diagnostics_available{false};
  bool return_to_current_task_available{false};
  ApplicationUpdateCheckSchedule check_schedule{
      ApplicationUpdateCheckSchedule::startup};
  ApplicationUpdateCheckOutcome last_check_outcome{
      ApplicationUpdateCheckOutcome::never};
  std::optional<WallClockTime> last_checked_at;
  std::string last_check_detail;
};

enum class UpdateUserIntent {
  check_for_update,
  request_update,
  confirm_update,
  cancel_update,
  retry_new_version,
  restore_previous_version,
  confirm_started_healthy,
  open_matching_stable_download,
  open_all_github_releases,
  export_diagnostic,
};

enum class UpdateCommandCode {
  accepted,
  rejected,
  deferred,
  diagnostic_exported,
};

struct UpdateCommandResult final {
  UpdateCommandCode code{UpdateCommandCode::rejected};
  UpdateSnapshot snapshot;
  std::string detail;
};

class ApplicationUpdateLifecycle final {
 public:
  ApplicationUpdateLifecycle(ApplicationUpdatePlatform& platform,
                             InitializationOperationActivity& activity,
                             ExecutionLog& log,
                             Clock const& clock);
  ApplicationUpdateLifecycle(ApplicationUpdatePlatform& platform,
                             InitializationOperationActivity& activity,
                             ExecutionLog& log, Clock const& clock,
                             ApplicationUpdateCheckStorage* check_storage);

  [[nodiscard]] UpdateSnapshot snapshot() const;
  [[nodiscard]] UpdateCommandResult handle(UpdateUserIntent intent);

  // Persist the user's preference. The default schedule is startup for a new
  // installation; setting disabled never affects manual checks.
  [[nodiscard]] UpdateCommandResult set_check_schedule(
      ApplicationUpdateCheckSchedule schedule);

  // Called by the host after the workbench is ready and from a lightweight
  // run-loop poll. It is deliberately typed and has no UI/platform types.
  [[nodiscard]] UpdateCommandResult check_if_due();
  [[nodiscard]] UpdateCommandResult run_automatic_check() {
    return check_if_due();
  }
  [[nodiscard]] UpdateCommandResult on_workbench_ready() {
    return check_if_due();
  }

 private:
  [[nodiscard]] UpdateCommandResult check_for_update(bool automatic = false);
  [[nodiscard]] UpdateCommandResult record_deferred_check(
      std::string detail);
  [[nodiscard]] bool persist_check_state();
  void initialize_from_check_state();
  [[nodiscard]] bool automatic_check_due() const;
  [[nodiscard]] UpdateCommandResult request_update();
  [[nodiscard]] UpdateCommandResult confirm_update();
  [[nodiscard]] UpdateCommandResult retry_new_version();
  [[nodiscard]] UpdateCommandResult restore_previous_version(bool automatic);
  [[nodiscard]] UpdateCommandResult confirm_started_healthy();
  [[nodiscard]] UpdateCommandResult open_manual_download(
      ManualApplicationDownloadRoute route);
  [[nodiscard]] UpdateCommandResult export_diagnostic();
  void initialize_from_health_record();
  void refresh_current();
  void log_event(std::string_view stage, ExecutionResult result,
                 std::string detail = {});
  [[nodiscard]] UpdateCommandResult rejected(std::string detail);
  [[nodiscard]] bool can_confirm_health() const;

  ApplicationUpdatePlatform& platform_;
  InitializationOperationActivity& activity_;
  ExecutionLog& log_;
  Clock const& clock_;
  ApplicationUpdateCheckStorage* check_storage_{nullptr};
  bool startup_check_attempted_{false};
  UpdateSnapshot snapshot_;
};

}  // namespace azzs::application
