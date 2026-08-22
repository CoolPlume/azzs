#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/installation_batch_creation.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace app = azzs::application;
namespace batch_app = azzs::application::installation_batch;
namespace batch = azzs::domain::installation_batch;
namespace catalog = azzs::domain::software_catalog;
namespace selection = azzs::domain::software_selection;
namespace architecture = azzs::domain::architecture_selection;
namespace cache = azzs::domain::offline_package_cache;
namespace cache_app = azzs::application::offline_package_cache;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "installation batch creation contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingLog final : public app::ExecutionLog {
 public:
  [[nodiscard]] app::CorrelationId begin_correlation() override {
    return {.value = "batch-creation-contract-" + std::to_string(next_++)};
  }

  [[nodiscard]] app::ExecutionLogReceipt append(
      app::CorrelationId const&, app::ExecutionEvent const&) override {
    return {.persisted = true, .segment = 1, .sequence = next_++};
  }

  [[nodiscard]] app::ExecutionLogClearReceipt clear() override { return {.cleared = true}; }

  [[nodiscard]] app::DiagnosticExportReceipt export_diagnostic(
      app::DiagnosticContext const&) override {
    return {.produced = true};
  }

 private:
  std::uint64_t next_{1};
};

class ScriptedDownload final : public batch_app::InstallationDownloadPort {
 public:
  [[nodiscard]] batch_app::InstallationDownloadObservation advance(
      batch_app::InstallationEffectTarget const&) override {
    if (advances.empty()) {
      return {.code = batch_app::InstallationDownloadCode::completed};
    }
    auto value = std::move(advances.front());
    advances.erase(advances.begin());
    return value;
  }

  [[nodiscard]] batch_app::InstallationDownloadObservation pause(
      batch_app::InstallationEffectTarget const&) override {
    return {.code = batch_app::InstallationDownloadCode::paused};
  }

  [[nodiscard]] batch_app::InstallationDownloadObservation resume(
      batch_app::InstallationEffectTarget const&) override {
    return {.code = batch_app::InstallationDownloadCode::downloading};
  }

  [[nodiscard]] batch_app::InstallationDownloadObservation stop(
      batch_app::InstallationEffectTarget const&) override {
    return {.code = batch_app::InstallationDownloadCode::stopped};
  }

  std::vector<batch_app::InstallationDownloadObservation> advances;
};

class ScriptedExecutor final : public batch_app::ControlledInstallerExecutor {
 public:
  [[nodiscard]] batch_app::ControlledInstallerObservation launch(
      batch_app::ControlledInstallerLaunch const&) override {
    return {.code = launch_code,
            .opaque_operation_handle = launch_code == batch_app::InstallerLaunchCode::started
                                           ? std::optional<std::string>{"controlled-operation"}
                                           : std::nullopt};
  }

  [[nodiscard]] batch_app::ControlledInstallerCompletionObservation observe_completion(
      batch_app::ControlledInstallerCompletionRequest const&) override {
    return {.code = batch_app::InstallerCompletionCode::completed,
            .post_install = batch_app::PostInstallCompletionCode::not_required};
  }

  [[nodiscard]] batch_app::ControlledInstallerTerminationObservation force_terminate(
      batch_app::ControlledInstallerTerminationRequest const&) override {
    return {.code = batch_app::InstallerTerminationCode::unavailable};
  }

  batch_app::InstallerLaunchCode launch_code{batch_app::InstallerLaunchCode::started};
};

class RegisteredReadiness final : public batch_app::ControlledProfileReadinessPort {
 public:
  [[nodiscard]] batch_app::ControlledProfileReadiness observe(
      batch::FrozenExecutionProfile const&) override {
    return {.code = batch_app::ControlledProfileReadinessCode::registered};
  }
};

class AbsentVerifier final : public batch_app::InstallResultVerifier {
 public:
  [[nodiscard]] batch_app::InstallVerificationObservation verify(
      batch_app::InstallVerificationRequest const&) override {
    return {.code = batch_app::InstallVerificationCode::absent};
  }
};

class RecordingFacts final : public batch_app::InstallationFactSink {
 public:
  void observe(batch_app::InstallationFact const&) noexcept override {}
};

class FixedPlanningState final : public batch_app::InstallationBatchPlanningStatePort {
 public:
  [[nodiscard]] batch_app::InstallationBatchPlanningSnapshot snapshot() override {
    ++calls;
    return value;
  }

  batch_app::InstallationBatchPlanningSnapshot value;
  std::size_t calls{};
};

class FixedProfiles final : public batch_app::ControlledInstallProfileCatalog {
 public:
  [[nodiscard]] std::span<catalog::ControlledInstallProfile const> profiles() const noexcept override {
    return values;
  }

  std::vector<catalog::ControlledInstallProfile> values;
};

[[nodiscard]] selection::ResolvedPackage package_for(std::string const& software_id) {
  return {.candidate = {.software_id = software_id,
                        .architecture = architecture::PackageArchitecture::x64,
                        .version = "1.2.3",
                        .identity = software_id + "-1.2.3-x64"},
          .package_type = selection::PackageType::full_package,
          .complete_package = true};
}

[[nodiscard]] selection::ResolvedSourceSnapshot source_for(std::string const& software_id) {
  return {.software_id = software_id,
          .declared_purpose = catalog::SourcePurpose::primary,
          .declared_address = "https://declared.example/" + software_id,
          .version = "1.2.3",
          .actual_address = "https://resolved.example/" + software_id,
          .hosting_mechanism = "official",
          .branch = "stable",
          .packages = {package_for(software_id)},
          .resolved_at_milliseconds = 1,
          .capability_version = "resolver-v1"};
}

[[nodiscard]] catalog::RuntimeSoftware software_for(
    std::string software_id, std::vector<std::string> dependencies = {}) {
  auto const install_profile = "profile-" + software_id;
  auto const declared_address = "https://declared.example/" + software_id;
  return {.definition = {.id = std::move(software_id),
                         .enabled = true,
                         .enabled_declared = true,
                         .name = "Contract software",
                         .tier = catalog::SoftwareTier::basic,
                         .category_id = "basic",
                         .branch = "stable",
                         .version_policy = catalog::VersionPolicy::latest_stable,
                         .dependencies = std::move(dependencies),
                         .dependencies_declared = true,
                         .install_profile = install_profile,
                         .sources = {{.purpose = catalog::SourcePurpose::primary,
                                      .address = std::move(declared_address)}},
                         .localizations = {}},
          .availability = catalog::ItemAvailability::available};
}

[[nodiscard]] catalog::ControlledInstallProfile profile_for(std::string software_id) {
  return {.id = "profile-" + software_id,
          .software_id = std::move(software_id),
          .baselines = {{.id = "baseline-v1", .version = "1.2.3"}},
          .execution = catalog::WindowsExecutionReadiness::project_executor_registered};
}

[[nodiscard]] batch_app::InstallationBatchPlanningSnapshot planning_snapshot() {
  auto runtime = catalog::RuntimeSoftwareCatalog{
      .schema_version = 1,
      .revision = 7,
      .release_state = catalog::ReleaseState::release,
      .default_locale = "zh-CN",
      .software = {software_for("editor", {"runtime"}), software_for("runtime")},
  };
  auto editor_source = source_for("editor");
  auto runtime_source = source_for("runtime");
  auto editor_asset = cache_app::make_cache_asset(editor_source, editor_source.packages.front());
  auto runtime_asset = cache_app::make_cache_asset(runtime_source, runtime_source.packages.front());
  return {
      .catalog = {.mode = app::software_catalog::CatalogLifecycleMode::ready,
                  .machine_access = app::software_catalog::CatalogAggregateAccess::writable,
                  .current = app::software_catalog::ActiveCatalogInfo{
                      .revision = 7,
                      .item_count = 2,
                      .identity = app::software_catalog::EffectiveCatalogIdentity::released,
                      .content_identity = "catalog-identity",
                      .application_id = "azzs-contract"},
                  .current_toml_bytes = "catalog = 'contract'",
                  .current_catalog = std::move(runtime)},
      .selection = {.mode = app::software_selection::SelectionLifecycleMode::ready,
                    .has_current_catalog = true,
                    .subject_writable = true,
                    .machine_writable = true,
                    .selection = {.initialized = true,
                                  .selected_software_ids = {"editor", "runtime"}},
                    .items = {{.software_id = "editor", .selected = true, .available = true},
                              {.software_id = "runtime", .selected = true, .available = true}},
                    .sources = {std::move(editor_source), std::move(runtime_source)},
                    .active_catalog = app::software_catalog::ActiveCatalogInfo{
                        .revision = 7,
                        .item_count = 2,
                        .identity = app::software_catalog::EffectiveCatalogIdentity::released,
                        .content_identity = "catalog-identity",
                        .application_id = "azzs-contract"}},
      .cache = {.selected_root = {.kind = cache::CacheLocationKind::system_directory,
                                  .id = "contract-cache"},
                .location_state = cache_app::CacheLocationState::available,
                .items = {{.asset = *editor_asset,
                           .availability = cache_app::OfflinePackageAvailability::ready_to_download},
                          {.asset = *runtime_asset,
                           .availability = cache_app::OfflinePackageAvailability::cached_available,
                           .cache_present = true}}},
  };
}

[[nodiscard]] batch_app::InstallationBatchCreateRequest create_request(
    std::string batch_id = "creation-batch") {
  return {.batch_id = std::move(batch_id),
          .correlation_id = "creation-correlation",
          .packages = {{.software_id = "editor",
                        .declared_address = "https://declared.example/editor",
                        .package_identity = "editor-1.2.3-x64"},
                       {.software_id = "runtime",
                        .declared_address = "https://declared.example/runtime",
                        .package_identity = "runtime-1.2.3-x64"}},
          .frozen_at_milliseconds = 1};
}

struct Fixture final {
  Fixture()
      : clock{app::WallClockTime{std::chrono::milliseconds{1'786'422'400'000}}},
        states{files, clock}, tokens{"batch-creation-lease-"},
        occupancy{occupancy_storage, tokens},
        service{states, occupancy, log, download, executor, readiness, verifier, facts, assets},
        creation{service, planning, readiness, profiles, assets} {
    planning.value = planning_snapshot();
    profiles.values = {profile_for("editor"), profile_for("runtime")};
  }

  [[nodiscard]] bool restore() { return service.restore().succeeded(); }

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
  AbsentVerifier verifier;
  RecordingFacts facts;
  batch_app::FrozenBatchAssetRegistry assets;
  batch_app::InstallationBatchService service;
  FixedPlanningState planning;
  FixedProfiles profiles;
  batch_app::InstallationBatchCreationService creation;
};

[[nodiscard]] bool registry_admits_only_staged_exact_plans() {
  Fixture fixture;
  if (!fixture.restore()) {
    return false;
  }
  auto const ready = fixture.creation.assess(create_request());
  auto const created = fixture.creation.create(create_request());
  auto original = fixture.assets.find("creation-batch");
  if (!original.has_value()) {
    return false;
  }
  auto tampered = *original;
  tampered.correlation_id = "tampered-correlation";
  return expect(ready.ready() && created.batch.succeeded() &&
                    fixture.assets.admit(*original).code ==
                        batch_app::FrozenBatchPlanAdmissionCode::accepted &&
                    fixture.assets.admit(tampered).code ==
                        batch_app::FrozenBatchPlanAdmissionCode::rejected,
                "only the exact staged initial plan may be admitted");
}

[[nodiscard]] bool create_freezes_complete_dependency_closure() {
  Fixture fixture;
  if (!fixture.restore()) {
    return false;
  }
  auto const result = fixture.creation.create(create_request());
  auto const active = fixture.service.snapshot().active;
  return expect(result.assessment.ready() && result.batch.succeeded() && active.has_value() &&
                    active->plan.items.size() == 2 &&
                    active->plan.items[0].item_id == "runtime" &&
                    active->plan.items[1].item_id == "editor" &&
                    active->plan.items[1].dependencies == std::vector<std::string>{"runtime"} &&
                    active->plan.items[0].resource_kind == batch::FrozenResourceKind::cached_package &&
                    active->plan.items[1].resource_kind ==
                        batch::FrozenResourceKind::controlled_download,
                "creation must freeze every selected dependency before its dependent item");
}

[[nodiscard]] bool incomplete_inputs_do_not_create_a_batch() {
  Fixture fixture;
  if (!fixture.restore()) {
    return false;
  }
  fixture.planning.value.cache.items.clear();
  auto const result = fixture.creation.create(create_request());
  return expect(result.assessment.code == batch_app::InstallationBatchCreationCode::cache_unavailable &&
                    !fixture.service.snapshot().active.has_value(),
                "a missing controlled asset must reject creation before batch state changes");
}

[[nodiscard]] bool mixed_request_keeps_independent_item_and_reports_unavailable_profile() {
  Fixture fixture;
  if (!fixture.restore()) {
    return false;
  }

  auto& planning = fixture.planning.value;
  planning.catalog.current_catalog->software.front().definition.dependencies.clear();
  auto cheat_engine = software_for("cheat-engine");
  cheat_engine.availability = catalog::ItemAvailability::install_profile_unavailable;
  planning.catalog.current_catalog->software.push_back(std::move(cheat_engine));
  planning.selection.selection.selected_software_ids = {"editor", "cheat-engine"};
  planning.selection.items = {{.software_id = "editor", .selected = true, .available = true},
                              {.software_id = "cheat-engine",
                               .selected = true,
                               .available = true}};
  auto cheat_source = source_for("cheat-engine");
  auto cheat_asset = cache_app::make_cache_asset(cheat_source, cheat_source.packages.front());
  planning.selection.sources.push_back(std::move(cheat_source));
  planning.cache.items.push_back(
      {.asset = *cheat_asset,
       .availability = cache_app::OfflinePackageAvailability::cached_available,
       .cache_present = true});

  auto request = create_request();
  request.packages = {{.software_id = "editor",
                       .declared_address = "https://declared.example/editor",
                       .package_identity = "editor-1.2.3-x64"},
                      {.software_id = "cheat-engine",
                       .declared_address = "https://declared.example/cheat-engine",
                       .package_identity = "cheat-engine-1.2.3-x64"}};
  auto const assessed = fixture.creation.assess(request);
  auto const created = fixture.creation.create(request);
  auto const editor = std::ranges::find(assessed.items, "editor",
                                        [](batch_app::InstallationBatchItemAssessment const& item) {
                                          return item.item_id;
                                        });
  auto const cheat = std::ranges::find(assessed.items, "cheat-engine",
                                       [](batch_app::InstallationBatchItemAssessment const& item) {
                                         return item.item_id;
                                       });
  auto const active = fixture.service.snapshot().active;
  return expect(assessed.ready() && created.batch.succeeded() && editor != assessed.items.end() &&
                    editor->ready() && cheat != assessed.items.end() &&
                    cheat->code == batch_app::InstallationBatchCreationCode::profile_unavailable &&
                    active.has_value() && active->plan.items.size() == 1 &&
                    active->plan.items.front().item_id == "editor",
                "a mixed request must retain an executable item and explain a missing profile");
}

[[nodiscard]] bool mixed_request_keeps_ready_item_when_source_is_unresolved() {
  Fixture fixture;
  if (!fixture.restore()) {
    return false;
  }

  auto& planning = fixture.planning.value;
  planning.catalog.current_catalog->software.front().definition.dependencies.clear();
  auto cheat_engine = software_for("cheat-engine");
  planning.catalog.current_catalog->software.push_back(std::move(cheat_engine));
  fixture.profiles.values.push_back(profile_for("cheat-engine"));
  planning.selection.selection.selected_software_ids = {"editor", "cheat-engine"};
  planning.selection.items = {{.software_id = "editor", .selected = true, .available = true},
                              {.software_id = "cheat-engine",
                               .selected = true,
                               .available = true}};

  auto request = create_request();
  request.packages = {{.software_id = "editor",
                       .declared_purpose = catalog::SourcePurpose::primary,
                       .declared_address = "https://declared.example/editor",
                       .package_identity = "editor-1.2.3-x64"},
                      {.software_id = "cheat-engine",
                       .declared_purpose = catalog::SourcePurpose::primary}};
  auto const assessed = fixture.creation.assess(request);
  auto const created = fixture.creation.create(request);
  auto const editor = std::ranges::find(assessed.items, "editor",
                                        [](batch_app::InstallationBatchItemAssessment const& item) {
                                          return item.item_id;
                                        });
  auto const cheat = std::ranges::find(assessed.items, "cheat-engine",
                                       [](batch_app::InstallationBatchItemAssessment const& item) {
                                         return item.item_id;
                                       });
  auto const active = fixture.service.snapshot().active;
  return expect(assessed.ready() && created.batch.succeeded() && editor != assessed.items.end() &&
                    editor->ready() && cheat != assessed.items.end() &&
                    cheat->code == batch_app::InstallationBatchCreationCode::source_unresolved &&
                    active.has_value() && active->plan.items.size() == 1 &&
                    active->plan.items.front().item_id == "editor",
                "a source-unresolved item must not discard an independently executable item");
}

[[nodiscard]] bool retry_without_active_batch_is_rejected_without_staged_assets() {
  Fixture fixture;
  if (!fixture.restore()) {
    return false;
  }
  auto const retry = fixture.creation.retry_current(
      {.batch_id = "retry-without-active", .correlation_id = "retry-correlation", .frozen_at_milliseconds = 2});
  return expect(retry.assessment.code == batch_app::InstallationBatchCreationCode::no_retryable_item &&
                    !fixture.assets.find("retry-without-active").has_value(),
                "retry without an active batch must not create or stage assets");
}

[[nodiscard]] bool failed_create_retains_assets_when_the_batch_is_durable() {
  Fixture fixture;
  fixture.files.fail_next(azzs::testing::StateFileOperation::write,
                          app::StateFileSlot::candidate);
  if (!fixture.restore()) {
    return false;
  }
  auto const created = fixture.creation.create(create_request());
  auto const active = fixture.service.snapshot().active;
  auto const frozen = fixture.assets.find("creation-batch");
  return expect(!created.batch.succeeded() && active.has_value() &&
                    active->plan.batch_id == "creation-batch" && frozen.has_value() &&
                    frozen->batch_id == "creation-batch",
                "a failed persistence result must retain assets for a durable active batch");
}

[[nodiscard]] bool restore_rebuilds_redacted_frozen_assets() {
  Fixture fixture;
  if (!fixture.restore() || !fixture.creation.create(create_request()).batch.succeeded()) {
    return false;
  }
  batch_app::FrozenBatchAssetRegistry restored_assets;
  batch_app::InstallationBatchService restored_service{
      fixture.states, fixture.occupancy, fixture.log, fixture.download, fixture.executor,
      fixture.readiness, fixture.verifier, fixture.facts, restored_assets};
  auto const restore_result = restored_service.restore();
  auto const full_plan = restored_assets.find("creation-batch");
  return expect(restore_result.succeeded() && full_plan.has_value() &&
                    full_plan->items.front().source.declared_address ==
                        "redacted-source" &&
                    full_plan->items.front().source.actual_address ==
                        "redacted-source" &&
                    full_plan->items.front().cache_root.id == "contract-cache",
                "restore must rebuild a safe frozen snapshot from durable state without a live selection query");
}

[[nodiscard]] bool retry_uses_registered_frozen_assets_without_replanning() {
  Fixture fixture;
  if (!fixture.restore() || !fixture.creation.create(create_request()).batch.succeeded()) {
    return false;
  }
  fixture.executor.launch_code = batch_app::InstallerLaunchCode::failed;
  auto const first = fixture.service.advance();
  auto const failed = fixture.service.advance();
  auto const planning_calls = fixture.planning.calls;
  auto const retry = fixture.creation.retry_current(
      {.batch_id = "retry-batch", .correlation_id = "retry-correlation", .frozen_at_milliseconds = 2});
  auto const active = fixture.service.snapshot().active;
  auto const retry_assets = fixture.assets.find("retry-batch");
  auto const admitted = retry_assets.has_value()
                            ? fixture.assets.admit_retry(*retry_assets).code
                            : batch_app::FrozenBatchPlanAdmissionCode::rejected;
  return expect(first.succeeded() && failed.succeeded() && retry.assessment.ready() &&
                    retry.batch.succeeded() && fixture.planning.calls == planning_calls &&
                    retry_assets.has_value() &&
                    retry_assets->retry_of_batch_id == "creation-batch" &&
                    retry_assets->items.size() == 1 &&
                    retry_assets->items.front().item_id == "runtime" &&
                    active.has_value() && active->plan.retry_of_batch_id == "creation-batch" &&
                    active->plan.items.size() == 1 &&
                    active->plan.items.front().item_id == "runtime" &&
                    admitted == batch_app::FrozenBatchPlanAdmissionCode::accepted,
                "retry must copy the failed frozen item without reading live planning state");
}

}  // namespace

int main() {
  return registry_admits_only_staged_exact_plans() &&
                 create_freezes_complete_dependency_closure() &&
                 incomplete_inputs_do_not_create_a_batch() &&
                 mixed_request_keeps_independent_item_and_reports_unavailable_profile() &&
                 mixed_request_keeps_ready_item_when_source_is_unresolved() &&
                 retry_without_active_batch_is_rejected_without_staged_assets() &&
                 failed_create_retains_assets_when_the_batch_is_durable() &&
                 restore_rebuilds_redacted_frozen_assets() &&
                 retry_uses_registered_frozen_assets_without_replanning()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
