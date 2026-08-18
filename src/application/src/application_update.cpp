#include "azzs/application/application_update.hpp"

#include <utility>
#include <vector>

namespace azzs::application {
namespace {

[[nodiscard]] bool is_blocking_initialization_phase(
    InitializationOperationPhase phase) noexcept {
  switch (phase) {
    case InitializationOperationPhase::active:
    case InitializationOperationPhase::waiting_confirmation:
    case InitializationOperationPhase::waiting_restart:
    case InitializationOperationPhase::pending_recovery:
    case InitializationOperationPhase::observation_unavailable:
      return true;
    case InitializationOperationPhase::none:
    case InitializationOperationPhase::manual_source_handoff:
      return false;
  }
  return true;
}

[[nodiscard]] ExecutionResult execution_result(
    UpdatePlatformResultCode code) noexcept {
  switch (code) {
    case UpdatePlatformResultCode::succeeded:
      return ExecutionResult::succeeded;
    case UpdatePlatformResultCode::unavailable:
      return ExecutionResult::unknown;
    case UpdatePlatformResultCode::failed:
      return ExecutionResult::failed;
  }
  return ExecutionResult::failed;
}

}  // namespace

ApplicationUpdateLifecycle::ApplicationUpdateLifecycle(
    ApplicationUpdatePlatform& platform, InitializationOperationActivity& activity,
    ExecutionLog& log, Clock const& clock)
    : platform_(platform), activity_(activity), log_(log), clock_(clock) {
  refresh_current();
  initialize_from_health_record();
}

UpdateSnapshot ApplicationUpdateLifecycle::snapshot() const {
  return snapshot_;
}

UpdateCommandResult ApplicationUpdateLifecycle::handle(UpdateUserIntent intent) {
  if (snapshot_.health.has_value() &&
      (intent == UpdateUserIntent::check_for_update ||
       intent == UpdateUserIntent::request_update ||
       intent == UpdateUserIntent::confirm_update ||
       intent == UpdateUserIntent::cancel_update)) {
    return rejected(
        "application update health must be confirmed before another update action");
  }
  switch (intent) {
    case UpdateUserIntent::check_for_update:
      return check_for_update();
    case UpdateUserIntent::request_update:
      return request_update();
    case UpdateUserIntent::confirm_update:
      return confirm_update();
    case UpdateUserIntent::cancel_update:
      if (snapshot_.state != UpdateState::awaiting_user_confirmation) {
        return rejected("no application update is awaiting confirmation");
      }
      snapshot_.state = UpdateState::idle;
      snapshot_.candidate.reset();
      snapshot_.detail = "user cancelled the application update";
      log_event("cancel-update", ExecutionResult::cancelled, snapshot_.detail);
      return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
    case UpdateUserIntent::retry_new_version:
      return retry_new_version();
    case UpdateUserIntent::restore_previous_version:
      return restore_previous_version(false);
    case UpdateUserIntent::confirm_started_healthy:
      return confirm_started_healthy();
    case UpdateUserIntent::open_matching_stable_download:
      return open_manual_download(
          ManualApplicationDownloadRoute::matching_stable_release);
    case UpdateUserIntent::open_all_github_releases:
      return open_manual_download(ManualApplicationDownloadRoute::all_github_releases);
    case UpdateUserIntent::export_diagnostic:
      return export_diagnostic();
  }
  return rejected("unknown application update intent");
}

void ApplicationUpdateLifecycle::initialize_from_health_record() {
  auto read = platform_.read_health_record();
  if (read.code != UpdatePlatformResultCode::succeeded) {
    snapshot_.detail = read.detail.empty()
                           ? "application update health record is unavailable"
                           : std::move(read.detail);
    snapshot_.state = UpdateState::recovery_read_only;
    snapshot_.read_only = true;
    snapshot_.diagnostics_available = true;
    log_event("read-health-record", execution_result(read.code), snapshot_.detail);
    return;
  }
  if (!read.record.has_value()) {
    return;
  }
  snapshot_.health = std::move(read.record);
  switch (snapshot_.health->phase) {
    case ApplicationUpdateHealthPhase::candidate_pending_start_health:
      snapshot_.state = UpdateState::candidate_pending_start_health;
      snapshot_.detail = "new application version is awaiting its first healthy start";
      break;
    case ApplicationUpdateHealthPhase::candidate_start_failed:
      if (snapshot_.health->retry_attempted) {
        static_cast<void>(restore_previous_version(true));
      } else {
        snapshot_.state = UpdateState::awaiting_start_recovery_choice;
        snapshot_.detail =
            "new application version did not enter the workbench; user choice is required";
      }
      break;
    case ApplicationUpdateHealthPhase::previous_pending_start_health:
      snapshot_.state = UpdateState::previous_pending_start_health;
      snapshot_.detail =
          "restored previous version is awaiting a healthy workbench entry";
      break;
    case ApplicationUpdateHealthPhase::previous_start_failed:
      snapshot_.state = UpdateState::recovery_read_only;
      snapshot_.read_only = true;
      snapshot_.diagnostics_available = true;
      snapshot_.detail =
          "both application versions failed to recover; local records remain read-only";
      break;
  }
}

void ApplicationUpdateLifecycle::refresh_current() {
  snapshot_.current = platform_.current_build();
}

void ApplicationUpdateLifecycle::log_event(std::string_view stage,
                                           ExecutionResult result,
                                           std::string detail) {
  auto const correlation = log_.begin_correlation();
  if (correlation.value.empty()) {
    return;
  }
  std::vector<DiagnosticField> fields{
      {"current_version", snapshot_.current.version,
       DiagnosticValueDisposition::retain},
      {"state", std::to_string(static_cast<int>(snapshot_.state)),
       DiagnosticValueDisposition::retain},
  };
  if (!detail.empty()) {
    fields.push_back(
        {"detail", std::move(detail), DiagnosticValueDisposition::retain});
  }
  static_cast<void>(log_.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "application-update",
          .stage = std::string{stage},
          .result = result,
          .fields = std::move(fields),
      }));
}

bool ApplicationUpdateLifecycle::can_confirm_health() const {
  return snapshot_.health.has_value() &&
         (snapshot_.health->phase ==
              ApplicationUpdateHealthPhase::candidate_pending_start_health ||
          snapshot_.health->phase ==
              ApplicationUpdateHealthPhase::previous_pending_start_health);
}

UpdateCommandResult ApplicationUpdateLifecycle::rejected(std::string detail) {
  snapshot_.detail = detail;
  return {.code = UpdateCommandCode::rejected,
          .snapshot = snapshot_,
          .detail = std::move(detail)};
}

UpdateCommandResult ApplicationUpdateLifecycle::check_for_update() {
  refresh_current();
  if (snapshot_.read_only) {
    return rejected("application update recovery is read-only");
  }
  auto const activity = activity_.observe();
  if (is_blocking_initialization_phase(activity.phase)) {
    snapshot_.state = UpdateState::deferred_initialization_operation;
    snapshot_.return_to_current_task_available = true;
    snapshot_.detail = activity.detail.empty()
                           ? "an initialization operation must finish before update"
                           : std::move(activity.detail);
    log_event("defer-for-initialization-operation", ExecutionResult::cancelled,
              snapshot_.detail);
    return {.code = UpdateCommandCode::deferred, .snapshot = snapshot_};
  }

  snapshot_.return_to_current_task_available = false;
  auto queried = platform_.query_releases();
  if (queried.code != GithubReleaseQueryResultCode::succeeded) {
    snapshot_.state = UpdateState::update_unavailable;
    snapshot_.candidate.reset();
    snapshot_.detail = queried.detail.empty()
                           ? "GitHub release information is unavailable"
                           : std::move(queried.detail);
    log_event("query-releases", ExecutionResult::failed, snapshot_.detail);
    return {.code = UpdateCommandCode::rejected,
            .snapshot = snapshot_,
            .detail = snapshot_.detail};
  }

  snapshot_.candidate = domain::application_update::latest_matching_stable_candidate(
      snapshot_.current, queried.releases);
  snapshot_.detail.clear();
  if (!snapshot_.candidate.has_value()) {
    if (!snapshot_.current.valid()) {
      snapshot_.state = UpdateState::update_unavailable;
      snapshot_.detail =
          "current application identity cannot safely select an automatic update";
      log_event("select-candidate", ExecutionResult::failed, snapshot_.detail);
      return {.code = UpdateCommandCode::rejected,
              .snapshot = snapshot_,
              .detail = snapshot_.detail};
    }
    snapshot_.state =
        domain::application_update::has_matching_formal_stable_asset(
            snapshot_.current, queried.releases)
            ? UpdateState::latest_stable
            : UpdateState::no_matching_stable_asset;
    log_event("select-candidate", ExecutionResult::succeeded,
              snapshot_.state == UpdateState::latest_stable
                  ? "no newer matching formal stable asset"
                  : "no formal stable asset matches the current form");
    return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
  }

  snapshot_.state =
      snapshot_.current.channel == ApplicationReleaseChannel::prerelease
          ? UpdateState::stable_switch_available
          : UpdateState::update_available;
  log_event("select-candidate", ExecutionResult::succeeded,
            "matching formal stable candidate selected");
  return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
}

UpdateCommandResult ApplicationUpdateLifecycle::request_update() {
  if (snapshot_.read_only) {
    return rejected("application update recovery is read-only");
  }
  if (snapshot_.state != UpdateState::update_available &&
      snapshot_.state != UpdateState::stable_switch_available) {
    return rejected("no matching application update is available");
  }
  snapshot_.state = UpdateState::awaiting_user_confirmation;
  snapshot_.detail =
      "the selected release is unsigned and has no project-provided integrity data";
  log_event("await-user-confirmation", ExecutionResult::started, snapshot_.detail);
  return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
}

UpdateCommandResult ApplicationUpdateLifecycle::confirm_update() {
  if (snapshot_.state != UpdateState::awaiting_user_confirmation ||
      !snapshot_.candidate.has_value()) {
    return rejected("application update confirmation is not available");
  }
  auto const activity = activity_.observe();
  if (is_blocking_initialization_phase(activity.phase)) {
    snapshot_.state = UpdateState::deferred_initialization_operation;
    snapshot_.return_to_current_task_available = true;
    snapshot_.detail = activity.detail.empty()
                           ? "an initialization operation began before update"
                           : std::move(activity.detail);
    log_event("defer-before-replace", ExecutionResult::cancelled, snapshot_.detail);
    return {.code = UpdateCommandCode::deferred, .snapshot = snapshot_};
  }

  auto pending = ApplicationUpdateHealthRecord{
      .phase = ApplicationUpdateHealthPhase::candidate_pending_start_health,
      .previous = snapshot_.current,
      .target = snapshot_.candidate->target,
      .started_at = clock_.now(),
  };
  auto persisted = platform_.write_health_record(pending);
  if (persisted.code != UpdatePlatformResultCode::succeeded) {
    snapshot_.detail = persisted.detail.empty()
                           ? "could not persist the application update health record"
                           : std::move(persisted.detail);
    log_event("persist-before-replace", execution_result(persisted.code),
              snapshot_.detail);
    return {.code = UpdateCommandCode::rejected,
            .snapshot = snapshot_,
            .detail = snapshot_.detail};
  }
  auto effect = platform_.download_and_replace(*snapshot_.candidate);
  if (effect.code == UpdatePlatformResultCode::succeeded) {
    snapshot_.health = pending;
    snapshot_.state = UpdateState::candidate_pending_start_health;
    snapshot_.detail = "replacement requested; new version must prove a healthy start";
    log_event("replace", ExecutionResult::succeeded, snapshot_.detail);
    return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
  }

  auto failure = effect.health.value_or(pending);
  failure.phase = ApplicationUpdateHealthPhase::candidate_start_failed;
  auto const failure_persisted = platform_.write_health_record(failure);
  snapshot_.health = failure;
  log_event("replace-failed", execution_result(effect.code), effect.detail);
  if (failure_persisted.code != UpdatePlatformResultCode::succeeded) {
    snapshot_.state = UpdateState::recovery_read_only;
    snapshot_.read_only = true;
    snapshot_.diagnostics_available = true;
    snapshot_.detail = failure_persisted.detail.empty()
                           ? "update failure could not be recorded safely"
                           : std::move(failure_persisted.detail);
    return {.code = UpdateCommandCode::rejected,
            .snapshot = snapshot_,
            .detail = snapshot_.detail};
  }
  auto rollback = platform_.rollback(failure);
  if (rollback.code == UpdatePlatformResultCode::succeeded) {
    auto restored = failure;
    restored.phase = ApplicationUpdateHealthPhase::previous_pending_start_health;
    restored.retry_attempted = false;
    auto const restored_persisted = platform_.write_health_record(restored);
    if (restored_persisted.code != UpdatePlatformResultCode::succeeded) {
      snapshot_.state = UpdateState::recovery_read_only;
      snapshot_.read_only = true;
      snapshot_.diagnostics_available = true;
      snapshot_.detail = restored_persisted.detail.empty()
                             ? "restoration succeeded but its health record could not be saved"
                             : std::move(restored_persisted.detail);
      return {.code = UpdateCommandCode::rejected,
              .snapshot = snapshot_,
              .detail = snapshot_.detail};
    }
    snapshot_.health = std::move(restored);
    snapshot_.state = UpdateState::update_failed_restored;
    snapshot_.detail =
        "update download or replacement failed; previous version restoration was requested";
    log_event("rollback-after-replace-failure", ExecutionResult::succeeded,
              snapshot_.detail);
    return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
  }

  snapshot_.state = UpdateState::recovery_read_only;
  snapshot_.read_only = true;
  snapshot_.diagnostics_available = true;
  snapshot_.detail =
      "application update and automatic restoration both failed; recovery is read-only";
  log_event("rollback-after-replace-failure", execution_result(rollback.code),
            rollback.detail);
  return {.code = UpdateCommandCode::rejected,
          .snapshot = snapshot_,
          .detail = snapshot_.detail};
}

UpdateCommandResult ApplicationUpdateLifecycle::retry_new_version() {
  if (snapshot_.state != UpdateState::awaiting_start_recovery_choice ||
      !snapshot_.health.has_value()) {
    return rejected("new application version retry is not available");
  }
  auto retry = *snapshot_.health;
  retry.retry_attempted = true;
  retry.phase = ApplicationUpdateHealthPhase::candidate_pending_start_health;
  auto persisted = platform_.write_health_record(retry);
  if (persisted.code != UpdatePlatformResultCode::succeeded) {
    return rejected(persisted.detail.empty()
                        ? "could not persist the retry health record"
                        : std::move(persisted.detail));
  }
  auto effect = platform_.retry_start(retry);
  if (effect.code == UpdatePlatformResultCode::succeeded) {
    snapshot_.health = retry;
    snapshot_.state = UpdateState::candidate_pending_start_health;
    snapshot_.detail = "new application version retry is awaiting health";
    log_event("retry-new-version", ExecutionResult::succeeded, snapshot_.detail);
    return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
  }

  retry.phase = ApplicationUpdateHealthPhase::candidate_start_failed;
  auto failure_persisted = platform_.write_health_record(retry);
  snapshot_.health = retry;
  log_event("retry-new-version", execution_result(effect.code), effect.detail);
  if (failure_persisted.code != UpdatePlatformResultCode::succeeded) {
    snapshot_.state = UpdateState::recovery_read_only;
    snapshot_.read_only = true;
    snapshot_.diagnostics_available = true;
    snapshot_.detail = failure_persisted.detail.empty()
                           ? "retry failure could not be recorded safely"
                           : std::move(failure_persisted.detail);
    return {.code = UpdateCommandCode::rejected,
            .snapshot = snapshot_,
            .detail = snapshot_.detail};
  }
  return restore_previous_version(true);
}

UpdateCommandResult ApplicationUpdateLifecycle::restore_previous_version(
    bool automatic) {
  if (!snapshot_.health.has_value()) {
    return rejected("previous application version is unavailable for restoration");
  }
  if (!automatic && snapshot_.state != UpdateState::awaiting_start_recovery_choice) {
    return rejected("previous version restoration requires a failed-start choice");
  }

  auto rollback = platform_.rollback(*snapshot_.health);
  if (rollback.code == UpdatePlatformResultCode::succeeded) {
    auto restored = *snapshot_.health;
    restored.phase = ApplicationUpdateHealthPhase::previous_pending_start_health;
    auto const persisted = platform_.write_health_record(restored);
    if (persisted.code != UpdatePlatformResultCode::succeeded) {
      snapshot_.health = std::move(restored);
      snapshot_.state = UpdateState::recovery_read_only;
      snapshot_.read_only = true;
      snapshot_.diagnostics_available = true;
      snapshot_.detail = persisted.detail.empty()
                             ? "previous version restoration could not be recorded"
                             : std::move(persisted.detail);
      return {.code = UpdateCommandCode::rejected,
              .snapshot = snapshot_,
              .detail = snapshot_.detail};
    }
    snapshot_.health = std::move(restored);
    snapshot_.state = UpdateState::previous_pending_start_health;
    snapshot_.detail = automatic
                           ? "retry failed; previous version restoration is awaiting health"
                           : "previous version restoration is awaiting health";
    log_event(automatic ? "automatic-rollback" : "restore-previous-version",
              ExecutionResult::succeeded, snapshot_.detail);
    return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
  }

  auto failed = *snapshot_.health;
  failed.phase = ApplicationUpdateHealthPhase::previous_start_failed;
  static_cast<void>(platform_.write_health_record(failed));
  snapshot_.health = std::move(failed);
  snapshot_.state = UpdateState::recovery_read_only;
  snapshot_.read_only = true;
  snapshot_.diagnostics_available = true;
  snapshot_.detail = "previous application version could not be restored";
  log_event(automatic ? "automatic-rollback" : "restore-previous-version",
            execution_result(rollback.code), rollback.detail);
  return {.code = UpdateCommandCode::rejected,
          .snapshot = snapshot_,
          .detail = snapshot_.detail};
}

UpdateCommandResult ApplicationUpdateLifecycle::confirm_started_healthy() {
  if (!can_confirm_health() || !snapshot_.health.has_value()) {
    return rejected("there is no application start health record to confirm");
  }
  refresh_current();
  auto confirmed = platform_.confirm_started_healthy(*snapshot_.health);
  if (confirmed.code == UpdatePlatformResultCode::succeeded &&
      confirmed.records_remain_visible) {
    auto const cleared = platform_.clear_health_record();
    if (cleared.code != UpdatePlatformResultCode::succeeded) {
      return rejected(cleared.detail.empty()
                          ? "healthy start was observed but its health record could not be cleared"
                          : std::move(cleared.detail));
    }
    snapshot_.health.reset();
    snapshot_.state = UpdateState::idle;
    snapshot_.detail.clear();
    snapshot_.read_only = false;
    log_event("confirm-started-healthy", ExecutionResult::succeeded);
    return {.code = UpdateCommandCode::accepted, .snapshot = snapshot_};
  }
  snapshot_.detail = confirmed.detail.empty()
                         ? "workbench records are not visible after start"
                         : std::move(confirmed.detail);
  log_event("confirm-started-healthy", execution_result(confirmed.code),
            snapshot_.detail);
  return {.code = UpdateCommandCode::rejected,
          .snapshot = snapshot_,
          .detail = snapshot_.detail};
}

UpdateCommandResult ApplicationUpdateLifecycle::open_manual_download(
    ManualApplicationDownloadRoute route) {
  ManualApplicationDownloadRequest request{.route = route};
  if (route == ManualApplicationDownloadRoute::matching_stable_release) {
    request.candidate = snapshot_.candidate;
  }
  auto opened = platform_.open_manual_download(request);
  log_event("open-manual-download", execution_result(opened.code), opened.detail);
  return {.code = opened.code == UpdatePlatformResultCode::succeeded
                     ? UpdateCommandCode::accepted
                     : UpdateCommandCode::rejected,
          .snapshot = snapshot_,
          .detail = std::move(opened.detail)};
}

UpdateCommandResult ApplicationUpdateLifecycle::export_diagnostic() {
  auto const receipt = log_.export_diagnostic(DiagnosticContext{
      .workbench_build = snapshot_.current.version,
      .release_form =
          snapshot_.current.form == ApplicationReleaseForm::portable
              ? "portable"
              : "installed",
      .process_architecture =
          snapshot_.current.architecture == domain::SystemArchitecture::arm64
              ? "arm64"
              : "x64",
      .package_architecture =
          snapshot_.current.architecture == domain::SystemArchitecture::arm64
              ? "arm64"
              : "x64",
      .coverage_started_at = clock_.now(),
      .coverage_ended_at = clock_.now(),
      .fields =
          {{"update_state", std::to_string(static_cast<int>(snapshot_.state)),
            DiagnosticValueDisposition::retain}},
  });
  if (!receipt.produced) {
    return rejected(receipt.error.empty() ? "diagnostic export failed"
                                          : std::move(receipt.error));
  }
  log_event("export-diagnostic", ExecutionResult::succeeded, receipt.file_name);
  return {.code = UpdateCommandCode::diagnostic_exported,
          .snapshot = snapshot_,
          .detail = std::move(receipt.file_name)};
}

}  // namespace azzs::application
