#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/application_update.hpp"
#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/debug_log_policy/debug_log_policy.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/history_and_logs.hpp"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_batch.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace app = azzs::application;
namespace architecture = azzs::domain::architecture_selection;
namespace catalog = azzs::domain::software_catalog;
namespace optimization = azzs::domain::software_optimization_catalog;
namespace optimization_batch = azzs::domain::software_optimization_batch;
namespace selection = azzs::domain::software_selection;
namespace catalog_app = azzs::application::software_catalog;
namespace optimization_app = azzs::application::software_optimization_batch;
namespace selection_app = azzs::application::software_selection;

using azzs::testing::FixedClock;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::SequenceLeaseTokenSource;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "history and logs contract failed: " << message << '\n';
  }
  return condition;
}

class ProjectionLog final : public app::ExecutionLog {
 public:
  [[nodiscard]] app::CorrelationId begin_correlation() override {
    return {.value = "history-contract-" + std::to_string(next_sequence_)};
  }

  [[nodiscard]] app::ExecutionLogReceipt append(
      app::CorrelationId const& correlation,
      app::ExecutionEvent const& event) override {
    auto const sequence = next_sequence_++;
    app::ExecutionLogEventProjection projection{
        .segment = 1,
        .sequence = sequence,
        .correlation = correlation,
        .recorded_at_milliseconds = static_cast<std::int64_t>(sequence),
        .kind = event.kind,
        .component = event.component,
        .stage = event.stage,
        .result = event.result,
        .error = event.error,
        .last_trusted_state = event.last_trusted_state,
        .coverage_gap = event.coverage_gap,
    };
    for (auto const& field : event.fields) {
      projection.fields.push_back({.key = field.key, .value = field.value});
    }
    events.push_back(std::move(projection));
    return {.persisted = true, .segment = 1, .sequence = sequence};
  }

  [[nodiscard]] app::ExecutionLogSnapshot snapshot() override {
    return {
        .available = true,
        .active_segment = 1,
        .last_sequence = next_sequence_ - 1,
        .coverage_started_at = app::WallClockTime{std::chrono::milliseconds{1}},
        .events = events,
    };
  }

  [[nodiscard]] app::ExecutionLogClearReceipt clear() override {
    events.clear();
    return {.cleared = true, .cutoff_segment = 1, .active_segment = 2};
  }

  [[nodiscard]] app::DiagnosticExportReceipt export_diagnostic(
      app::DiagnosticContext const& context) override {
    exported_context = context;
    return {.produced = true,
            .complete = false,
            .file_count = 1,
            .file_name = "history-contract-diagnostic.txt"};
  }

  void record(std::string correlation, std::string stage,
              std::string software_id = {}) {
    app::ExecutionEvent event{
        .kind = app::ExecutionEventKind::state_transition,
        .component = "history-contract",
        .stage = std::move(stage),
        .result = app::ExecutionResult::succeeded,
    };
    if (!software_id.empty()) {
      event.fields.push_back({.key = "software_id",
                              .value = std::move(software_id),
                              .disposition = app::DiagnosticValueDisposition::retain});
    }
    static_cast<void>(append({.value = std::move(correlation)}, event));
  }

  std::vector<app::ExecutionLogEventProjection> events;
  std::optional<app::DiagnosticContext> exported_context;

 private:
  std::uint64_t next_sequence_{1};
};

class FixedPlatform final : public app::PlatformInfo {
 public:
  [[nodiscard]] std::optional<azzs::domain::SystemVersion> windows_version()
      const override {
    return azzs::domain::SystemVersion{10, 0, 22631};
  }

  [[nodiscard]] azzs::domain::SystemArchitecture windows_architecture()
      const override {
    return azzs::domain::SystemArchitecture::x64;
  }
};

class IdleActivity final : public app::InitializationOperationActivity {
 public:
  [[nodiscard]] app::InitializationOperationActivitySnapshot observe() override {
    return {};
  }
};

class EmptyUpdatePlatform final : public app::ApplicationUpdatePlatform {
 public:
  [[nodiscard]] app::ApplicationBuildIdentity current_build()
      const noexcept override {
    return {};
  }

  [[nodiscard]] app::GithubReleaseQueryResult query_releases() override {
    return {.code = app::GithubReleaseQueryResultCode::unavailable};
  }

  [[nodiscard]] app::ApplicationUpdateEffect download_and_replace(
      app::ApplicationUpdateCandidate const&) override {
    return {};
  }

  [[nodiscard]] app::ApplicationUpdateEffect retry_start(
      app::ApplicationUpdateHealthRecord const&) override {
    return {};
  }

  [[nodiscard]] app::ApplicationUpdateEffect rollback(
      app::ApplicationUpdateHealthRecord const&) override {
    return {};
  }

  [[nodiscard]] app::ApplicationUpdateHealthRead read_health_record() override {
    return {.code = app::UpdatePlatformResultCode::unavailable};
  }

  [[nodiscard]] app::UpdatePlatformResult write_health_record(
      app::ApplicationUpdateHealthRecord const&) override {
    return {.code = app::UpdatePlatformResultCode::unavailable};
  }

  [[nodiscard]] app::UpdatePlatformResult clear_health_record() override {
    return {.code = app::UpdatePlatformResultCode::unavailable};
  }

  [[nodiscard]] app::ApplicationUpdateStartHealth confirm_started_healthy(
      app::ApplicationUpdateHealthRecord const&) override {
    return {.code = app::UpdatePlatformResultCode::unavailable};
  }

  [[nodiscard]] app::UpdatePlatformResult open_manual_download(
      app::ManualApplicationDownloadRequest const&) override {
    return {.code = app::UpdatePlatformResultCode::unavailable};
  }
};

class EmptyHardwareObserver final : public app::HardwareObserver {
 public:
  [[nodiscard]] app::HardwareObservationResult observe(
      std::stop_token) override {
    return {.code = app::HardwareObservationCode::failed,
            .error = "history contract does not observe hardware"};
  }
};

class EmptyCatalogFiles final : public catalog_app::SoftwareCatalogFileReader {
 public:
  [[nodiscard]] catalog_app::CatalogFileRead read_built_in() const override {
    return {};
  }

  [[nodiscard]] catalog_app::CatalogFileRead read_update() const override {
    return {};
  }

  [[nodiscard]] catalog_app::CatalogFileRead read_manual_import(
      std::string const&) const override {
    return {};
  }
};

class EmptyCatalogCodec final : public catalog_app::SoftwareCatalogCodec {
 public:
  [[nodiscard]] catalog_app::CatalogDecodeResult decode(
      std::string_view) const override {
    return {};
  }

  [[nodiscard]] std::string encode(
      catalog::SoftwareCatalogDocument const&) const override {
    return {};
  }
};

class ClosedCatalogAccess final : public catalog_app::CatalogMaintenanceAccess {
 public:
  [[nodiscard]] catalog_app::CatalogEditorAccess editor_access()
      const noexcept override {
    return catalog_app::CatalogEditorAccess::unavailable;
  }
};

class EmptySettingsCatalog final
    : public app::SystemSettingsCatalogSnapshotSource {
 public:
  [[nodiscard]] std::optional<app::settings_domain::ValidatedSettingsCatalog>
  current_settings_catalog() override {
    return std::nullopt;
  }
};

class EmptySettingsPlatform final : public app::SystemSettingsPlatformAdapter {
 public:
  [[nodiscard]] std::optional<app::settings_domain::WindowsVersion>
  windows_version() const override {
    return std::nullopt;
  }

  [[nodiscard]] std::optional<app::SystemSettingsWindowsVersionFact>
  windows_version_fact() const override {
    return std::nullopt;
  }

  [[nodiscard]] app::SystemSettingsRead read(app::ControlledSystemSetting)
      override {
    return {};
  }

  [[nodiscard]] app::SystemSettingsAdapterResult apply(
      app::ControlledSystemSetting) override {
    return {};
  }

  [[nodiscard]] app::SystemSettingsAdapterResult restore(
      app::ControlledSystemSetting, app::WindowsSystemSettingValue) override {
    return {};
  }

  [[nodiscard]] app::SystemSettingsAdapterResult restart_explorer() override {
    return {};
  }
};

[[nodiscard]] app::SystemSettingsOperationHistory
immutable_system_settings_history() {
  auto const windows11_25h2 = app::settings_domain::WindowsVersion{
      .generation = app::settings_domain::WindowsGeneration::windows_11,
      .feature_update_year = 25,
      .feature_update_half = 2,
  };
  auto const declared_range = app::settings_domain::WindowsVersionRange{
      .minimum = windows11_25h2,
      .maximum = windows11_25h2,
  };
  app::SystemSettingsOperationFact applied{
      .fact_id = 41,
      .operation = app::SystemSettingsOperationKind::apply,
      .catalog_availability = app::SystemSettingsFactAvailability::obtained,
      .catalog_identity = "system-settings-catalog",
      .catalog_revision = 42,
      .selected_plan_id = app::settings_domain::StableId{"plan.recommended"},
      .selected_plan_name = "Recommended overall",
      .windows_environment = {
          .availability = app::SystemSettingsFactAvailability::obtained,
          .display_version = "Windows 11 25H2",
          .internal_build = 26'200,
      },
      .explorer_restart_requested = true,
      .explorer_restart_result =
          app::SystemSettingsExplorerRestartResult::deferred,
      .status = app::SystemSettingsOperationStatus::waiting_explorer_restart,
      .reason = "waiting for Explorer restart",
      .settings = {{
          .setting_id = app::settings_domain::StableId{
              "setting.classic-context-menu"},
          .display_name = "Classic context menu",
          .controlled_identity = "windows11.classic-context-menu",
          .catalog_revision = 42,
          .declared_range_availability =
              app::SystemSettingsFactAvailability::obtained,
          .declared_windows_range = declared_range,
          .original_value = app::WindowsSystemSettingValue{
              app::ClassicContextMenuMode::windows11},
          .target_value = app::WindowsSystemSettingValue{
              app::ClassicContextMenuMode::classic},
          .recovery_record_id = 7,
          .restart_requirement = app::settings_domain::RestartRequirement::explorer,
          .force_attempt_confirmed = true,
          .status =
              app::SystemSettingsOperationStatus::waiting_explorer_restart,
          .reason = "waiting for Explorer restart",
      }},
      .timeline = {{
                       .ordinal = 1,
                       .stage = "selection-frozen",
                       .status = app::SystemSettingsOperationStatus::completed,
                       .reason = "frozen recommended plan",
                   },
                   {
                       .ordinal = 2,
                       .stage = "operation-finished",
                       .status = app::SystemSettingsOperationStatus::
                           waiting_explorer_restart,
                       .reason = "waiting for Explorer restart",
                   }},
  };
  app::SystemSettingsOperationFact restored{
      .fact_id = 42,
      .operation = app::SystemSettingsOperationKind::restore,
      .catalog_availability = app::SystemSettingsFactAvailability::not_obtained,
      .catalog_identity = "NOT_OBTAINED",
      .catalog_revision = 42,
      .catalog_reason =
          "NOT_OBTAINED: restore uses frozen recovery record, not current catalog",
      .windows_environment = {
          .availability = app::SystemSettingsFactAvailability::not_obtained,
          .display_version = "NOT_OBTAINED",
          .reason = "NOT_OBTAINED: Windows environment was not captured",
      },
      .explorer_restart_requested = true,
      .explorer_restart_result =
          app::SystemSettingsExplorerRestartResult::succeeded,
      .status = app::SystemSettingsOperationStatus::restored,
      .reason = "restored frozen original value",
      .settings = {{
          .setting_id = app::settings_domain::StableId{
              "setting.classic-context-menu"},
          .display_name = "Classic context menu",
          .controlled_identity = "NOT_OBTAINED",
          .catalog_revision = 42,
          .declared_range_availability =
              app::SystemSettingsFactAvailability::not_obtained,
          .declared_range_reason =
              "NOT_OBTAINED: recovery record did not retain declared Windows range",
          .original_value = app::WindowsSystemSettingValue{
              app::ClassicContextMenuMode::windows11},
          .target_value = app::WindowsSystemSettingValue{
              app::ClassicContextMenuMode::windows11},
          .recovery_record_id = 7,
          .restart_requirement = app::settings_domain::RestartRequirement::explorer,
          .status = app::SystemSettingsOperationStatus::restored,
          .reason = "restored frozen original value",
      }},
      .timeline = {{
                       .ordinal = 1,
                       .stage = "restore-frozen-recovery",
                       .status = app::SystemSettingsOperationStatus::completed,
                       .reason = "recovery record frozen",
                   },
                   {
                       .ordinal = 2,
                       .stage = "restore-finished",
                       .status = app::SystemSettingsOperationStatus::restored,
                       .reason = "restored frozen original value",
                   }},
  };
  return {.facts = {std::move(applied), std::move(restored)}};
}

class EmptyRecoveryStore final : public app::SystemSettingsRecoveryStore {
 public:
  [[nodiscard]] app::RecoveryStorageRead read() override {
    return {.status = app::RecoveryStorageStatus::loaded,
            .operation_history = operation_history};
  }

  [[nodiscard]] app::RecoveryStorageWrite save(
      app::SystemSettingsRecoveryRecord) override {
    return {.status = app::RecoveryStorageStatus::committed};
  }

  [[nodiscard]] app::RecoveryStorageWrite append_operation_fact(
      app::SystemSettingsOperationFact fact) override {
    operation_history.facts.push_back(std::move(fact));
    return {.status = app::RecoveryStorageStatus::committed};
  }

  [[nodiscard]] app::RecoveryStorageWrite erase(std::uint64_t) override {
    return {.status = app::RecoveryStorageStatus::committed};
  }

  app::SystemSettingsOperationHistory operation_history{
      immutable_system_settings_history()};
};

class NoopDownload final : public app::installation_batch::InstallationDownloadPort {
 public:
  [[nodiscard]] app::installation_batch::InstallationDownloadObservation advance(
      app::installation_batch::InstallationEffectTarget const&) override {
    return {};
  }

  [[nodiscard]] app::installation_batch::InstallationDownloadObservation pause(
      app::installation_batch::InstallationEffectTarget const&) override {
    return {};
  }

  [[nodiscard]] app::installation_batch::InstallationDownloadObservation resume(
      app::installation_batch::InstallationEffectTarget const&) override {
    return {};
  }

  [[nodiscard]] app::installation_batch::InstallationDownloadObservation stop(
      app::installation_batch::InstallationEffectTarget const&) override {
    return {};
  }
};

class NoopInstaller final : public app::installation_batch::ControlledInstallerExecutor {
 public:
  [[nodiscard]] app::installation_batch::ControlledInstallerObservation launch(
      app::installation_batch::ControlledInstallerLaunch const&) override {
    return {};
  }

  [[nodiscard]] app::installation_batch::ControlledInstallerCompletionObservation
  observe_completion(
      app::installation_batch::ControlledInstallerCompletionRequest const&)
      override {
    return {};
  }

  [[nodiscard]] app::installation_batch::ControlledInstallerTerminationObservation
  force_terminate(
      app::installation_batch::ControlledInstallerTerminationRequest const&)
      override {
    return {};
  }
};

class NoopReadiness final
    : public app::installation_batch::ControlledProfileReadinessPort {
 public:
  [[nodiscard]] app::installation_batch::ControlledProfileReadiness observe(
      azzs::domain::installation_batch::FrozenExecutionProfile const&) override {
    return {};
  }
};

class NoopVerifier final : public app::installation_batch::InstallResultVerifier {
 public:
  [[nodiscard]] app::installation_batch::InstallVerificationObservation verify(
      app::installation_batch::InstallVerificationRequest const&) override {
    return {};
  }
};

class NoopInstallationFacts final : public app::installation_batch::InstallationFactSink {
 public:
  void observe(app::installation_batch::InstallationFact const&) noexcept override {}
};

class AcceptingInstallationPlan final
    : public app::installation_batch::FrozenBatchPlanAdmissionPort {
 public:
  [[nodiscard]] app::installation_batch::FrozenBatchPlanAdmission admit(
      azzs::domain::installation_batch::FrozenBatchPlan const&) const override {
    return {.code = app::installation_batch::FrozenBatchPlanAdmissionCode::accepted};
  }

  [[nodiscard]] app::installation_batch::FrozenBatchPlanAdmission admit_retry(
      azzs::domain::installation_batch::FrozenBatchPlan const&) const override {
    return {.code = app::installation_batch::FrozenBatchPlanAdmissionCode::accepted};
  }
};

[[nodiscard]] optimization_batch::FrozenOptimizationBatchPlan
make_optimization_plan() {
  optimization::TargetSoftware target{
      .id = {"history-target"},
      .identity_anchor = {"vendor.history.target"},
      .support_mode = optimization::SupportMode::supported,
      .supported_versions = {"1.0", "2.0"},
      .install_detection = {optimization::RuleKind::built_in_definition,
                            {"history-detect-install"}},
      .version_detection = {optimization::RuleKind::built_in_definition,
                            {"history-detect-version"}},
      .explanation_source = "history contract target",
  };
  optimization::SoftwareOptimizationScheme scheme{
      .id = {"history-scheme"},
      .target_id = target.id,
      .automation = optimization::AutomationSupport::controlled,
      .supported_versions = {"1.0", "2.0"},
      .impact = "history contract scheme",
      .risk = optimization::RiskLevel::low,
      .exit_requirement = optimization::ExitRequirement::none,
      .restart_requirement = optimization::RestartRequirement::none,
      .explanation_source = "history contract scheme",
      .availability = optimization::SchemeAvailability::available,
  };
  optimization::SoftwareOptimizationOption option{
      .id = {"history-option"},
      .scheme_id = scheme.id,
      .supported_versions = {"1.0", "2.0"},
      .impact = "history contract option",
      .automation = optimization::AutomationSupport::controlled,
      .execution = {optimization::RuleKind::built_in_definition,
                    {"history-execute-option"}},
      .state_detection = {optimization::RuleKind::built_in_definition,
                          {"history-detect-option"}},
      .explanation_source = "history contract option",
  };
  optimization_batch::FrozenOptimizationBatchPlan plan;
  plan.batch_id = "optimization-history-batch";
  plan.correlation_id = "frozen-optimization-correlation";
  plan.catalog_revision = 7;
  plan.emergency_notice_revision = 4;
  plan.schemes.push_back({
      .target = std::move(target),
      .scheme = std::move(scheme),
      .detected_version = "1.0",
      .risk_confirmation_id = "history-risk-confirmation",
      .selected_options = {{.option = std::move(option)}},
  });
  plan.frozen_at_milliseconds = 100;
  return plan;
}

class FixedOptimizationPlans final
    : public optimization_app::SoftwareOptimizationBatchPlanSource {
 public:
  FixedOptimizationPlans() : plan(make_optimization_plan()) {}

  [[nodiscard]] optimization_app::FrozenPlanAdmission freeze(
      optimization_app::OptimizationBatchStartRequest const&) override {
    return {.accepted = true, .plan = plan};
  }

  optimization_batch::FrozenOptimizationBatchPlan plan;
};

class ImmediateOptimizationExecutor final
    : public optimization_app::SoftwareOptimizationStepExecutor {
 public:
  [[nodiscard]] optimization_app::StepExecutionObservation execute(
      optimization_batch::FrozenOptimizationOption const&) override {
    return {.code = optimization_app::StepExecutionCode::applied};
  }

  [[nodiscard]] optimization_app::StepVerificationObservation verify(
      optimization_batch::FrozenOptimizationOption const&) override {
    return {.code = optimization_app::StepVerificationCode::optimized};
  }

  [[nodiscard]] optimization_app::TargetExitObservation observe_target_exit(
      optimization_batch::FrozenOptimizationScheme const&) override {
    return {.known = true, .exited = true};
  }
};

class AllowedWithdrawals final
    : public optimization_app::SoftwareOptimizationWithdrawalAuthorization {
 public:
  [[nodiscard]] optimization_app::WithdrawalAuthorization authorize(
      optimization_batch::FrozenOptimizationScheme const&,
      optimization_batch::FrozenOptimizationOption const&) override {
    return {.code = optimization_app::WithdrawalAuthorizationCode::allowed,
            .notice_revision = 4};
  }
};

class FixtureResolver final : public selection_app::ControlledSourceResolver {
 public:
  [[nodiscard]] selection_app::SourceResolutionResult resolve(
      std::string_view software_id,
      catalog::CatalogSource const& declared_source) override {
    return {
        .resolved = true,
        .snapshot = selection::ResolvedSourceSnapshot{
            .software_id = std::string{software_id},
            .declared_purpose = *declared_source.purpose,
            .declared_address = declared_source.address,
            .version = "2.3.4",
            .actual_address = "https://downloads.example.test/editor-2.3.4.msi",
            .hosting_mechanism = "controlled-release-asset",
            .branch = "stable",
            .packages = {{
                .candidate = {.software_id = std::string{software_id},
                              .architecture = architecture::PackageArchitecture::x64,
                              .version = "2.3.4",
                              .identity = "editor-2.3.4-x64"},
                .package_type = selection::PackageType::full_package,
                .complete_package = true,
            }},
            .resolved_at_milliseconds = 1'000,
            .capability_version = "resolver-v1",
        }};
  }
};

class AvailableNetwork final : public selection_app::NetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override { return true; }
};

class AbsentDetector final : public selection_app::SoftwarePresenceDetector {
 public:
  [[nodiscard]] selection_app::PresenceDetection detect(std::string_view) override {
    return {.completed = true,
            .present = false,
            .detail = "test fixture has no external installation"};
  }
};

class AcceptingLauncher final : public selection_app::ExternalAddressLauncher {
 public:
  [[nodiscard]] bool open_declared_address(
      std::string_view, catalog::CatalogSource const&, std::string& error)
      override {
    error.clear();
    return true;
  }
};

class MutableDebugPolicySource final : public app::DebugLogPolicySnapshotSource {
 public:
  [[nodiscard]] app::DebugLogPolicySnapshot snapshot() const override {
    ++reads;
    return value;
  }

  mutable std::size_t reads{};
  app::DebugLogPolicySnapshot value;
};

[[nodiscard]] app::DebugLogPolicySnapshot debug_policy(
    app::DebugModeState mode) {
  return {
      .debug_mode = mode,
      .granularity = mode == app::DebugModeState::enabled
                         ? app::DebugLogGranularity::maximum
                         : app::DebugLogGranularity::normal,
      .filterable_fields = {app::DebugLogFilterField::command,
                            app::DebugLogFilterField::correlation_id},
      .locating_semantics =
          app::DebugLogLocationSemantics::event_sequence_and_correlation_id,
      .coverage = {app::DebugLogCoverage::source_resolution,
                   app::DebugLogCoverage::installation},
      .existing_log_retention = app::DebugLogRetention::preserves_existing_records,
      .facts_available = true,
  };
}

[[nodiscard]] catalog::RuntimeSoftwareCatalog selection_catalog() {
  return {
      .schema_version = 1,
      .revision = 3,
      .release_state = catalog::ReleaseState::release,
      .default_locale = "zh-CN",
      .software = {{
          .definition = {
              .id = "editor",
              .enabled = true,
              .enabled_declared = true,
              .name = "Editor",
              .tier = catalog::SoftwareTier::normal,
              .category_id = "tools",
              .branch = "stable",
              .version_policy = catalog::VersionPolicy::latest_stable,
              .dependencies_declared = true,
              .bundled_editions_declared = true,
              .sources = {{.purpose = catalog::SourcePurpose::primary,
                           .address = "https://example.test/editor"}},
          },
          .availability = catalog::ItemAvailability::available,
      }},
  };
}

[[nodiscard]] selection_app::CatalogSelectionProjection
selection_catalog_projection() {
  auto runtime = selection_catalog();
  return {
      .runtime = runtime,
      .active = {
          .revision = runtime.revision,
          .item_count = runtime.software.size() + runtime.drivers.size(),
          .origin = catalog_app::CatalogCandidateOrigin::built_in,
          .identity = catalog_app::EffectiveCatalogIdentity::released,
          .content_identity = "history-selection-catalog",
          .application_id = "history-selection-app",
      },
  };
}

struct HistoryFixture final {
  HistoryFixture()
      : clock{app::WallClockTime{std::chrono::milliseconds{2'000}}},
        states{files, clock},
        tokens{"history-contract-lease-"},
        occupancy{occupancy_storage, tokens},
        updates{update_platform, activity, log, clock},
        hardware{hardware_observer, clock},
        catalog_lifecycle{states, log, occupancy, catalog_files, catalog_codec,
                          catalog_policy, catalog_access,
                          azzs::domain::StateSubject{"history-catalog"}},
        installation_batches{states, occupancy, log, download, installer,
                             readiness, verifier, installation_facts,
                             installation_admission},
        optimization_batches{states, occupancy, log, optimization_plans,
                             optimization_executor, withdrawals},
        architectures{platform, log,
                      architecture::ArchitecturePreference::
                          prefer_arm64_prompt_fallback},
        selection_lifecycle{states, clock, log, architectures, resolver, network,
                            detector, launcher,
                            azzs::domain::StateSubject{"history-user"}},
        settings{settings_catalog, settings_platform, recovery_store, occupancy,
                 log},
        history{clock, updates, platform, hardware, catalog_lifecycle, log,
                installation_batches, optimization_batches, settings,
                selection_lifecycle, &debug_source} {
    debug_source.value = debug_policy(app::DebugModeState::enabled);
  }

  [[nodiscard]] bool prepare() {
    auto const selection_restored = selection_lifecycle.restore();
    auto const catalog_replaced =
        selection_lifecycle.on_catalog_replaced(selection_catalog_projection());
    auto const runtime = selection_catalog();
    auto const& primary = runtime.software.front().definition.sources.front();
    auto const resolved = selection_lifecycle.resolve_declared_source("editor", primary);
    auto const opened = selection_lifecycle.begin_external_handoff("editor", primary);
    auto const rechecked = selection_lifecycle.detect_external_install("editor");
    auto const batches_restored = optimization_batches.restore();
    auto const created = optimization_batches.create(
        {.batch_id = "optimization-history-batch",
         .correlation_id = "requested-optimization-correlation",
         .frozen_at_milliseconds = 101});
    auto const advanced = optimization_batches.advance();
    log.record("frozen-optimization-correlation", "retained-frozen-marker");
    log.record("unrelated-correlation", "unrelated-marker", "unrelated-item");
    return selection_restored.succeeded() && catalog_replaced.succeeded() &&
           resolved.succeeded() && opened.succeeded() && rechecked.succeeded() &&
           batches_restored.succeeded() && created.succeeded() && advanced.succeeded();
  }

  InMemoryStateFileSystem files;
  FixedClock clock;
  app::DeviceStateStore states;
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource tokens;
  app::SharedOperationOccupancy occupancy;
  ProjectionLog log;
  FixedPlatform platform;
  IdleActivity activity;
  EmptyUpdatePlatform update_platform;
  app::ApplicationUpdateLifecycle updates;
  EmptyHardwareObserver hardware_observer;
  app::HardwareOverviewService hardware;
  EmptyCatalogFiles catalog_files;
  EmptyCatalogCodec catalog_codec;
  catalog::SoftwareCatalogPolicy catalog_policy;
  ClosedCatalogAccess catalog_access;
  catalog_app::SoftwareCatalogLifecycle catalog_lifecycle;
  NoopDownload download;
  NoopInstaller installer;
  NoopReadiness readiness;
  NoopVerifier verifier;
  NoopInstallationFacts installation_facts;
  AcceptingInstallationPlan installation_admission;
  app::installation_batch::InstallationBatchService installation_batches;
  FixedOptimizationPlans optimization_plans;
  ImmediateOptimizationExecutor optimization_executor;
  AllowedWithdrawals withdrawals;
  optimization_app::SoftwareOptimizationBatchService optimization_batches;
  app::architecture_selection::ArchitectureSelectionLifecycle architectures;
  FixtureResolver resolver;
  AvailableNetwork network;
  AbsentDetector detector;
  AcceptingLauncher launcher;
  selection_app::SoftwareSelectionLifecycle selection_lifecycle;
  EmptySettingsCatalog settings_catalog;
  EmptySettingsPlatform settings_platform;
  EmptyRecoveryStore recovery_store;
  app::SystemSettingsApplyService settings;
  MutableDebugPolicySource debug_source;
  app::HistoryAndLogsService history;
};

[[nodiscard]] app::HistoryEntryProjection const* find_entry(
    app::HistoryAndLogsSnapshot const& snapshot, app::HistoryEntryKind kind,
    std::string_view stable_id) {
  auto const found = std::find_if(
      snapshot.history.begin(), snapshot.history.end(), [&](auto const& entry) {
        return entry.kind == kind && entry.stable_id == stable_id;
      });
  return found == snapshot.history.end() ? nullptr : &*found;
}

[[nodiscard]] app::HistoryTimelineProjection const* find_timeline(
    app::HistoryEntryProjection const& entry, std::string_view detail) {
  auto const found = std::find_if(
      entry.timeline.begin(), entry.timeline.end(), [&](auto const& timeline) {
        return timeline.detail == detail;
      });
  return found == entry.timeline.end() ? nullptr : &*found;
}

[[nodiscard]] app::HistoryFactProjection const* find_fact(
    std::vector<app::HistoryFactProjection> const& facts, std::string_view key) {
  auto const found = std::find_if(facts.begin(), facts.end(), [&](auto const& fact) {
    return fact.key == key;
  });
  return found == facts.end() ? nullptr : &*found;
}

[[nodiscard]] bool has_fact(std::vector<app::HistoryFactProjection> const& facts,
                            std::string_view key, std::string_view value) {
  auto const* fact = find_fact(facts, key);
  return fact != nullptr &&
         fact->disposition == app::HistoryFactDisposition::obtained &&
         fact->value == value;
}

[[nodiscard]] bool has_missing_fact(
    std::vector<app::HistoryFactProjection> const& facts, std::string_view key,
    std::string_view reason) {
  auto const* fact = find_fact(facts, key);
  return fact != nullptr &&
         fact->disposition == app::HistoryFactDisposition::not_obtained &&
         fact->reason == reason;
}

[[nodiscard]] bool stable_history_taxonomy_is_available() {
  bool passed = true;
  passed &= expect(
      std::string{app::to_string(app::HistoryEntryKind::installation_batch)} ==
          "installation-batch",
      "installation batches must retain their stable history kind");
  passed &= expect(
      std::string{app::to_string(app::HistoryEntryKind::system_setting_apply)} ==
          "system-setting-apply",
      "system setting operations must be independently addressable");
  passed &= expect(
      std::string{app::to_string(app::HistoryEntryKind::restart_resume)} ==
          "restart-resume",
      "restart handoffs must be independently addressable");
  return passed;
}

[[nodiscard]] bool immutable_owner_timeline_and_missing_facts_are_projected() {
  HistoryFixture fixture;
  if (!expect(fixture.prepare(), "fake owners must prepare immutable snapshots")) {
    return false;
  }
  auto const snapshot = fixture.history.refresh();
  auto const* entry = find_entry(snapshot, app::HistoryEntryKind::external_install_handoff,
                                 "editor");
  if (!expect(entry != nullptr,
              "an external handoff must be projected from its durable owner")) {
    return false;
  }
  auto const* opened = find_timeline(*entry, "declared-address-opened");
  auto const* rechecked = find_timeline(*entry, "returned-for-recheck");
  bool passed = true;
  passed &= expect(entry->detail == "returned-for-recheck" &&
                       has_fact(entry->facts, "handoff.timeline_fact_count", "2"),
                   "current handoff presentation must identify the immutable latest fact");
  passed &= expect(opened != nullptr &&
                       has_fact(opened->facts, "source.version", "2.3.4") &&
                       has_missing_fact(
                           opened->facts, "source.resolved_address",
                           "the address is available only through the centrally redacted event trail"),
                   "a handoff must retain its frozen resolved version without exposing addresses");
  passed &= expect(rechecked != nullptr &&
                       has_missing_fact(rechecked->facts, "source.snapshot",
                                        "not-captured-for-this-fact") &&
                       has_missing_fact(
                           rechecked->facts, "handoff.detail",
                           "raw handoff detail is available only through the centrally redacted event trail"),
                   "facts absent from an immutable handoff event must remain explicitly unavailable");
  return passed;
}

[[nodiscard]] bool immutable_system_settings_facts_are_projected() {
  HistoryFixture fixture;
  if (!expect(fixture.prepare(),
              "fake owners must prepare immutable settings operation facts")) {
    return false;
  }
  auto const snapshot = fixture.history.refresh();
  auto const* applied = find_entry(snapshot,
                                   app::HistoryEntryKind::system_setting_apply,
                                   "system-settings-operation/41");
  auto const* restored = find_entry(snapshot,
                                    app::HistoryEntryKind::system_setting_apply,
                                    "system-settings-operation/42");
  if (!expect(applied != nullptr && restored != nullptr,
              "each immutable system settings operation fact must have a projection")) {
    return false;
  }

  auto const* applied_setting = find_timeline(*applied, "Classic context menu");
  auto const* restored_setting =
      find_timeline(*restored, "Classic context menu");
  bool passed = true;
  passed &= expect(
      has_fact(applied->facts, "settings.operation", "apply") &&
          has_fact(applied->facts, "settings.selection_source",
                   "recommended-overall") &&
          has_fact(applied->facts, "settings.selected_plan_id",
                   "plan.recommended") &&
          has_fact(applied->facts, "settings.windows_display_version",
                   "Windows 11 25H2") &&
          has_fact(applied->facts, "settings.windows_internal_build", "26200") &&
          has_fact(applied->facts, "settings.explorer_restart_requested", "true") &&
          has_fact(applied->facts, "settings.explorer_restart_result",
                   "deferred"),
      "apply history must retain its frozen source, Windows version/build, and Explorer result");
  passed &= expect(
      applied_setting != nullptr &&
          has_fact(applied_setting->facts, "settings.declared_windows_range",
                   "windows-11-25H2..windows-11-25H2") &&
          has_fact(applied_setting->facts, "settings.force_attempt_confirmed",
                   "true") &&
          has_fact(applied_setting->facts, "settings.original_value", "windows11") &&
          has_fact(applied_setting->facts, "settings.target_value", "classic") &&
          has_fact(applied_setting->facts, "settings.recovery_record_id", "7"),
      "apply history must retain declared scope, force confirmation, original and target values");
  passed &= expect(
      has_fact(restored->facts, "settings.operation", "restore") &&
          has_fact(restored->facts, "settings.selection_source", "individual") &&
          has_fact(restored->facts, "settings.explorer_restart_result",
                   "succeeded") &&
          has_missing_fact(
              restored->facts, "settings.catalog_identity",
              "NOT_OBTAINED: restore uses frozen recovery record, not current catalog") &&
          has_missing_fact(
              restored->facts, "settings.windows_display_version",
              "NOT_OBTAINED: Windows environment was not captured"),
      "restore history must retain its own result and mark unavailable operation facts");
  passed &= expect(
      restored_setting != nullptr &&
          has_fact(restored_setting->facts, "settings.undo", "true") &&
          has_fact(restored_setting->facts, "settings.restore_value", "windows11") &&
          has_missing_fact(
              restored_setting->facts, "settings.controlled_identity",
              "NOT_OBTAINED: immutable operation did not capture a controlled setting identity") &&
          has_missing_fact(
              restored_setting->facts, "settings.declared_windows_range",
              "NOT_OBTAINED: recovery record did not retain declared Windows range"),
      "undo history must retain frozen restore values without inventing missing scope");
  passed &= expect(
      std::none_of(snapshot.history.begin(), snapshot.history.end(),
                   [](auto const& entry) {
                     return entry.kind == app::HistoryEntryKind::system_setting_recovery;
                   }),
      "current recovery records must not be projected as immutable settings history");
  return passed;
}

[[nodiscard]] bool frozen_correlation_generation_and_value_shape_are_preserved() {
  HistoryFixture fixture;
  if (!expect(fixture.prepare(), "fake owners must prepare optimization history")) {
    return false;
  }
  auto const snapshot = fixture.history.refresh();
  auto const* entry = find_entry(snapshot,
                                 app::HistoryEntryKind::software_optimization_batch,
                                 "optimization-history-batch");
  if (!expect(entry != nullptr,
              "the active optimization batch must have a history projection")) {
    return false;
  }
  bool passed = true;
  passed &= expect(
      has_fact(entry->facts, "last_durable_transition.generation", "4"),
      "a valid durable transition must retain its generation");
  passed &= expect(
      has_fact(entry->facts,
               "scheme.history-scheme.option.history-option.selected_value_kind",
               "no-value-parameter"),
      "a known valueless option must not be mislabeled as an unavailable fact");

  auto const located = fixture.history.locate("optimization-history-batch");
  passed &= expect(located.history.size() == 1 && !located.log.events.empty() &&
                       std::all_of(
                           located.log.events.begin(), located.log.events.end(),
                           [](auto const& event) {
                             return event.correlation.value ==
                                    "frozen-optimization-correlation";
                           }),
                   "locate must use the frozen batch correlation instead of the batch stable id");
  return passed;
}

[[nodiscard]] bool debug_policy_is_read_only_and_missing_sources_fail_closed() {
  HistoryFixture fixture;
  if (!expect(fixture.prepare(), "fake owners must prepare debug policy history")) {
    return false;
  }
  auto const retained_count = fixture.log.events.size();
  auto const enabled = fixture.history.refresh();
  fixture.debug_source.value = debug_policy(app::DebugModeState::disabled);
  auto const disabled = fixture.history.refresh();
  bool passed = true;
  passed &= expect(enabled.debug.facts_available &&
                       enabled.debug.debug_mode == app::DebugModeState::enabled &&
                       disabled.debug.debug_mode == app::DebugModeState::disabled &&
                       fixture.log.events.size() == retained_count,
                   "debug policy reads must not delete retained logs when debug mode changes");

  auto const exported = fixture.history.export_diagnostic();
  passed &= expect(exported.code == app::HistoryAndLogsActionCode::succeeded &&
                       fixture.log.exported_context.has_value() &&
                       fixture.log.exported_context->debug_log_coverage.value.find(
                           "mode=disabled") != std::string::npos,
                   "diagnostic export must consume the injected debug policy snapshot");

  app::HistoryAndLogsService missing_policy{
      fixture.clock, fixture.updates, fixture.platform, fixture.hardware,
      fixture.catalog_lifecycle, fixture.log, fixture.installation_batches,
      fixture.optimization_batches, fixture.settings, fixture.selection_lifecycle};
  auto const missing = missing_policy.refresh();
  passed &= expect(!missing.debug.facts_available &&
                       missing.debug.not_obtained_reason ==
                           "debug log policy is not provided by the current composition root",
                   "a missing debug policy source must remain explicitly unavailable");
  return passed;
}

}  // namespace

int main() {
  auto const passed = stable_history_taxonomy_is_available() &&
                      immutable_owner_timeline_and_missing_facts_are_projected() &&
                      immutable_system_settings_facts_are_projected() &&
                      frozen_correlation_generation_and_value_shape_are_preserved() &&
                      debug_policy_is_read_only_and_missing_sources_fail_closed();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
