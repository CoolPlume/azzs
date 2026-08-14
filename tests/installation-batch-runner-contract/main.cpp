#include <chrono>
#include <cstddef>
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

[[nodiscard]] bool contains_text(azzs::domain::StateBytes const& bytes,
                                 std::string_view needle) {
  if (needle.empty() || bytes.size() < needle.size()) {
    return false;
  }
  for (std::size_t offset = 0; offset <= bytes.size() - needle.size(); ++offset) {
    bool matches = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      if (bytes[offset + index] != std::byte{static_cast<unsigned char>(needle[index])}) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
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

  [[nodiscard]] batch_app::ControlledInstallerCompletionObservation
  observe_completion(batch_app::ControlledInstallerCompletionRequest const& request) override {
    ++completion_calls;
    completion_requests.push_back(request);
    if (completions.empty()) {
      return {.code = batch_app::InstallerCompletionCode::completed};
    }
    auto value = std::move(completions.front());
    completions.erase(completions.begin());
    return value;
  }

  std::vector<batch_app::ControlledInstallerObservation> launches;
  std::vector<batch_app::ControlledInstallerCompletionObservation> completions;
  std::vector<batch_app::InstallationEffectTarget> targets;
  std::vector<batch_app::ControlledInstallerCompletionRequest> completion_requests;
  std::size_t launch_calls{};
  std::size_t completion_calls{};
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

  [[nodiscard]] batch_app::FrozenBatchPlanAdmission admit_retry(
      batch::FrozenBatchPlan const& plan) const override {
    ++retry_calls;
    retry_plans.push_back(plan);
    return {.code = retry_accepts ? batch_app::FrozenBatchPlanAdmissionCode::accepted
                                  : batch_app::FrozenBatchPlanAdmissionCode::rejected,
            .detail = retry_accepts ? "" : "injected frozen-retry-plan rejection"};
  }

  bool accepts{true};
  bool retry_accepts{true};
  mutable std::size_t calls{};
  mutable std::size_t retry_calls{};
  mutable std::vector<batch::FrozenBatchPlan> plans;
  mutable std::vector<batch::FrozenBatchPlan> retry_plans;
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
        catalog::ResultDetectionStrategy::project_owned_presence_probe,
    catalog::PostInstallBehavior post_install = catalog::PostInstallBehavior::none) {
  return {.profile_id = "controlled-profile-v1",
          .baseline = {.id = "controlled-baseline", .version = "1"},
          .executor =
              catalog::ControlledWindowsExecutionKind::project_owned_windows_executor,
          .execution = catalog::WindowsExecutionReadiness::project_executor_registered,
          .completion_boundary = boundary,
          .post_install = post_install,
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

[[nodiscard]] bool installer_completion_and_nonterminal_lease_are_required() {
  Fixture fixture;
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::installed},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  fixture.executor.completions = {
      {.code = batch_app::InstallerCompletionCode::running,
       .detail = "installer still running"},
      {.code = batch_app::InstallerCompletionCode::completed},
  };
  if (!create(fixture, fixture_plan({fixture_item("editor"), fixture_item("browser")})) ||
      !advance(fixture, 3)) {
    return false;
  }
  auto const running = fixture.service.advance();
  auto const occupancy_while_running = fixture.occupancy.inspect();
  auto const after_completion = fixture.service.advance();
  auto const before_next = fixture.service.snapshot();
  auto const terminal = fixture.service.stop_current();
  auto const occupancy_after_terminal = fixture.occupancy.inspect();
  return expect(running.succeeded() && current_item(running).state ==
                                       batch::InstallationItemState::installer_running,
                "an observed running installer must stay running") &&
         expect(fixture.verifier.verify_calls == 2,
                "the verifier must run only for the initial absence and explicit completion") &&
         expect(occupancy_while_running.code == app::OccupancyResultCode::observed &&
                    occupancy_while_running.current.has_value(),
                "a nonterminal installer state must retain its controlled occupancy") &&
         expect(after_completion.succeeded() &&
                    current_item(after_completion).state ==
                        batch::InstallationItemState::succeeded &&
                    before_next.active->items[1].state ==
                        batch::InstallationItemState::pending &&
                    fixture.executor.launch_calls == 1,
                "explicit completion must not auto-start a later installer") &&
         expect(terminal.succeeded() &&
                    terminal.snapshot.active->state == batch::InstallationBatchState::stopped &&
                    !terminal.snapshot.active->active_lease.has_value() &&
                    occupancy_after_terminal.code == app::OccupancyResultCode::observed &&
                    !occupancy_after_terminal.current.has_value(),
                "only terminal batch completion may release the durable lease");
}

[[nodiscard]] bool controlled_post_install_requires_completion_before_verification() {
  Fixture fixture;
  auto profile = fixture_profile(
      catalog::InstallationCompletionBoundary::post_install_then_result_detection,
      catalog::ResultDetectionStrategy::project_owned_presence_probe,
      catalog::PostInstallBehavior::controlled_preferences);
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::installed},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  fixture.executor.completions = {
      {.code = batch_app::InstallerCompletionCode::completed,
       .post_install = batch_app::PostInstallCompletionCode::unknown,
       .detail = "controlled post-install status is unknown"},
      {.code = batch_app::InstallerCompletionCode::completed,
       .post_install = batch_app::PostInstallCompletionCode::completed},
  };
  if (!create(fixture,
              fixture_plan({fixture_item("editor", std::move(profile)),
                            fixture_item("browser")})) ||
      !advance(fixture, 3)) {
    return false;
  }

  auto const pending = fixture.service.advance();
  auto const occupancy_while_pending = fixture.occupancy.inspect();
  auto const verifier_calls_while_pending = fixture.verifier.verify_calls;
  auto const completed = fixture.service.advance();
  return expect(pending.succeeded(),
                "unknown controlled post-install behavior must persist a conservative state") &&
         expect(current_item(pending).state ==
                    batch::InstallationItemState::result_confirmation_pending,
                "unknown controlled post-install behavior must remain pending") &&
         expect(verifier_calls_while_pending == 1,
                "missing controlled post-install completion must not call the verifier") &&
         expect(pending.snapshot.active->items[1].state ==
                    batch::InstallationItemState::pending,
                "missing controlled post-install completion must not start the next item") &&
         expect(occupancy_while_pending.current.has_value(),
                "missing controlled post-install completion must retain the lease") &&
         expect(completed.succeeded() &&
                    current_item(completed).state == batch::InstallationItemState::succeeded &&
                    completed.snapshot.active->items[1].state ==
                        batch::InstallationItemState::pending &&
                    fixture.verifier.verify_calls == 2 && fixture.executor.launch_calls == 1,
                "only explicit process and post-install completion facts may authorize result detection");
}

[[nodiscard]] bool source_failure_only_blocks_dependent_items() {
  Fixture fixture;
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::absent},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::source_invalid,
       .detail = "frozen source became unavailable"},
  };
  if (!create(fixture, fixture_plan({fixture_item("editor"), fixture_item("browser")})) ||
      !advance(fixture, 2)) {
    return false;
  }
  auto const continued = fixture.service.advance();
  auto const snapshot = fixture.service.snapshot();
  return expect(snapshot.active->items.front().state ==
                    batch::InstallationItemState::source_invalid,
                "source failure must stay on the current item") &&
         expect(continued.succeeded() && snapshot.active->items[1].state ==
                                            batch::InstallationItemState::downloading,
                "a source-invalid item must not block an unrelated later item") &&
         expect(fixture.executor.launch_calls == 0,
                "a source-invalid item must not launch an installer") &&
         expect(fixture.download.advance_calls == 1,
                "the runner must not pre-download an unrelated later item");
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
  fixture.verifier.observations = {{.code = batch_app::InstallVerificationCode::absent}};
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::failed, .detail = "download failed"},
  };
  if (!create(fixture, original)) {
    return false;
  }
  auto const initial = fixture.service.snapshot();
  if (!initial.active.has_value()) {
    return false;
  }
  auto const expected_history_plan = initial.active->plan;
  auto retry_profile = fixture_profile();
  retry_profile.profile_id = "controlled-profile-v2";
  retry_profile.baseline.version = "2";
  auto expected_retry_item = fixture_item("editor", std::move(retry_profile));
  expected_retry_item.source.declared_address = "https://new.example.test/editor";
  expected_retry_item.source.actual_address = "https://cdn.new.example.test/editor-2.0.0.msi";
  expected_retry_item.source.version = "2.0.0";
  expected_retry_item.source.packages.front().candidate.version = "2.0.0";
  expected_retry_item.selected_package.candidate.version = "2.0.0";
  expected_retry_item.cache_asset.identity.version = "2.0.0";
  expected_retry_item.cache_asset.identity.source_identity = "opaque-editor-v2";
  auto retry_plan = fixture_plan({expected_retry_item}, "batch-retry-fresh");
  retry_plan.correlation_id = "correlation-retry-fresh";
  retry_plan.retry_of_batch_id = original.batch_id;
  retry_plan.catalog.raw_catalog_bytes = "[fresh-catalog]";
  retry_plan.catalog.content_identity = "fresh-catalog-content";
  retry_plan.catalog.revision = 2;
  retry_plan.frozen_at_milliseconds = 2;
  if (!advance(fixture, 2)) {
    return false;
  }
  auto const retried = fixture.service.retry_current(std::move(retry_plan));
  auto const& active = *retried.snapshot.active;
  return expect(retried.succeeded(), "failed item retry must create a new durable batch") &&
         expect(fixture.admission.calls == 1 && fixture.admission.retry_calls == 1,
                "retry must use frozen retry admission without live re-admission") &&
         expect(retried.snapshot.history.size() == 1 &&
                    retried.snapshot.history.front().plan == expected_history_plan,
                "retry must preserve the complete original history projection") &&
         expect(active.plan.retry_of_batch_id == std::optional{original.batch_id} &&
                    active.plan.items.size() == 1 &&
                    active.plan.batch_id == "batch-retry-fresh" &&
                    active.plan.catalog.revision == 2 &&
                    active.plan.items.front().execution_profile.profile_id ==
                        "controlled-profile-v2" &&
                    active.plan.items.front().execution_profile.baseline.version == "2" &&
                    active.plan.items.front().selected_package.candidate.version == "2.0.0" &&
                    active.plan.items.front().cache_asset.identity.version == "2.0.0" &&
                    active.plan.items.front().cache_asset.identity.source_identity ==
                        "opaque-editor-v2",
                "retry must adopt the distinct upstream catalog, source, package and profile snapshot") &&
         expect(active.items.front().attempt == 1,
                "retry must start a distinct attempt without mutating prior history");
}

[[nodiscard]] bool retry_requires_admission_and_readiness_before_replacing_failed_batch() {
  Fixture fixture;
  auto original = fixture_plan({fixture_item("editor")}, "retry-gate-original");
  fixture.verifier.observations = {{.code = batch_app::InstallVerificationCode::absent}};
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::failed, .detail = "download failed"},
  };
  if (!create(fixture, original) || !advance(fixture, 2)) {
    return false;
  }
  auto const failed = fixture.service.snapshot();
  if (!failed.active.has_value()) {
    return false;
  }
  auto retry_plan = [&](std::string batch_id, batch::FrozenExecutionProfile profile) {
    auto plan = fixture_plan({fixture_item("editor", std::move(profile))}, std::move(batch_id));
    plan.retry_of_batch_id = original.batch_id;
    return plan;
  };
  fixture.admission.retry_accepts = false;
  auto const unregistered = fixture.service.retry_current(
      retry_plan("retry-gate-unregistered", fixture_profile()));

  fixture.admission.retry_accepts = true;
  fixture.readiness.registered = false;
  auto const unavailable = fixture.service.retry_current(
      retry_plan("retry-gate-unavailable", fixture_profile()));

  fixture.readiness.registered = true;
  auto declaration_only = fixture_profile();
  declaration_only.execution = catalog::WindowsExecutionReadiness::declaration_only;
  auto const declaration = fixture.service.retry_current(
      retry_plan("retry-gate-declaration-only", std::move(declaration_only)));

  auto preserves_failed_batch = [&failed](batch_app::InstallationBatchActionResult const& result) {
    return result.code == batch_app::InstallationBatchActionCode::rejected &&
           result.snapshot.active.has_value() &&
           result.snapshot.active->plan == failed.active->plan &&
           result.snapshot.active->state == failed.active->state &&
           result.snapshot.history == failed.history;
  };
  return expect(preserves_failed_batch(unregistered) && preserves_failed_batch(unavailable) &&
                    preserves_failed_batch(declaration),
                "rejected retry admission or readiness must preserve the failed batch") &&
         expect(fixture.admission.calls == 1 && fixture.admission.retry_calls == 3,
                "retry must use its dedicated frozen admission for every replacement attempt") &&
         expect(fixture.readiness.profiles.size() == 2,
                "only an admitted registered-profile retry may query readiness") &&
         expect(fixture.executor.launch_calls == 0,
                "rejected retry admission or readiness must not launch an executor");
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

  auto safely_blocked = [](batch_app::InstallationBatchActionResult const& result) {
    return result.snapshot.active.has_value() &&
           (result.snapshot.active->state == batch::InstallationBatchState::failed_closed ||
            result.snapshot.active->state == batch::InstallationBatchState::recovery_required) &&
           result.snapshot.active->last_transition.outcome ==
               batch::DurableTransitionOutcome::outcome_unknown;
  };
  return expect(!state_result.succeeded() && safely_blocked(state_result) &&
                    state_failure.executor.launch_calls == 0,
                "state persistence failure must block before an executor call") &&
         expect(!log_result.succeeded() && safely_blocked(log_result) &&
                    log_failure.executor.launch_calls == 0,
                "log persistence failure must block before an executor call") &&
         expect(!occupancy_result.succeeded() && safely_blocked(occupancy_result) &&
                    occupancy_failure.executor.launch_calls == 0,
                "occupancy failure must block before an executor call");
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

[[nodiscard]] bool interrupted_launch_is_recovered_without_a_second_effect() {
  Fixture fixture;
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::unknown},
  };
  fixture.download.advances = {
      {.code = batch_app::InstallationDownloadCode::completed},
  };
  fixture.executor.completions = {
      {.code = batch_app::InstallerCompletionCode::unknown,
       .detail = "launch completion cannot be established"},
  };
  if (!create(fixture, fixture_plan({fixture_item("editor")})) || !advance(fixture, 3)) {
    return false;
  }
  auto const launch_calls_before_restart = fixture.executor.launch_calls;
  batch_app::InstallationBatchService reopened{
      fixture.states, fixture.occupancy, fixture.log, fixture.download, fixture.executor,
      fixture.readiness, fixture.verifier, fixture.facts, fixture.admission};
  auto const restored = reopened.restore();
  auto const blocked = reopened.advance();
  auto const recovered = reopened.recover_read_only();
  return expect(restored.succeeded() &&
                    restored.snapshot.active->state ==
                        batch::InstallationBatchState::recovery_required &&
                    blocked.code == batch_app::InstallationBatchActionCode::recovery_required,
                "a restored launch request must require read-only recovery") &&
         expect(recovered.succeeded() && fixture.executor.launch_calls == launch_calls_before_restart &&
                    current_item(recovered).state ==
                        batch::InstallationItemState::result_confirmation_pending,
                "recovery must observe an uncertain launch without issuing it again");
}

[[nodiscard]] bool public_and_durable_batch_state_is_redacted() {
  constexpr std::string_view raw_catalog = "raw-catalog-sentinel";
  constexpr std::string_view source_url = "https://private.example.invalid/source-sentinel";
  constexpr std::string_view actual_url = "https://cdn.private.example.invalid/setup-sentinel.msi";
  constexpr std::string_view package_identity = "package-identity-sentinel";
  constexpr std::string_view cache_source = "opaque-cache-source-sentinel";
  constexpr std::string_view cache_root = "controlled-cache-root-sentinel";
  constexpr std::string_view opaque_handle = "opaque-handle-sentinel";
  constexpr std::string_view native_detail = "C:\\private\\setup-sentinel.msi";
  constexpr std::string_view lease_token = "installation-lease-1";

  Fixture fixture;
  auto plan = fixture_plan({fixture_item("editor")}, "privacy-batch");
  plan.catalog.raw_catalog_bytes = std::string{raw_catalog};
  plan.catalog.content_identity = "catalog-identity-sentinel";
  plan.catalog.application_id = "application-id-sentinel";
  auto& item = plan.items.front();
  item.source.declared_address = std::string{source_url};
  item.source.actual_address = std::string{actual_url};
  item.selected_package.candidate.identity = std::string{package_identity};
  item.source.packages.front().candidate.identity = std::string{package_identity};
  item.cache_asset.identity.source_identity = std::string{cache_source};
  item.cache_root.id = std::string{cache_root};
  fixture.verifier.observations = {{.code = batch_app::InstallVerificationCode::absent}};
  fixture.download.advances = {{.code = batch_app::InstallationDownloadCode::completed,
                                .detail = std::string{native_detail}}};
  fixture.executor.launches = {{.code = batch_app::InstallerLaunchCode::started,
                                .opaque_operation_handle = std::string{opaque_handle},
                                .detail = std::string{native_detail}}};
  fixture.executor.completions = {{.code = batch_app::InstallerCompletionCode::failed,
                                   .detail = std::string{native_detail}}};

  if (!fixture.restore()) {
    return false;
  }
  auto const created = fixture.service.create(std::move(plan));
  auto const verified = fixture.service.advance();
  auto const downloaded = fixture.service.advance();
  auto const launched = fixture.service.advance();
  auto const failed = fixture.service.advance();
  auto retry_item = fixture_item("editor");
  retry_item.cache_asset.identity.source_identity = std::string{cache_source};
  retry_item.cache_root.id = std::string{cache_root};
  auto retry_plan = fixture_plan({std::move(retry_item)}, "privacy-retry-batch");
  retry_plan.retry_of_batch_id = "privacy-batch";
  auto const retried = fixture.service.retry_current(std::move(retry_plan));
  if (!expect(created.succeeded(), "privacy fixture creation must succeed") ||
      !expect(verified.succeeded(), "privacy fixture pre-launch verification must succeed") ||
      !expect(downloaded.succeeded(), "privacy fixture download must succeed") ||
      !expect(launched.succeeded(), "privacy fixture launch must succeed") ||
      !expect(failed.succeeded(), "privacy fixture failed completion must persist") ||
      !expect(retried.succeeded(), "privacy fixture retry must succeed") ||
      !expect(retried.snapshot.active.has_value() && retried.snapshot.history.size() == 1,
              "retry must expose both the active projection and a history projection")) {
    return false;
  }

  auto const& active = *retried.snapshot.active;
  auto const& active_item = active.plan.items.front();
  auto const& history = retried.snapshot.history.front();
  auto const& historical_item = history.plan.items.front();
  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"installation-batch"});
  auto const current = fixture.files.raw_file(key, app::StateFileSlot::current);
  auto const previous = fixture.files.raw_file(key, app::StateFileSlot::previous);
  auto const candidate = fixture.files.raw_file(key, app::StateFileSlot::candidate);
  std::vector<std::string_view> const forbidden{raw_catalog, source_url, actual_url,
                                                  package_identity, opaque_handle, native_detail,
                                                  lease_token};
  auto no_forbidden_bytes = [&forbidden](
                                std::optional<azzs::domain::StateBytes> const& bytes) {
    if (!bytes.has_value()) {
      return true;
    }
    for (auto const marker : forbidden) {
      if (contains_text(*bytes, marker)) {
        return false;
      }
    }
    return true;
  };
  auto no_forbidden_message = [&forbidden](std::string const& message) {
    for (auto const marker : forbidden) {
      if (message.find(marker) != std::string::npos) {
        return false;
      }
    }
    return true;
  };

  return expect(active.plan.catalog.raw_catalog_bytes == "[redacted]" &&
                    active_item.source.declared_address == "redacted-source" &&
                    active_item.source.actual_address == "redacted-source" &&
                    active_item.selected_package.candidate.identity == "redacted-package" &&
                    active_item.cache_asset.identity.source_identity == cache_source &&
                    active_item.cache_root.id == cache_root &&
                    !active.active_lease.has_value(),
                "public active snapshots must redact private values without changing opaque cache identity") &&
         expect(history.plan.catalog.raw_catalog_bytes == "[redacted]" &&
                    historical_item.source.declared_address == "redacted-source" &&
                    historical_item.source.actual_address == "redacted-source" &&
                    historical_item.selected_package.candidate.identity == "redacted-package" &&
                    historical_item.cache_asset.identity.source_identity == cache_source &&
                    historical_item.cache_root.id == cache_root &&
                    !history.items.front().opaque_installer_handle.has_value() &&
                    history.items.front().detail.empty() &&
                    history.reason == "batch-history-retained",
                "public history must redact plan, handle, detail and reason") &&
         expect(no_forbidden_message(created.message) && no_forbidden_message(verified.message) &&
                    no_forbidden_message(downloaded.message) && no_forbidden_message(launched.message) &&
                    no_forbidden_message(failed.message) && no_forbidden_message(retried.message),
                "action results must not return private execution details") &&
         expect(current.has_value() && no_forbidden_bytes(current) &&
                    no_forbidden_bytes(previous) && no_forbidden_bytes(candidate) &&
                    contains_text(*current, cache_source) && contains_text(*current, cache_root),
                "durable state must retain opaque cache identities but not private inputs");
}

[[nodiscard]] bool frozen_cache_identity_survives_redacted_recovery() {
  constexpr std::string_view source_url = "https://private.example.invalid/recovery-source";
  constexpr std::string_view actual_url = "https://cdn.private.example.invalid/recovery.msi";
  constexpr std::string_view cache_source = "opaque-recovery-source";
  constexpr std::string_view cache_root = "controlled-recovery-root";
  constexpr std::string_view opaque_handle = "opaque-recovery-handle";
  constexpr std::string_view native_detail = "C:\\private\\recovery.msi";
  constexpr std::string_view lease_token = "installation-lease-1";

  Fixture fixture;
  auto item = fixture_item("editor");
  item.resource_kind = batch::FrozenResourceKind::cached_package;
  item.source.declared_address = std::string{source_url};
  item.source.actual_address = std::string{actual_url};
  item.cache_asset.identity.source_identity = std::string{cache_source};
  item.cache_root.id = std::string{cache_root};
  fixture.verifier.observations = {
      {.code = batch_app::InstallVerificationCode::absent},
      {.code = batch_app::InstallVerificationCode::installed},
  };
  fixture.executor.launches = {{.code = batch_app::InstallerLaunchCode::started,
                                .opaque_operation_handle = std::string{opaque_handle},
                                .detail = std::string{native_detail}}};
  if (!create(fixture, fixture_plan({std::move(item)}, "recovery-batch")) ||
      !advance(fixture, 2)) {
    return false;
  }

  auto const launch_calls_before = fixture.executor.launch_calls;
  auto const download_calls_before = fixture.download.advance_calls;
  batch_app::InstallationBatchService reopened{
      fixture.states, fixture.occupancy, fixture.log, fixture.download, fixture.executor,
      fixture.readiness, fixture.verifier, fixture.facts, fixture.admission};
  auto const restored = reopened.restore();
  auto const recovered = reopened.recover_read_only();
  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"installation-batch"});
  auto const current = fixture.files.raw_file(key, app::StateFileSlot::current);
  auto const& completion_target = fixture.executor.completion_requests.back().target;
  auto const& verification_target = fixture.verifier.requests.back().target;

  return expect(restored.succeeded() && restored.snapshot.active.has_value() &&
                    restored.snapshot.active->plan.items.front().cache_asset.identity.source_identity ==
                        cache_source &&
                    restored.snapshot.active->plan.items.front().cache_root.id == cache_root,
                "public recovery projection must retain the frozen opaque cache identity") &&
         expect(recovered.succeeded() && fixture.executor.launch_calls == launch_calls_before &&
                    fixture.download.advance_calls == download_calls_before &&
                    completion_target.cache_asset.identity.source_identity == cache_source &&
                    completion_target.cache_root.id == cache_root &&
                    verification_target.cache_asset.identity.source_identity == cache_source &&
                    verification_target.cache_root.id == cache_root,
                "read-only recovery must verify the original cached artifact without a second effect") &&
         expect(current.has_value() && contains_text(*current, cache_source) &&
                    contains_text(*current, cache_root) && !contains_text(*current, source_url) &&
                    !contains_text(*current, actual_url) && !contains_text(*current, native_detail) &&
                    !contains_text(*current, opaque_handle) && !contains_text(*current, lease_token),
                "durable recovery state must keep opaque identities without URL path detail or lease token");
}

[[nodiscard]] bool retry_foreign_occupancy_preserves_terminal_record() {
  Fixture fixture;
  fixture.verifier.observations = {{.code = batch_app::InstallVerificationCode::absent}};
  fixture.download.advances = {{.code = batch_app::InstallationDownloadCode::failed,
                                .detail = "controlled download failed"}};
  auto original = fixture_plan({fixture_item("editor")}, "occupied-original");
  if (!create(fixture, original) || !advance(fixture, 2)) {
    return false;
  }
  auto const terminal = fixture.service.advance();

  azzs::testing::SequenceLeaseTokenSource foreign_tokens{"foreign-lease-"};
  app::SharedOperationOccupancy foreign{fixture.occupancy_storage, foreign_tokens};
  auto const blocker = foreign.try_acquire({.kind = "foreign-operation",
                                            .operation_id = "foreign-occupied",
                                            .correlation_id = "foreign-correlation"});
  auto retry_plan = fixture_plan({fixture_item("editor")}, "occupied-retry");
  retry_plan.retry_of_batch_id = original.batch_id;
  auto const occupied = fixture.service.retry_current(std::move(retry_plan));
  auto const foreign_current = fixture.occupancy.inspect();
  auto const released = blocker.lease.has_value() &&
                        foreign.release(*blocker.lease).code ==
                            app::OccupancyResultCode::released;

  return expect(terminal.succeeded() && terminal.snapshot.active.has_value() &&
                    terminal.snapshot.active->state == batch::InstallationBatchState::completed &&
                    !terminal.snapshot.active->active_lease.has_value(),
                "a completed failed attempt must release its old lease before a new retry") &&
         expect(blocker.code == app::OccupancyResultCode::acquired &&
                    blocker.lease.has_value() &&
                    foreign_current.code == app::OccupancyResultCode::observed &&
                    foreign_current.current.has_value(),
                "foreign controlled occupancy must be observable before retry") &&
         expect(occupied.code == batch_app::InstallationBatchActionCode::occupied &&
                    occupied.snapshot.active.has_value() &&
                    occupied.snapshot.active->state == batch::InstallationBatchState::completed &&
                    !occupied.snapshot.active->active_lease.has_value() &&
                    occupied.snapshot.history.empty() &&
                    released,
                "foreign occupied retry must not expose an unleased nonterminal replacement record");
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
                  installer_completion_and_nonterminal_lease_are_required() &&
                  controlled_post_install_requires_completion_before_verification() &&
                  source_failure_only_blocks_dependent_items() &&
                 explicit_restart_profile_is_the_only_restart_barrier() &&
                 interaction_and_result_confirmation_are_disjoint() &&
                 retry_creates_a_new_immutable_single_item_plan() &&
                 retry_requires_admission_and_readiness_before_replacing_failed_batch() &&
                 failures_close_the_batch_before_any_effect() &&
                  restore_and_read_only_recovery_never_start_effects() &&
                  interrupted_launch_is_recovered_without_a_second_effect() &&
                  public_and_durable_batch_state_is_redacted() &&
                  frozen_cache_identity_survives_redacted_recovery() &&
                  retry_foreign_occupancy_preserves_terminal_record() &&
                  external_handoff_is_rejected_before_admission()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
