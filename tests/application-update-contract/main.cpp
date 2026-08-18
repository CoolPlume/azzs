#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/state_application_update_health_storage.hpp"
#include "azzs/application/application_update.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

using azzs::application::ApplicationBuildIdentity;
using azzs::application::ApplicationReleaseChannel;
using azzs::application::ApplicationReleaseEdition;
using azzs::application::ApplicationReleaseForm;
using azzs::application::ApplicationUpdateCandidate;
using azzs::application::ApplicationUpdateEffect;
using azzs::application::ApplicationUpdateHealthPhase;
using azzs::application::ApplicationUpdateHealthRead;
using azzs::application::ApplicationUpdateHealthRecord;
using azzs::application::ApplicationUpdateLifecycle;
using azzs::application::ApplicationUpdatePlatform;
using azzs::application::ApplicationUpdateStartHealth;
using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::GithubApplicationAsset;
using azzs::application::GithubApplicationRelease;
using azzs::application::GithubReleaseQueryResult;
using azzs::application::GithubReleaseQueryResultCode;
using azzs::application::InitializationOperationActivity;
using azzs::application::InitializationOperationActivitySnapshot;
using azzs::application::InitializationOperationPhase;
using azzs::application::ManualApplicationDownloadRequest;
using azzs::application::UpdatePlatformResultCode;
using azzs::application::UpdateState;
using azzs::application::UpdateUserIntent;
using azzs::adapters::infrastructure::StateApplicationUpdateHealthStorage;
using azzs::domain::SystemArchitecture;
using azzs::domain::application_update::VersionComparison;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "application update contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"update-contract-" + std::to_string(next_id_++)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events.push_back(event);
    return {.persisted = true,
            .segment = 1,
            .sequence = static_cast<std::uint64_t>(events.size())};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    return {.cleared = true, .active_segment = 2};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    ++diagnostic_exports;
    return {.produced = true,
            .file_count = 1,
            .file_name = "application-update-diagnostic.txt"};
  }

  std::vector<ExecutionEvent> events;
  std::size_t diagnostic_exports{0};

 private:
  std::uint64_t next_id_{1};
};

class FixedOperationActivity final : public InitializationOperationActivity {
 public:
  [[nodiscard]] InitializationOperationActivitySnapshot observe() override {
    ++observations;
    return snapshot;
  }

  InitializationOperationActivitySnapshot snapshot;
  std::size_t observations{0};
};

class FakeUpdatePlatform final : public ApplicationUpdatePlatform {
 public:
  [[nodiscard]] ApplicationBuildIdentity current_build() const noexcept override {
    return current;
  }

  [[nodiscard]] GithubReleaseQueryResult query_releases() override {
    ++queries;
    return releases;
  }

  [[nodiscard]] ApplicationUpdateEffect download_and_replace(
      ApplicationUpdateCandidate const& candidate) override {
    ++replace_calls;
    replaced = candidate;
    if (replace_result.health.has_value()) {
      health = replace_result.health;
    }
    return replace_result;
  }

  [[nodiscard]] ApplicationUpdateEffect retry_start(
      ApplicationUpdateHealthRecord const& record) override {
    ++retry_start_calls;
    retried = record;
    if (retry_start_result.health.has_value()) {
      health = retry_start_result.health;
    }
    return retry_start_result;
  }

  [[nodiscard]] ApplicationUpdateEffect rollback(
      ApplicationUpdateHealthRecord const& record) override {
    ++rollback_calls;
    rolled_back = record;
    if (rollback_result.health.has_value()) {
      health = rollback_result.health;
    }
    return rollback_result;
  }

  [[nodiscard]] ApplicationUpdateHealthRead read_health_record() override {
    ++health_reads;
    return {.code = health_read_code, .record = health};
  }

  [[nodiscard]] azzs::application::UpdatePlatformResult write_health_record(
      ApplicationUpdateHealthRecord const& record) override {
    ++health_writes;
    if (fail_health_write_number.has_value() &&
        health_writes == *fail_health_write_number) {
      return {.code = UpdatePlatformResultCode::failed,
              .detail = "injected health persistence failure"};
    }
    written_health.push_back(record);
    health = record;
    return health_write_result;
  }

  [[nodiscard]] azzs::application::UpdatePlatformResult clear_health_record()
      override {
    ++health_clears;
    if (health_clear_result.code == UpdatePlatformResultCode::succeeded) {
      health.reset();
    }
    return health_clear_result;
  }

  [[nodiscard]] ApplicationUpdateStartHealth confirm_started_healthy(
      ApplicationUpdateHealthRecord const& record) override {
    ++health_confirmations;
    confirmed = record;
    if (health_confirmation.code == UpdatePlatformResultCode::succeeded &&
        health_confirmation.records_remain_visible) {
      health.reset();
    }
    return health_confirmation;
  }

  [[nodiscard]] azzs::application::UpdatePlatformResult open_manual_download(
      ManualApplicationDownloadRequest const& request) override {
    ++manual_download_requests;
    last_manual_download = request;
    return manual_result;
  }

  ApplicationBuildIdentity current;
  GithubReleaseQueryResult releases;
  ApplicationUpdateEffect replace_result;
  ApplicationUpdateEffect retry_start_result;
  ApplicationUpdateEffect rollback_result;
  UpdatePlatformResultCode health_read_code{UpdatePlatformResultCode::succeeded};
  azzs::application::UpdatePlatformResult health_write_result{
      .code = UpdatePlatformResultCode::succeeded};
  azzs::application::UpdatePlatformResult health_clear_result{
      .code = UpdatePlatformResultCode::succeeded};
  std::optional<ApplicationUpdateHealthRecord> health;
  ApplicationUpdateStartHealth health_confirmation{
      .code = UpdatePlatformResultCode::succeeded,
      .records_remain_visible = true,
  };
  azzs::application::UpdatePlatformResult manual_result{
      .code = UpdatePlatformResultCode::succeeded};
  std::size_t queries{0};
  std::size_t replace_calls{0};
  std::size_t retry_start_calls{0};
  std::size_t rollback_calls{0};
  std::size_t health_reads{0};
  std::size_t health_writes{0};
  std::size_t health_clears{0};
  std::size_t health_confirmations{0};
  std::size_t manual_download_requests{0};
  std::optional<std::size_t> fail_health_write_number;
  std::optional<ApplicationUpdateCandidate> replaced;
  std::optional<ApplicationUpdateHealthRecord> retried;
  std::optional<ApplicationUpdateHealthRecord> rolled_back;
  std::optional<ApplicationUpdateHealthRecord> confirmed;
  std::optional<ManualApplicationDownloadRequest> last_manual_download;
  std::vector<ApplicationUpdateHealthRecord> written_health;
};

[[nodiscard]] ApplicationBuildIdentity build(
    std::string version, ApplicationReleaseChannel channel =
                             ApplicationReleaseChannel::stable,
    SystemArchitecture architecture = SystemArchitecture::x64,
    ApplicationReleaseEdition edition = ApplicationReleaseEdition::standard,
    ApplicationReleaseForm form = ApplicationReleaseForm::portable) {
  return {.version = std::move(version),
          .channel = channel,
          .architecture = architecture,
          .edition = edition,
          .form = form};
}

[[nodiscard]] GithubApplicationAsset asset(
    std::string id, ApplicationBuildIdentity target) {
  return {.asset_id = std::move(id), .target = std::move(target)};
}

[[nodiscard]] GithubApplicationRelease release(
    std::string id, std::string tag, std::string title,
    std::vector<GithubApplicationAsset> assets, bool draft = false,
    bool prerelease = false) {
  return {.release_id = std::move(id),
          .tag_name = std::move(tag),
          .title = std::move(title),
          .draft = draft,
          .prerelease = prerelease,
          .assets = std::move(assets)};
}

[[nodiscard]] ApplicationUpdateHealthRecord pending_health(
    ApplicationBuildIdentity previous, ApplicationBuildIdentity target,
    ApplicationUpdateHealthPhase phase, bool retry_attempted = false) {
  return {.phase = phase,
          .previous = std::move(previous),
          .target = std::move(target),
          .retry_attempted = retry_attempted,
          .started_at = azzs::application::WallClockTime{
              std::chrono::milliseconds{1'786'422'400'000}}};
}

[[nodiscard]] ApplicationUpdateLifecycle make_lifecycle(
    FakeUpdatePlatform& platform, FixedOperationActivity& activity,
    RecordingLog& log, FixedClock& clock) {
  return ApplicationUpdateLifecycle{platform, activity, log, clock};
}

[[nodiscard]] bool has_stage(RecordingLog const& log, std::string_view stage) {
  for (auto const& event : log.events) {
    if (event.component == "application-update" && event.stage == stage) {
      return true;
    }
  }
  return false;
}

static_assert(
    !std::is_invocable_v<decltype(&ApplicationUpdateLifecycle::handle),
                         ApplicationUpdateLifecycle&, std::string_view>,
    "application updates must not accept user-provided URLs, commands, paths, or artifacts");

[[nodiscard]] bool filtering_pairing_and_version_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.0.0");
  platform.releases = {
      .code = GithubReleaseQueryResultCode::succeeded,
      .releases =
          {
              release("draft", "v9.0.0", "正式版",
                      {asset("draft-asset", build("9.0.0"))}, true),
              release("pre", "v8.0.0-beta.1", "Beta",
                      {asset("pre-asset", build("8.0.0"))}, false, true),
              release("nightly", "v7.0.0", "Nightly 构建",
                      {asset("nightly-asset", build("7.0.0"))}),
              release("wrong-edition", "v2.0.0", "正式版",
                      {asset("rescue-asset",
                             build("2.0.0", ApplicationReleaseChannel::stable,
                                   SystemArchitecture::x64,
                                   ApplicationReleaseEdition::rescue))}),
              release("wrong-architecture", "v1.9.0", "正式版",
                      {asset("arm64-asset",
                             build("1.9.0", ApplicationReleaseChannel::stable,
                                   SystemArchitecture::arm64))}),
              release("stable", "v1.2.0", "正式版",
                      {asset("x64-portable", build("1.2.0"))}),
          }};
  FixedOperationActivity activity;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};

  auto lifecycle = make_lifecycle(platform, activity, log, clock);
  auto const checked = lifecycle.handle(UpdateUserIntent::check_for_update);
  bool passed = expect(checked.snapshot.state == UpdateState::update_available,
                       "only the latest formal stable matching asset may be an automatic candidate");
  passed &= expect(checked.snapshot.candidate.has_value() &&
                       checked.snapshot.candidate->target.version == "1.2.0" &&
                       checked.snapshot.candidate->asset_id == "x64-portable",
                   "candidate pairing must preserve release, architecture, edition, and form");
  passed &= expect(checked.snapshot.unsigned_artifact_warning,
                   "the snapshot must disclose unsigned assets without project integrity data");
  passed &= expect(platform.queries == 1,
                   "the core must obtain release facts only through its typed platform seam");

  platform.current =
      build("9.0.0-beta.1", ApplicationReleaseChannel::prerelease);
  auto prerelease_lifecycle = make_lifecycle(platform, activity, log, clock);
  auto const prerelease_checked =
      prerelease_lifecycle.handle(UpdateUserIntent::check_for_update);
  passed &= expect(
      prerelease_checked.snapshot.state == UpdateState::stable_switch_available &&
          prerelease_checked.snapshot.candidate.has_value() &&
          prerelease_checked.snapshot.candidate->target.version == "1.2.0",
      "a prerelease build may only expose a user-triggered switch to the latest matching stable build");

  platform.current = build("1.2.0");
  auto current_lifecycle = make_lifecycle(platform, activity, log, clock);
  passed &= expect(
      current_lifecycle.handle(UpdateUserIntent::check_for_update)
              .snapshot.state == UpdateState::latest_stable,
      "equal stable versions must not become an update candidate");

  platform.current = build("1.0.0");
  platform.releases.releases = {
      release("wrong-shape", "v3.0.0", "正式版",
              {asset("arm64-asset", build("3.0.0",
                                          ApplicationReleaseChannel::stable,
                                          SystemArchitecture::arm64))})};
  auto invalid_lifecycle = make_lifecycle(platform, activity, log, clock);
  passed &= expect(
      invalid_lifecycle.handle(UpdateUserIntent::check_for_update)
              .snapshot.state == UpdateState::no_matching_stable_asset,
      "a missing matching asset must explicitly refuse automatic update");
  return passed;
}

[[nodiscard]] bool prerelease_identity_and_ordering_contract() {
  auto const stable = build("0.9.0");
  auto const prerelease =
      build("0.9.0-beta.1", ApplicationReleaseChannel::prerelease);
  auto const mismatched_stable =
      build("0.9.0-beta.1", ApplicationReleaseChannel::stable);
  auto const mismatched_prerelease =
      build("0.9.0", ApplicationReleaseChannel::prerelease);
  auto const malformed_prerelease =
      build("0.9.0-beta.", ApplicationReleaseChannel::prerelease);

  bool passed = expect(stable.valid() && prerelease.valid(),
                       "stable and prerelease build identities must be valid when their channels match");
  passed &= expect(!mismatched_stable.valid() && !mismatched_prerelease.valid() &&
                       !malformed_prerelease.valid(),
                   "build identities must reject a version and release channel mismatch or malformed prerelease");
  passed &= expect(
      azzs::domain::application_update::compare_versions("0.9.0-beta.1",
                                                          "0.9.0-beta.2") ==
          VersionComparison::higher &&
          azzs::domain::application_update::compare_versions("0.9.0-beta.2",
                                                              "0.9.0") ==
              VersionComparison::higher &&
          azzs::domain::application_update::compare_versions("0.9.0",
                                                              "0.9.0-beta.2") ==
              VersionComparison::lower,
      "semantic prerelease ordering must place beta.1 before beta.2 and both before the stable version");

  auto const prerelease_asset = asset(
      "prerelease-asset",
      build("0.9.0-beta.1", ApplicationReleaseChannel::prerelease));
  auto const prerelease_release = release(
      "prerelease-asset-release", "v0.9.0", "正式版", {prerelease_asset});
  passed &= expect(
      !azzs::domain::application_update::is_formal_stable_release(
          prerelease_release),
      "a prerelease asset must not be treated as a formal stable release even without a GitHub prerelease flag");
  return passed;
}

[[nodiscard]] bool deferral_and_confirmation_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.0.0");
  platform.releases = {
      .code = GithubReleaseQueryResultCode::succeeded,
      .releases = {release("stable", "v1.1.0", "正式版",
                           {asset("asset", build("1.1.0"))})}};
  platform.replace_result = {
      .code = UpdatePlatformResultCode::succeeded,
      .health = pending_health(platform.current, build("1.1.0"),
                               ApplicationUpdateHealthPhase::
                                   candidate_pending_start_health)};
  FixedOperationActivity activity;
  activity.snapshot.phase = InitializationOperationPhase::pending_recovery;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  auto lifecycle = make_lifecycle(platform, activity, log, clock);

  bool passed = expect(
      lifecycle.handle(UpdateUserIntent::check_for_update).snapshot.state ==
          UpdateState::deferred_initialization_operation &&
          platform.queries == 0,
      "active, waiting, or recoverable initialization work must defer updates before release access");

  activity.snapshot.phase = InitializationOperationPhase::manual_source_handoff;
  auto const checked = lifecycle.handle(UpdateUserIntent::check_for_update);
  passed &= expect(checked.snapshot.state == UpdateState::update_available,
                   "manual source handoff must not block an update check");
  auto const requested = lifecycle.handle(UpdateUserIntent::request_update);
  passed &= expect(
      requested.snapshot.state == UpdateState::awaiting_user_confirmation &&
          platform.replace_calls == 0,
      "an available update must still wait for explicit user confirmation");
  auto const confirmed = lifecycle.handle(UpdateUserIntent::confirm_update);
  passed &= expect(
      confirmed.snapshot.state == UpdateState::candidate_pending_start_health &&
          platform.replace_calls == 1 && platform.health_writes == 1 &&
          has_stage(log, "replace"),
      "only confirmation may persist health and request the typed download and replacement operation");
  return passed;
}

[[nodiscard]] bool replace_failure_rolls_back_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.0.0");
  platform.releases = {
      .code = GithubReleaseQueryResultCode::succeeded,
      .releases = {release("stable", "v1.1.0", "正式版",
                           {asset("asset", build("1.1.0"))})}};
  auto failure_record = pending_health(
      platform.current, build("1.1.0"),
      ApplicationUpdateHealthPhase::candidate_pending_start_health);
  platform.replace_result = {
      .code = UpdatePlatformResultCode::failed,
      .health = failure_record,
      .detail = "injected replacement failure"};
  platform.rollback_result = {
      .code = UpdatePlatformResultCode::succeeded,
      .health = pending_health(
          build("1.1.0"), platform.current,
          ApplicationUpdateHealthPhase::previous_pending_start_health)};
  FixedOperationActivity activity;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  auto lifecycle = make_lifecycle(platform, activity, log, clock);

  static_cast<void>(lifecycle.handle(UpdateUserIntent::check_for_update));
  static_cast<void>(lifecycle.handle(UpdateUserIntent::request_update));
  auto const result = lifecycle.handle(UpdateUserIntent::confirm_update);
  bool passed = expect(
      result.snapshot.state == UpdateState::update_failed_restored &&
          platform.replace_calls == 1 && platform.rollback_calls == 1 &&
          platform.health_writes >= 2 && has_stage(log, "replace-failed") &&
          has_stage(log, "rollback-after-replace-failure"),
      "download or replacement failure must automatically attempt to restore the previous version and log both facts");
  auto const blocked = lifecycle.handle(UpdateUserIntent::check_for_update);
  passed &= expect(
      blocked.code == azzs::application::UpdateCommandCode::rejected &&
          blocked.snapshot.state == UpdateState::update_failed_restored &&
          platform.queries == 1,
      "pending previous-version health must block another update check");
  auto const healthy = lifecycle.handle(UpdateUserIntent::confirm_started_healthy);
  passed &= expect(
      healthy.snapshot.state == UpdateState::idle && !platform.health.has_value(),
      "a healthy restored previous version may complete recovery");
  return passed;
}

[[nodiscard]] bool startup_retry_and_automatic_rollback_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.1.0");
  auto const failed_once = pending_health(
      build("1.0.0"), platform.current,
      ApplicationUpdateHealthPhase::candidate_start_failed);
  platform.health = failed_once;
  platform.retry_start_result = {
      .code = UpdatePlatformResultCode::succeeded,
      .health = pending_health(
          build("1.0.0"), platform.current,
          ApplicationUpdateHealthPhase::candidate_pending_start_health, true)};
  platform.rollback_result = {
      .code = UpdatePlatformResultCode::succeeded,
      .health = pending_health(
          platform.current, build("1.0.0"),
          ApplicationUpdateHealthPhase::previous_pending_start_health, true)};
  FixedOperationActivity activity;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  auto lifecycle = make_lifecycle(platform, activity, log, clock);

  bool passed = expect(
      lifecycle.snapshot().state == UpdateState::awaiting_start_recovery_choice &&
          platform.rollback_calls == 0,
      "the first failed start of a new version must wait for the user's retry or restore choice");
  auto const retried = lifecycle.handle(UpdateUserIntent::retry_new_version);
  passed &= expect(
      retried.snapshot.state == UpdateState::candidate_pending_start_health &&
          platform.retry_start_calls == 1 && platform.replace_calls == 0 &&
          platform.health_writes == 1,
      "retry must be an explicit user action that does not re-download or replace the application");

  platform.health = pending_health(
      build("1.0.0"), platform.current,
      ApplicationUpdateHealthPhase::candidate_start_failed, true);
  auto retry_failed_lifecycle = make_lifecycle(platform, activity, log, clock);
  passed &= expect(
      retry_failed_lifecycle.snapshot().state ==
              UpdateState::previous_pending_start_health &&
          platform.rollback_calls == 1,
      "a failed retry must automatically restore the previous version");

  auto const healthy =
      retry_failed_lifecycle.handle(UpdateUserIntent::confirm_started_healthy);
  passed &= expect(
      healthy.snapshot.state == UpdateState::idle &&
          platform.health_confirmations == 1 &&
          platform.health_clears == 1 && !platform.health.has_value(),
      "only a confirmed healthy previous version with visible records may clear recovery");
  return passed;
}

[[nodiscard]] bool dual_failure_is_read_only_and_manual_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.0.0");
  platform.health = pending_health(
      build("1.1.0"), platform.current,
      ApplicationUpdateHealthPhase::previous_start_failed, true);
  FixedOperationActivity activity;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  auto lifecycle = make_lifecycle(platform, activity, log, clock);

  bool passed = expect(
      lifecycle.snapshot().state == UpdateState::recovery_read_only &&
          lifecycle.snapshot().read_only &&
          lifecycle.snapshot().health.has_value(),
      "a failed recovered version must enter a persistent read-only fault recovery state");
  passed &= expect(
      lifecycle.handle(UpdateUserIntent::check_for_update).snapshot.state ==
          UpdateState::recovery_read_only,
      "fault recovery must reject more automatic update activity");
  auto const manual =
      lifecycle.handle(UpdateUserIntent::open_all_github_releases);
  passed &= expect(manual.code == azzs::application::UpdateCommandCode::accepted &&
                       platform.manual_download_requests == 1 &&
                       lifecycle.snapshot().state == UpdateState::recovery_read_only,
                   "opening controlled manual download routes must not clear fault recovery");
  auto const diagnostic =
      lifecycle.handle(UpdateUserIntent::export_diagnostic);
  passed &= expect(
      diagnostic.code == azzs::application::UpdateCommandCode::diagnostic_exported &&
          log.diagnostic_exports == 1 &&
          lifecycle.snapshot().state == UpdateState::recovery_read_only,
      "diagnostic export must remain available without declaring recovery complete");
  return passed;
}

[[nodiscard]] bool first_start_health_requires_visible_records_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.1.0");
  platform.health = pending_health(
      build("1.0.0"), platform.current,
      ApplicationUpdateHealthPhase::candidate_pending_start_health);
  platform.health_confirmation.records_remain_visible = false;
  FixedOperationActivity activity;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  auto lifecycle = make_lifecycle(platform, activity, log, clock);

  auto const invisible =
      lifecycle.handle(UpdateUserIntent::confirm_started_healthy);
  bool passed = expect(
      invisible.snapshot.state == UpdateState::candidate_pending_start_health &&
          platform.health.has_value(),
      "showing a window without original records remaining visible must not confirm health");
  platform.health_confirmation.records_remain_visible = true;
  auto const visible = lifecycle.handle(UpdateUserIntent::confirm_started_healthy);
  passed &= expect(visible.snapshot.state == UpdateState::idle &&
                       !platform.health.has_value(),
                   "a real healthy entry with visible records may complete first-start confirmation");
  return passed;
}

[[nodiscard]] bool retry_failure_persistence_contract() {
  FakeUpdatePlatform platform;
  platform.current = build("1.1.0");
  platform.health = pending_health(
      build("1.0.0"), platform.current,
      ApplicationUpdateHealthPhase::candidate_start_failed);
  platform.retry_start_result = {
      .code = UpdatePlatformResultCode::failed,
      .detail = "injected retry failure"};
  platform.fail_health_write_number = 2;
  FixedOperationActivity activity;
  RecordingLog log;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  auto lifecycle = make_lifecycle(platform, activity, log, clock);

  auto const retried = lifecycle.handle(UpdateUserIntent::retry_new_version);
  return expect(
      retried.code == azzs::application::UpdateCommandCode::rejected &&
          retried.snapshot.state == UpdateState::recovery_read_only &&
          retried.snapshot.read_only && platform.rollback_calls == 0,
      "a failed retry whose failure fact cannot persist must fail closed before rollback");
}

[[nodiscard]] bool persistent_health_storage_contract() {
  using namespace std::chrono_literals;

  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{1'786'422'400'000ms}};
  azzs::application::DeviceStateStore first_states{files, clock};
  azzs::application::DeviceStateStore second_states{files, clock};
  StateApplicationUpdateHealthStorage first{first_states};
  StateApplicationUpdateHealthStorage second{second_states};
  auto record = pending_health(
      build("1.0.0"), build("1.1.0"),
      ApplicationUpdateHealthPhase::candidate_pending_start_health);

  bool passed = expect(
      first.write(record).code == UpdatePlatformResultCode::succeeded,
      "health storage must persist a valid replacement health record");
  auto observed = second.read();
  passed &= expect(
      observed.code == UpdatePlatformResultCode::succeeded &&
          observed.record.has_value() && *observed.record == record,
      "health storage must be observable from another application instance");
  passed &= expect(
      second.clear().code == UpdatePlatformResultCode::succeeded &&
          first.read().code == UpdatePlatformResultCode::succeeded &&
          !first.read().record.has_value(),
      "clearing health storage must remove the pending update fact");

  passed &= expect(
      first.write(record).code == UpdatePlatformResultCode::succeeded,
      "health storage must support a later replacement health record");
  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"application-update-health"});
  files.corrupt(key, azzs::application::StateFileSlot::current);
  files.corrupt(key, azzs::application::StateFileSlot::previous);
  auto damaged_read = first.read();
  passed &= expect(
      damaged_read.code == UpdatePlatformResultCode::failed &&
          first.write(record).code == UpdatePlatformResultCode::failed,
      "dual health state corruption must fail closed for read and write");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= filtering_pairing_and_version_contract();
  passed &= prerelease_identity_and_ordering_contract();
  passed &= deferral_and_confirmation_contract();
  passed &= replace_failure_rolls_back_contract();
  passed &= startup_retry_and_automatic_rollback_contract();
  passed &= dual_failure_is_read_only_and_manual_contract();
  passed &= first_start_health_requires_visible_records_contract();
  passed &= retry_failure_persistence_contract();
  passed &= persistent_health_storage_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "application update contract passed\n";
  return EXIT_SUCCESS;
}
