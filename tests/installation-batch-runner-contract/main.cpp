#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/installation_batch.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace app = azzs::application;
namespace batch_app = azzs::application::installation_batch;
namespace batch = azzs::domain::installation_batch;
namespace catalog = azzs::domain::software_catalog;
namespace cache = azzs::domain::offline_package_cache;
namespace selection = azzs::domain::software_selection;
namespace architecture = azzs::domain::architecture_selection;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "installation batch runner contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingLog final : public app::ExecutionLog {
 public:
  [[nodiscard]] app::CorrelationId begin_correlation() override {
    return {.value = "installation-batch-contract-" + std::to_string(next_++)};
  }

  [[nodiscard]] app::ExecutionLogReceipt append(
      app::CorrelationId const&, app::ExecutionEvent const& event) override {
    events.push_back(event);
    if (fail_next_append) {
      fail_next_append = false;
      return {.persisted = false, .error = "injected log append failure"};
    }
    return {.persisted = true, .segment = 1, .sequence = next_++};
  }

  [[nodiscard]] app::ExecutionLogClearReceipt clear() override {
    return {.cleared = true};
  }

  [[nodiscard]] app::DiagnosticExportReceipt export_diagnostic(
      app::DiagnosticContext const&) override {
    return {.produced = true};
  }

  bool fail_next_append{false};
  std::vector<app::ExecutionEvent> events;

 private:
  std::uint64_t next_{1};
};

class ScriptedDownload final : public batch_app::InstallationDownloadPort {
 public:
  [[nodiscard]] batch_app::InstallationDownloadObservation advance(
      batch_app::InstallationEffectTarget const& target) override {
    ++advance_calls;
    targets.push_back(target);
    return take(advances, {.code = batch_app::InstallationDownloadCode::completed});
  }

  [[nodiscard]] batch_app::InstallationDownloadObservation stop(
      batch_app::InstallationEffectTarget const& target) override {
    ++stop_calls;
    targets.push_back(target);
    return take(stops, {.code = batch_app::InstallationDownloadCode::paused});
  }

  std::vector<batch_app::InstallationDownloadObservation> advances;
  std::vector<batch_app::InstallationDownloadObservation> stops;
  std::vector<batch_app::InstallationEffectTarget> targets;
  std::size_t advance_calls{};
  std::size_t stop_calls{};

 private:
  static batch_app::InstallationDownloadObservation take(
      std::vector<batch_app::InstallationDownloadObservation>& values,
      batch_app::InstallationDownloadObservation fallback) {
    if (values.empty()) {
      return fallback;
    }
    auto value = std::move(values.front());
    values.erase(values.begin());
    return value;
  }
};

class ScriptedExecutor final : public batch_app::ControlledInstallerExecutor {
 public:
  [[nodiscard]] batch_app::ControlledInstallerObservation launch(
      batch_app::ControlledInstallerLaunch const& request) override {
    ++launch_calls;
    targets.push_back(request.target);
    if (launches.empty()) {
      return {.code = batch_app::InstallerLaunchCode::started,
              .opaque_operation_handle = "operation-" + std::to_string(launch_calls)};
    }
    auto value = std::move(launches.front());
    launches.erase(launches.begin());
    return value;
  }

  std::vector<batch_app::ControlledInstallerObservation> launches;
  std::vector<batch_app::InstallationEffectTarget> targets;
  std::size_t launch_calls{};
};

class ScriptedVerifier final : public batch_app::InstallResultVerifier {
 public:
  [[nodiscard]] batch_app::InstallVerificationObservation verify(
      batch_app::InstallVerificationRequest const& request) override {
    ++verify_calls;
    requests.push_back(request);
    if (observations.empty()) {
      return {.code = batch_app::InstallVerificationCode::unknown,
              .detail = "no scripted verification fact"};
    }
    auto value = std::move(observations.front());
    observations.erase(observations.begin());
    return value;
  }

  std::vector<batch_app::InstallVerificationObservation> observations;
  std::vector<batch_app::InstallVerificationRequest> requests;
  std::size_t verify_calls{};
};

class RegisteredReadiness final : public batch_app::ControlledProfileReadinessPort {
 public:
  [[nodiscard]] batch_app::ControlledProfileReadiness observe(
      batch::FrozenExecutionProfile const& profile) override {
    profiles.push_back(profile);
    return {.code = registered ? batch_app::ControlledProfileReadinessCode::registered
                               : batch_app::ControlledProfileReadinessCode::unavailable,
            .detail = registered ? "" : "injected executor unavailability"};
  }

  bool registered{true};
  std::vector<batch::FrozenExecutionProfile> profiles;
};

class RecordingFactSink final : public batch_app::InstallationFactSink {
 public:
  void observe(batch_app::InstallationFact const& fact) noexcept override {
    facts.push_back(fact);
  }

  std::vector<batch_app::InstallationFact> facts;
};

class CountingAdmission final : public batch_app::FrozenBatchPlanAdmissionPort {
 public:
  [[nodiscard]] batch_app::FrozenBatchPlanAdmission admit(
      batch::FrozenBatchPlan const& plan) const override {
    ++calls;
    plans.push_back(plan);
    return {.code = accepts ? batch_app::FrozenBatchPlanAdmissionCode::accepted
                            : batch_app::FrozenBatchPlanAdmissionCode::rejected,
            .detail = accepts ? "" : "injected frozen-plan rejection"};
  }

  bool accepts{true};
  mutable std::size_t calls{};
  mutable std::vector<batch::FrozenBatchPlan> plans;
};

struct Fixture final {
  Fixture()
      : clock{app::WallClockTime{std::chrono::milliseconds{1'786'422'400'000}}},
        states{files, clock}, tokens{"installation-lease-"},
        occupancy{occupancy_storage, tokens},
        service{states, occupancy, log, download, executor, readiness, verifier, facts,
                admission} {}

  [[nodiscard]] bool restore() {
    return expect(service.restore().succeeded(), "service restore must initialize writable state");
  }

  azzs::testing::InMemoryStateFileSystem files;
  azzs::testing::FixedClock clock;
  app::DeviceStateStore states;
  azzs::testing::InMemoryOperationOccupancyStorage occupancy_storage;
  azzs::testing::SequenceLeaseTokenSource tokens;
  app::SharedOperationOccupancy occupancy;
  RecordingLog log;
  ScriptedDownload download;
  ScriptedExecutor executor;
  RegisteredReadiness readiness;
  ScriptedVerifier verifier;
  RecordingFactSink facts;
  CountingAdmission admission;
  batch_app::InstallationBatchService service;
};

[[nodiscard]] batch::FrozenExecutionProfile fixture_profile(
    catalog::InstallationCompletionBoundary boundary =
        catalog::InstallationCompletionBoundary::post_install_then_result_detection,
    catalog::ResultDetectionStrategy detection =
        catalog::ResultDetectionStrategy::project_owned_presence_probe) {
  return {.profile_id = "controlled-profile-v1",
          .baseline = {.id = "controlled-baseline", .version = "1"},
          .executor =
              catalog::ControlledWindowsExecutionKind::project_owned_windows_executor,
          .execution = catalog::WindowsExecutionReadiness::project_executor_registered,
          .completion_boundary = boundary,
          .post_install = catalog::PostInstallBehavior::none,
          .restart = boundary ==
                             catalog::InstallationCompletionBoundary::
                                 post_install_then_restart_verification
                         ? catalog::RestartVerification::required_after_restart
                         : catalog::RestartVerification::not_required,
          .result_detection = detection,
          .interaction_scope =
              catalog::InstallerInteractionScope::non_identity_preferences_only,
          .interaction_disposition =
              catalog::InteractionDisposition::controlled_automatic};
}

[[nodiscard]] batch::FrozenInstallationItem fixture_item(
    std::string id, batch::FrozenExecutionProfile profile = fixture_profile()) {
  selection::ResolvedPackage package{
      .candidate = {.software_id = id,
                    .architecture = architecture::PackageArchitecture::x64,
                    .version = "1.2.3",
                    .identity = id + "-1.2.3-x64"},
      .package_type = selection::PackageType::full_package,
      .complete_package = true,
  };
  return {
      .item_id = id,
      .source = {.software_id = id,
                 .declared_purpose = catalog::SourcePurpose::primary,
                 .declared_address = "https://example.test/" + id,
                 .version = "1.2.3",
                 .actual_address = "https://download.example.test/" + id + ".msi",
                 .hosting_mechanism = "controlled-release",
                 .branch = "stable",
                 .packages = {package},
                 .resolved_at_milliseconds = 1,
                 .capability_version = "resolver-v1"},
      .selected_package = package,
      .execution_profile = std::move(profile),
      .resource_kind = batch::FrozenResourceKind::controlled_download,
      .cache_asset = {.identity = {.software_id = id,
                                   .version = "1.2.3",
                                   .architecture = cache::CacheArchitecture::x64,
                                   .source_identity = "opaque-" + id},
                      .kind = cache::CacheAssetKind::full_package},
      .cache_root = {.kind = cache::CacheLocationKind::system_directory,
                     .id = "machine-cache"},
  };
}

[[nodiscard]] batch::FrozenBatchPlan fixture_plan(
    std::vector<batch::FrozenInstallationItem> items,
    std::string batch_id = "batch-1") {
  return {.batch_id = std::move(batch_id),
          .correlation_id = "correlation-1",
          .catalog = {.raw_catalog_bytes = "[catalog]",
                      .content_identity = "catalog-content",
                      .application_id = "app-1",
                      .schema_version = 1,
                      .revision = 1,
                      .release_state = catalog::ReleaseState::draft,
                      .local_trial = true},
          .items = std::move(items),
          .frozen_at_milliseconds = 1};
}

[[nodiscard]] bool create(Fixture& fixture, batch::FrozenBatchPlan plan) {
  if (!fixture.restore()) {
    return false;
  }
  return expect(fixture.service.create(std::move(plan)).succeeded(),
                "initial frozen batch creation must succeed");
}

[[nodiscard]] bool advance(Fixture& fixture, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    auto const result = fixture.service.advance();
    if (!expect(result.succeeded(), "scripted batch advance must commit")) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] batch::InstallationItemProgress const& current_item(
    batch_app::InstallationBatchActionResult const& result) {
  return result.snapshot.active->items.front();
}

template <typename Value>
concept ExposesForbiddenExecutionInput =
    requires(Value const& value) { value.url; } ||
    requires(Value const& value) { value.path; } ||
    requires(Value const& value) { value.command; } ||
    requires(Value const& value) { value.selector; } ||
    requires(Value const& value) { value.source; } ||
    requires(Value const& value) { value.selected_package; };

template <typename Port>
concept AcceptsFrozenInstallationItem = requires(
    Port& port, batch::FrozenInstallationItem const& item) { port.advance(item); };

template <typename Port>
concept LaunchesFrozenInstallationItem = requires(
    Port& port, batch::FrozenInstallationItem const& item) { port.launch(item); };

template <typename Port>
concept VerifiesFrozenInstallationItem = requires(
    Port& port, batch::FrozenInstallationItem const& item) { port.verify(item); };

static_assert(!ExposesForbiddenExecutionInput<batch_app::InstallationEffectTarget>);
static_assert(!ExposesForbiddenExecutionInput<batch_app::ControlledInstallerLaunch>);
static_assert(!ExposesForbiddenExecutionInput<batch_app::InstallVerificationRequest>);
static_assert(!AcceptsFrozenInstallationItem<batch_app::InstallationDownloadPort>);
static_assert(!LaunchesFrozenInstallationItem<batch_app::ControlledInstallerExecutor>);
static_assert(!VerifiesFrozenInstallationItem<batch_app::InstallResultVerifier>);

[[nodiscard]] bool effect_boundary_is_closed() {
  auto const item = fixture_item("editor");
  batch_app::InstallationEffectTarget const target{
      .item_id = item.item_id,
      .cache_asset = item.cache_asset,
      .cache_root = item.cache_root,
      .execution_profile = item.execution_profile,
      .opaque_item_handle = "batch-item:" + item.cache_asset.identity.stable_key(),
  };
  return expect(target.valid(), "effect target must carry only valid frozen identities") &&
         expect(target.opaque_item_handle.find("http") == std::string::npos,
                "effect target must not expose a source address");
}

[[nodiscard]] bool strict_serial_unknown_post_action_blocks_next_item() {
  Fixture fixture;
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::post_action_pending,
       .detail = "child setup still running"},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  auto plan = fixture_plan({fixture_item("editor"), fixture_item("browser")});
  if (!create(fixture, std::move(plan)) || !advance(fixture, 4)) {
    return false;
  }
  auto const pending = fixture.service.snapshot();
  auto const blocked = fixture.service.advance();
  return expect(pending.active->items.front().state ==
                    batch::InstallationItemState::result_confirmation_pending,
                "unknown post action must become result confirmation pending") &&
         expect(blocked.code == batch_app::InstallationBatchActionCode::blocked,
                "unknown post action must block the batch") &&
         expect(fixture.executor.launch_calls == 1,
                "the next item must not launch before the current result is known") &&
         expect(fixture.download.advance_calls == 1,
                "the next item must not pre-download while the current result is unknown") &&
         expect(fixture.executor.targets.front().valid(),
                "executor must receive a valid effect target only");
}

[[nodiscard]] bool source_failure_does_not_bypass_current_item() {
  Fixture fixture;
  fixture.verifier.observations = {{.code = batch_app::InstallVerificationCode::absent}};
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::source_invalid,
       .detail = "frozen source became unavailable"},
  };
  if (!create(fixture, fixture_plan({fixture_item("editor"), fixture_item("browser")})) ||
      !advance(fixture, 2)) {
    return false;
  }
  auto const blocked = fixture.service.advance();
  auto const snapshot = fixture.service.snapshot();
  return expect(snapshot.active->items.front().state ==
                    batch::InstallationItemState::source_invalid,
                "source failure must stay on the current item") &&
         expect(blocked.code == batch_app::InstallationBatchActionCode::blocked,
                "source failure must require retry or stop") &&
         expect(fixture.executor.launch_calls == 0,
                "a source-invalid item must not launch an installer") &&
         expect(fixture.download.advance_calls == 1,
                "a source-invalid item must not start the next download");
}

[[nodiscard]] bool explicit_restart_profile_is_the_only_restart_barrier() {
  Fixture restart_fixture;
  restart_fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::installed},
      {.code = batch_app::InstallVerificationCode::installed},
  };
  restart_fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  auto restart_profile = fixture_profile(
      catalog::InstallationCompletionBoundary::post_install_then_restart_verification);
  if (!create(restart_fixture,
              fixture_plan({fixture_item("editor", std::move(restart_profile))})) ||
      !advance(restart_fixture, 4)) {
    return false;
  }
  auto const barrier = restart_fixture.service.snapshot();
  auto const blocked = restart_fixture.service.advance();
  auto const recovered = restart_fixture.service.recover_read_only();

  Fixture ordinary_fixture;
  ordinary_fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::restart_required},
  };
  ordinary_fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  if (!create(ordinary_fixture, fixture_plan({fixture_item("editor")})) ||
      !advance(ordinary_fixture, 4)) {
    return false;
  }
  auto const ordinary = ordinary_fixture.service.snapshot();
  return expect(barrier.active->items.front().state ==
                    batch::InstallationItemState::waiting_restart,
                "only an explicit restart profile may create a restart barrier") &&
         expect(blocked.code == batch_app::InstallationBatchActionCode::blocked,
                "restart barrier must block the batch") &&
         expect(recovered.succeeded() &&
                    current_item(recovered).state == batch::InstallationItemState::succeeded,
                "post-restart read-only detection must establish completion") &&
         expect(ordinary.active->items.front().state ==
                    batch::InstallationItemState::result_confirmation_pending,
                "an unexpected restart observation must remain pending confirmation");
}

[[nodiscard]] bool interaction_and_result_confirmation_are_disjoint() {
  using Command = batch::InstallationItemCommand;
  using State = batch::InstallationItemState;
  Fixture fixture;
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::post_action_pending},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  fixture.executor.launches = {
      {.code = batch_app::InstallerLaunchCode::interaction_required,
       .detail = "official account step required"},
  };
  if (!create(fixture, fixture_plan({fixture_item("editor")})) || !advance(fixture, 3)) {
    return false;
  }
  auto const interaction = fixture.service.snapshot();
  auto const wrong_confirmation = fixture.service.confirm_current_complete();
  auto const completed_interaction = fixture.service.complete_current_installer_interaction();
  auto const after_interaction = fixture.service.advance();
  auto const pending = fixture.service.snapshot();
  auto const wrong_interaction = fixture.service.complete_current_installer_interaction();
  auto const probe_confirmation = fixture.service.confirm_current_complete();

  Fixture confirmation_fixture;
  confirmation_fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::unknown},
  };
  confirmation_fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  auto confirmation_profile = fixture_profile(
      catalog::InstallationCompletionBoundary::post_install_then_result_detection,
      catalog::ResultDetectionStrategy::user_confirmation_only);
  if (!create(confirmation_fixture,
              fixture_plan({fixture_item("confirmable", std::move(confirmation_profile))})) ||
      !advance(confirmation_fixture, 4)) {
    return false;
  }
  auto const user_confirmation = confirmation_fixture.service.confirm_current_complete();
  return expect(batch::command_allowed(State::installer_interaction_pending,
                                       Command::user_complete_installer_interaction) &&
                    !batch::command_allowed(State::installer_interaction_pending,
                                            Command::user_complete_confirmation) &&
                    !batch::command_allowed(State::installer_interaction_pending,
                                            Command::retry),
                "installer interaction must have its own command set") &&
         expect(batch::command_allowed(State::result_confirmation_pending,
                                       Command::user_complete_confirmation) &&
                    !batch::command_allowed(State::result_confirmation_pending,
                                            Command::user_complete_installer_interaction) &&
                    !batch::command_allowed(State::result_confirmation_pending,
                                            Command::retry),
                "result confirmation must not reuse the interaction commands") &&
         expect(interaction.active->items.front().state == State::installer_interaction_pending,
                "interaction-required launch must pause at the interaction state") &&
         expect(wrong_confirmation.code == batch_app::InstallationBatchActionCode::rejected &&
                    completed_interaction.succeeded() && after_interaction.succeeded(),
                "only explicit interaction completion may leave the interaction state") &&
         expect(pending.active->items.front().state == State::result_confirmation_pending &&
                    wrong_interaction.code == batch_app::InstallationBatchActionCode::rejected &&
                    probe_confirmation.code == batch_app::InstallationBatchActionCode::rejected,
                "probe profiles must remain verifier-owned after unknown post action") &&
         expect(user_confirmation.succeeded() &&
                    current_item(user_confirmation).state == State::succeeded,
                "only a user-confirmation profile may record explicit completion") &&
         expect(fixture.executor.launch_calls == 1,
                "interaction completion must not relaunch the installer");
}

[[nodiscard]] bool retry_creates_a_new_immutable_single_item_plan() {
  Fixture fixture;
  auto original = fixture_plan({fixture_item("editor"), fixture_item("browser")}, "batch-original");
  auto expected_retry_item = original.items.front();
  expected_retry_item.dependencies.clear();
  fixture.verifier.observations = {{.code = batch_app::InstallVerificationCode::absent}};
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::failed, .detail = "download failed"},
  };
  if (!create(fixture, original) || !advance(fixture, 2)) {
    return false;
  }
  auto const blocked = fixture.service.advance();
  auto const retried = fixture.service.retry_current();
  auto const& active = *retried.snapshot.active;
  return expect(blocked.code == batch_app::InstallationBatchActionCode::blocked,
                "a failed item must not bypass to a later item") &&
         expect(retried.succeeded(), "failed item retry must create a new durable batch") &&
         expect(fixture.admission.calls == 1,
                "retry must not re-admit or live-resolve the frozen plan") &&
         expect(retried.snapshot.history.size() == 1 &&
                    retried.snapshot.history.front().plan == original,
                "retry must preserve the complete original history") &&
         expect(active.plan.retry_of_batch_id == std::optional{original.batch_id} &&
                    active.plan.items.size() == 1 &&
                    active.plan.items.front() == expected_retry_item &&
                    active.plan.catalog == original.catalog,
                "retry must copy the frozen catalog, source, package, profile and cache") &&
         expect(active.items.front().attempt == 1,
                "retry must start a distinct attempt without mutating prior history");
}

[[nodiscard]] bool failures_close_the_batch_before_any_effect() {
  Fixture state_failure;
  state_failure.files.fail_next(azzs::testing::StateFileOperation::write,
                                app::StateFileSlot::candidate);
  if (!state_failure.restore()) {
    return false;
  }
  auto const state_result = state_failure.service.create(fixture_plan({fixture_item("editor")}));

  Fixture log_failure;
  log_failure.log.fail_next_append = true;
  if (!log_failure.restore()) {
    return false;
  }
  auto const log_result = log_failure.service.create(fixture_plan({fixture_item("editor")}));

  Fixture occupancy_failure;
  occupancy_failure.occupancy_storage.fail_writes(true);
  if (!occupancy_failure.restore()) {
    return false;
  }
  auto const occupancy_result =
      occupancy_failure.service.create(fixture_plan({fixture_item("editor")}));

  auto failed_closed = [](batch_app::InstallationBatchActionResult const& result) {
    return result.snapshot.active.has_value() &&
           result.snapshot.active->state == batch::InstallationBatchState::failed_closed;
  };
  return expect(!state_result.succeeded() && failed_closed(state_result) &&
                    state_failure.executor.launch_calls == 0,
                "state persistence failure must fail closed before an executor call") &&
         expect(!log_result.succeeded() && failed_closed(log_result) &&
                    log_failure.executor.launch_calls == 0,
                "log persistence failure must fail closed before an executor call") &&
         expect(!occupancy_result.succeeded() && failed_closed(occupancy_result) &&
                    occupancy_failure.executor.launch_calls == 0,
                "occupancy failure must fail closed before an executor call");
}

[[nodiscard]] bool restore_and_read_only_recovery_never_start_effects() {
  Fixture fixture;
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::unknown},
      {.code = batch_app::InstallVerificationCode::unknown},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  if (!create(fixture, fixture_plan({fixture_item("editor"), fixture_item("browser")})) ||
      !advance(fixture, 4)) {
    return false;
  }
  auto const download_before = fixture.download.advance_calls;
  auto const executor_before = fixture.executor.launch_calls;
  auto const verifier_before = fixture.verifier.verify_calls;
  batch_app::InstallationBatchService reopened{
      fixture.states, fixture.occupancy, fixture.log, fixture.download, fixture.executor,
      fixture.readiness, fixture.verifier, fixture.facts, fixture.admission};
  auto const restored = reopened.restore();
  auto const restore_effect_free =
      fixture.download.advance_calls == download_before &&
      fixture.executor.launch_calls == executor_before &&
      fixture.verifier.verify_calls == verifier_before;
  auto const recovery = reopened.recover_read_only();
  auto const blocked = reopened.advance();
  return expect(restored.succeeded(), "restore must decode without launching any effect") &&
         expect(restore_effect_free,
                "restore must not call download, executor or verifier") &&
         expect(recovery.succeeded() && fixture.download.advance_calls == download_before &&
                    fixture.executor.launch_calls == executor_before &&
                    fixture.verifier.verify_calls == verifier_before + 1,
                "read-only recovery may verify but must never start download or execution") &&
         expect(current_item(recovery).state == batch::InstallationItemState::result_confirmation_pending &&
                    blocked.code == batch_app::InstallationBatchActionCode::recovery_required,
                "unknown recovery result must remain pending and cannot auto-continue");
}

[[nodiscard]] bool external_handoff_is_rejected_before_admission() {
  Fixture fixture;
  auto item = fixture_item("editor");
  item.selected_package.package_type = selection::PackageType::external_handoff;
  if (!fixture.restore()) {
    return false;
  }
  auto const rejected = fixture.service.create(fixture_plan({std::move(item)}));
  return expect(rejected.code == batch_app::InstallationBatchActionCode::rejected,
                "external handoff must never become an installation batch item") &&
         expect(fixture.admission.calls == 0,
                "invalid external handoff plans must not reach the admission port");
}

}  // namespace

int main() {
  return effect_boundary_is_closed() &&
                 strict_serial_unknown_post_action_blocks_next_item() &&
                 source_failure_does_not_bypass_current_item() &&
                 explicit_restart_profile_is_the_only_restart_barrier() &&
                 interaction_and_result_confirmation_are_disjoint() &&
                 retry_creates_a_new_immutable_single_item_plan() &&
                 failures_close_the_batch_before_any_effect() &&
                 restore_and_read_only_recovery_never_start_effects() &&
                 external_handoff_is_rejected_before_admission()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
