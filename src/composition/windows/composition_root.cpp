#include "pch.h"

#include "composition_root.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <filesystem>
#include <thread>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../adapters/ui/winui/DesignSystem/motion_preferences.hpp"
#include "../../adapters/ui/winui/MainWindow.xaml.h"
#include "azzs/adapters/infrastructure/local_file_log_storage.hpp"
#include "azzs/adapters/infrastructure/local_package_cache_storage.hpp"
#include "azzs/adapters/infrastructure/local_software_optimization_catalog_file.hpp"
#include "azzs/adapters/infrastructure/software_catalog_file.hpp"
#include "azzs/adapters/infrastructure/settings_catalog_file_adapter.hpp"
#include "azzs/adapters/infrastructure/state_application_update_health_storage.hpp"
#include "azzs/adapters/infrastructure/state_operation_occupancy_storage.hpp"
#include "azzs/adapters/infrastructure/structured_execution_log.hpp"
#include "azzs/adapters/infrastructure/system_settings_recovery_store.hpp"
#include "azzs/adapters/infrastructure/system_clock.hpp"
#include "azzs/adapters/windows/bundled_catalog_resource_reader.hpp"
#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/adapters/windows/windows_driver_acquisition.hpp"
#include "azzs/adapters/windows/windows_application_update_platform.hpp"
#include "azzs/adapters/windows/windows_emergency_withdrawal_notice_source.hpp"
#include "azzs/adapters/windows/windows_external_address_launcher.hpp"
#include "azzs/adapters/windows/windows_hardware_observer.hpp"
#include "azzs/adapters/windows/windows_installation_batch_adapters.hpp"
#include "azzs/adapters/windows/windows_lease_token_source.hpp"
#include "azzs/adapters/windows/windows_platform_info.hpp"
#include "azzs/adapters/windows/windows_restart_resume_registration.hpp"
#include "azzs/adapters/windows/windows_state_file_system.hpp"
#include "azzs/adapters/windows/windows_sogou_optimization_adapter.hpp"
#include "azzs/adapters/windows/windows_system_settings_adapter.hpp"
#include "azzs/adapters/windows/windows_view_preferences.hpp"
#include "azzs/application/clock.hpp"
#include "azzs/application/application_settings.hpp"
#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/advanced_view_preferences.hpp"
#include "azzs/application/application_update.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/debug_mode_catalog_editor.hpp"
#include "azzs/application/debug_log_policy/debug_log_policy.hpp"
#include "azzs/application/driver_acquisition.hpp"
#include "azzs/application/guided_initialization.hpp"
#include "azzs/application/emergency_withdrawal_service.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/history_and_logs.hpp"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/restart_resume.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/application/software_optimization_batch.hpp"
#include "azzs/application/software_optimization_discovery.hpp"
#include "azzs/application/sogou_optimization.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/application/workbench.hpp"
#include "azzs/application/workbench_services.hpp"
#include "azzs/settings_catalog/initial_settings_catalog.hpp"
#include "azzs/settings_catalog/settings_catalog_lifecycle.hpp"

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace azzs::composition::windows {
namespace {

class ProductionSettingsCatalogImportAuthorization final
    : public application::settings_catalog::SettingsCatalogImportAuthorization {
 public:
  [[nodiscard]] bool debug_import_allowed() const noexcept override {
    return false;
  }
};

class ProductionSoftwareOptimizationCatalogDebugAuthorization final
    : public application::SoftwareOptimizationCatalogDebugAuthorization {
 public:
  [[nodiscard]] bool local_import_allowed() const noexcept override {
    return false;
  }
};

class EmbeddedSettingsCatalogUpdateSource final
    : public application::TrustedSettingsCatalogUpdateSource {
 public:
  [[nodiscard]] application::TrustedSettingsCatalogUpdateRead read_update()
      override {
    return {.candidate = application::settings_catalog::initial_settings_catalog()};
  }
};

class EmbeddedSoftwareOptimizationCatalogUpdateSource final
    : public application::TrustedSoftwareOptimizationCatalogUpdateSource {
 public:
  explicit EmbeddedSoftwareOptimizationCatalogUpdateSource(std::string source)
      : source_(std::move(source)) {}

  [[nodiscard]] application::TrustedSoftwareOptimizationCatalogUpdateRead
  read_update() override {
    if (source_.empty()) {
      return {.detail = "embedded software optimization catalog is unavailable"};
    }
    return {.update = application::TrustedSoftwareOptimizationCatalogUpdate{
                .source = source_,
                .source_reference = "embedded-software-optimization-catalog"}};
  }

 private:
  std::string const source_;
};

class UnavailableControlledSourceResolver final
    : public application::software_selection::ControlledSourceResolver {
 public:
  [[nodiscard]] application::software_selection::SourceResolutionResult resolve(
      std::string_view,
      domain::software_catalog::CatalogSource const&) override {
    return {.error = "controlled source resolution is not configured"};
  }
};

class OfflineNetworkObserver final
    : public application::software_selection::NetworkObserver,
      public application::offline_package_cache::PackageCacheNetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override { return false; }
};

[[nodiscard]] std::filesystem::path path_from_utf8(std::string const& value) {
  auto const* begin = reinterpret_cast<char8_t const*>(value.data());
  return std::filesystem::path{std::u8string{begin, begin + value.size()}};
}

struct BundledCatalogResources final {
  adapters::windows::VerifiedBundledCatalogResource software_catalog;
  adapters::windows::VerifiedBundledCatalogResource
      software_optimization_catalog;
};

constexpr std::uintmax_t kSoftwareCatalogBytes = 8894;
constexpr std::array<std::uint8_t, 32> kSoftwareCatalogSha256{
    0xe2, 0x45, 0xe0, 0x95, 0xa2, 0xc5, 0x44, 0xbd,
    0x37, 0x6a, 0x56, 0x4c, 0xe3, 0x26, 0x63, 0x22,
    0x50, 0x38, 0x10, 0x51, 0xa8, 0x29, 0xac, 0x05,
    0x0d, 0xf3, 0x36, 0xef, 0xc6, 0xd7, 0x67, 0x8a};
constexpr std::uintmax_t kSoftwareOptimizationCatalogBytes = 18010;
constexpr std::array<std::uint8_t, 32> kSoftwareOptimizationCatalogSha256{
    0x59, 0x1b, 0xc9, 0x26, 0x97, 0x1a, 0x39, 0x05,
    0xb6, 0x7b, 0x67, 0xc3, 0x21, 0x2d, 0x1e, 0xad,
    0x8b, 0xf7, 0x5a, 0x8e, 0x4d, 0x28, 0x2b, 0x3c,
    0xb8, 0xbd, 0xcf, 0x26, 0x3c, 0xa7, 0x14, 0x37};

[[nodiscard]] std::optional<std::filesystem::path> workbench_module_directory() {
  std::vector<wchar_t> buffer(512);
  for (;;) {
    auto const length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::nullopt;
    }
    // GetModuleFileNameW returns the required length excluding the terminator
    // when the complete path fits. A valid path may therefore use the final
    // character slot; truncation is reported by returning the buffer size.
    if (length < buffer.size()) {
      auto const module = std::filesystem::path{
          std::wstring{buffer.data(), length}};
      auto const directory = module.parent_path();
      return directory.empty() ? std::nullopt
                               : std::optional<std::filesystem::path>{directory};
    }
    if (buffer.size() >= 32768) {
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2);
  }
}

// `bundled_catalog_paths` delegates the path_chain_has_no_reparse_points and
// bundled_catalog_resource_matches checks to one handle-backed reader, so no
// later consumer receives a package path that it could reopen.
[[nodiscard]] std::optional<BundledCatalogResources> bundled_catalog_paths() {
  auto const module_directory = workbench_module_directory();
  if (!module_directory.has_value()) {
    return std::nullopt;
  }

  adapters::windows::WindowsBundledCatalogResourceReader reader{
      *module_directory};
  auto software_catalog = reader.read(
      "catalog/software-catalog.toml",
      {.byte_count = kSoftwareCatalogBytes, .sha256 = kSoftwareCatalogSha256});
  auto software_optimization_catalog = reader.read(
      "catalog/software-optimization-catalog.toml",
      {.byte_count = kSoftwareOptimizationCatalogBytes,
       .sha256 = kSoftwareOptimizationCatalogSha256});
  if (!software_catalog || !software_optimization_catalog) {
    return std::nullopt;
  }
  return BundledCatalogResources{
      .software_catalog = std::move(*software_catalog.resource),
      .software_optimization_catalog =
          std::move(*software_optimization_catalog.resource),
  };
}

// Runs the real lifecycle gate before any workbench service is constructed.
// A pure parse check would miss its persistence, log, and occupancy failures.
class StartupOptimizationCatalogGate final {
 public:
  explicit StartupOptimizationCatalogGate(
      adapters::windows::DeviceDataEnvironment const& environment)
      : clock_{},
        state_files_(environment),
        states_(state_files_, clock_),
        log_storage_(environment.root_utf8, environment.subject_id),
        log_(log_storage_, clock_),
        occupancy_storage_(states_),
        occupancy_(occupancy_storage_, lease_tokens_),
        optimization_catalog_(
            states_, log_, occupancy_, optimization_catalog_file_,
            optimization_catalog_debug_authorization_,
            application::sogou_optimization::built_in_rule_definitions(),
            optimization_installer_baselines_) {}

  [[nodiscard]] bool ensure_bundled_catalog(
      BundledCatalogResources const& resources) {
    auto const result = optimization_catalog_.ensure_builtin(
        resources.software_optimization_catalog.bytes(),
        "embedded-software-optimization-catalog");
    auto const accepted =
        result.code == application::SoftwareOptimizationCatalogLifecycleCode::
                           applied ||
        result.code == application::SoftwareOptimizationCatalogLifecycleCode::
                           unchanged;
    return accepted && result.logging_error.empty() &&
           result.occupancy_error.empty();
  }

 private:
  adapters::infrastructure::SystemClock clock_;
  adapters::windows::WindowsStateFileSystem state_files_;
  application::DeviceStateStore states_;
  adapters::infrastructure::LocalFileLogStorage log_storage_;
  adapters::infrastructure::StructuredExecutionLog log_;
  adapters::infrastructure::StateOperationOccupancyStorage occupancy_storage_;
  adapters::windows::WindowsLeaseTokenSource lease_tokens_;
  application::SharedOperationOccupancy occupancy_;
  adapters::infrastructure::LocalSoftwareOptimizationCatalogFile
      optimization_catalog_file_;
  ProductionSoftwareOptimizationCatalogDebugAuthorization
      optimization_catalog_debug_authorization_;
  std::vector<domain::software_optimization_catalog::
                  SoftwareCatalogInstallerBaseline>
      optimization_installer_baselines_{{
          .software_item_id = {"sogou-input"},
          .installer_baseline_id = {"sogou-input-windows-16.7"},
          .installed_versions = {"16.7", "16.7"},
      }};
  application::SoftwareOptimizationCatalogLifecycle optimization_catalog_;
};

class UnavailableSoftwarePresenceDetector final
    : public application::software_selection::SoftwarePresenceDetector {
 public:
  [[nodiscard]] application::software_selection::PresenceDetection detect(
      std::string_view) override {
    return {.detail = "software presence detection is not configured"};
  }
};

class WindowsGuidedInitializationEvidenceSource final
    : public application::guided_initialization::EvidenceSource {
 public:
  WindowsGuidedInitializationEvidenceSource(
      application::driver_acquisition::DriverAcquisitionService& drivers,
      application::SystemSettingsApplyService& system_settings,
      application::installation_batch::InstallationBatchService& installations,
      application::software_optimization_discovery::
          SoftwareOptimizationDiscoveryService& discovery,
      application::software_optimization_batch::SoftwareOptimizationBatchService&
          optimization_batches,
      application::software_selection::SoftwareSelectionLifecycle& selection,
      application::software_catalog::SoftwareCatalogLifecycle& software_catalog,
      application::SoftwareOptimizationCatalogLifecycle& optimization_catalog,
      application::restart_resume::RestartResumeService& restart_resume)
      : drivers_(drivers),
        system_settings_(system_settings),
        installations_(installations),
        discovery_(discovery),
        optimization_batches_(optimization_batches),
        selection_(selection),
        software_catalog_(software_catalog),
        optimization_catalog_(optimization_catalog),
        restart_resume_(restart_resume) {}

  [[nodiscard]] application::guided_initialization::Evidence observe() override {
    using namespace application::guided_initialization;

    Evidence result;
    result.drivers = observe_drivers(drivers_.snapshot());
    result.system_optimization = observe_system_settings(system_settings_.snapshot());

    auto const selection = selection_.snapshot();
    result.external_handoffs.reserve(selection.handoffs.size());
    for (auto const& handoff : selection.handoffs) {
      ExternalHandoffState state{};
      switch (handoff.status) {
        case domain::software_selection::ExternalHandoffStatus::externally_recognized:
        case domain::software_selection::ExternalHandoffStatus::awaiting_user_confirmation:
          state = ExternalHandoffState::externally_recognized;
          break;
        case domain::software_selection::ExternalHandoffStatus::waiting_for_external_install:
          state = ExternalHandoffState::waiting_for_external_install;
          break;
        case domain::software_selection::ExternalHandoffStatus::skipped:
        case domain::software_selection::ExternalHandoffStatus::completed:
          state = ExternalHandoffState::skipped;
          break;
        case domain::software_selection::ExternalHandoffStatus::none:
          continue;
      }
      result.external_handoffs.push_back({.software_id = handoff.software_id,
                                          .state = state});
    }

    result.software_installation =
        observe_installation(installations_.snapshot(), result.external_handoffs,
                             selection);
    result.software_optimization =
        observe_software_optimization(optimization_batches_.snapshot(),
                                      discovery_.snapshot());

    result.restart_gate = observe_restart_gate(restart_resume_.snapshot());
    auto const catalog = software_catalog_.snapshot();
    auto const optimization_catalog = optimization_catalog_.snapshot();
    result.local_trial_software_catalog =
        (catalog.current.has_value() &&
         catalog.current->identity ==
             application::software_catalog::EffectiveCatalogIdentity::local_trial) ||
        (optimization_catalog.current_provenance.has_value() &&
         optimization_catalog.current_provenance->local_trial);
    return result;
  }

  [[nodiscard]] bool continue_external_handoff(std::string_view software_id,
                                                std::string& error) override {
    auto result = selection_.continue_external_handoff(software_id);
    if (!result.succeeded()) {
      error = result.message.empty() ? "software selection rejected handoff continuation"
                                      : result.message;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool continue_after_restart(std::string& error) override {
    auto result = restart_resume_.confirm_continue();
    if (!result.succeeded()) {
      error = result.message.empty() ? "restart resume rejected continuation"
                                      : result.message;
      return false;
    }
    return true;
  }

 private:
  using GuidedStageEvidence = application::guided_initialization::StageEvidence;
  using GuidedStageState = application::guided_initialization::StageState;

  [[nodiscard]] static GuidedStageEvidence observe_drivers(
      application::driver_acquisition::DriverAcquisitionSnapshot const& source) {
    switch (source.state) {
      case application::driver_acquisition::DriverAcquisitionState::handoff_in_progress:
        return {.state = GuidedStageState::external_handoff,
                .detail = source.detail.empty()
                              ? "driver handoff is in progress"
                              : source.detail};
      case application::driver_acquisition::DriverAcquisitionState::awaiting_user_decision:
        return {.state = GuidedStageState::result_confirmation_pending,
                .detail = source.detail.empty()
                              ? "driver handoff needs an explicit result"
                              : source.detail};
      case application::driver_acquisition::DriverAcquisitionState::waiting_for_restart:
        return {.state = GuidedStageState::waiting_for_restart,
                .detail = source.detail.empty() ? "driver handoff is waiting for restart"
                                                : source.detail};
      case application::driver_acquisition::DriverAcquisitionState::read_only:
        return {.state = GuidedStageState::not_executed,
                .detail = source.detail.empty() ? "driver handoff is read-only"
                                                : source.detail};
      case application::driver_acquisition::DriverAcquisitionState::not_restored:
        return {.state = GuidedStageState::pending,
                .detail = source.detail.empty() ? "driver handoff is not restored"
                                                : source.detail};
      case application::driver_acquisition::DriverAcquisitionState::ready:
        return {.state = GuidedStageState::pending,
                .detail = source.detail.empty() ? "driver handoff is available"
                                                : source.detail};
    }
    return {};
  }

  [[nodiscard]] static GuidedStageEvidence observe_system_settings(
      application::SystemSettingsApplySnapshot const& source) {
    using State = application::SystemSettingApplyState;
    if (source.waiting_for_explorer_restart) {
      return {.state = GuidedStageState::waiting_explorer_restart,
              .detail = source.detail.empty()
                            ? "system settings are waiting for Explorer restart"
                            : source.detail};
    }
    bool any_failed = false;
    bool all_not_applicable = !source.settings.empty();
    bool all_effective = !source.settings.empty();
    for (auto const& setting : source.settings) {
      any_failed = any_failed || setting.state == State::failed;
      all_not_applicable =
          all_not_applicable && setting.state == State::not_applicable;
      all_effective =
          all_effective &&
          (setting.state == State::already_effective ||
           setting.state == State::applied);
    }
    if (any_failed || source.status == application::SystemSettingsSnapshotStatus::failed) {
      return {.state = GuidedStageState::failed,
              .detail = source.detail.empty() ? "system settings reported a failure"
                                               : source.detail};
    }
    if (all_not_applicable) {
      return {.state = GuidedStageState::no_applicable_items,
              .detail = source.detail.empty() ? "no system settings apply here"
                                               : source.detail};
    }
    if (all_effective && source.status == application::SystemSettingsSnapshotStatus::completed) {
      return {.state = GuidedStageState::completed,
              .detail = source.detail.empty() ? "system settings are effective"
                                               : source.detail};
    }
    switch (source.status) {
      case application::SystemSettingsSnapshotStatus::applying:
        return {.state = GuidedStageState::active,
                .detail = source.detail.empty() ? "system settings are applying"
                                                 : source.detail};
      case application::SystemSettingsSnapshotStatus::completed:
        return {.state = GuidedStageState::completed,
                .detail = source.detail.empty() ? "system settings completed"
                                                 : source.detail};
      case application::SystemSettingsSnapshotStatus::unavailable:
      case application::SystemSettingsSnapshotStatus::ready:
        return {.state = GuidedStageState::pending,
                .detail = source.detail.empty() ? "system settings are ready for review"
                                                 : source.detail};
      case application::SystemSettingsSnapshotStatus::failed:
        return {.state = GuidedStageState::failed,
                .detail = source.detail.empty() ? "system settings reported a failure"
                                                 : source.detail};
    }
    return {};
  }

  [[nodiscard]] static GuidedStageEvidence observe_installation(
      domain::installation_batch::InstallationBatchSnapshot const& source,
      std::vector<application::guided_initialization::ExternalHandoffEvidence> const& handoffs,
      application::software_selection::SoftwareSelectionSnapshot const& selection) {
    using BatchState = domain::installation_batch::InstallationBatchState;
    using Stage = GuidedStageState;
    auto from_batch_state = [](BatchState state) -> GuidedStageEvidence {
      switch (state) {
        case BatchState::completed:
          return {.state = Stage::completed, .detail = "installation batch completed"};
        case BatchState::stopped:
          return {.state = Stage::partial, .detail = "installation batch was stopped"};
        case BatchState::waiting_restart:
          return {.state = Stage::waiting_for_restart,
                  .detail = "installation batch is waiting for restart"};
        case BatchState::recovery_required:
        case BatchState::failed_closed:
          return {.state = Stage::failed,
                  .detail = "installation batch needs explicit recovery"};
        case BatchState::ready:
        case BatchState::running:
        case BatchState::download_paused:
        case BatchState::stopping:
        case BatchState::awaiting_user:
        case BatchState::closing:
          return {.state = Stage::active, .detail = "installation batch is active"};
      }
      return {};
    };
    if (source.active.has_value()) {
      return from_batch_state(source.active->state);
    }
    auto const unresolved = std::ranges::find_if(
        handoffs, [](auto const& handoff) {
          return handoff.state ==
                     application::guided_initialization::ExternalHandoffState::
                         waiting_for_external_install ||
                 handoff.state ==
                     application::guided_initialization::ExternalHandoffState::
                         externally_recognized;
        });
    if (unresolved != handoffs.end()) {
      return {.state = Stage::external_handoff,
              .detail = "software installation needs an external handoff"};
    }
    if (!source.history.empty()) {
      return from_batch_state(source.history.back().final_state);
    }
    if (selection.active_catalog.has_value() && selection.items.empty()) {
      return {.state = Stage::no_applicable_items,
              .detail = "no software is available in the current catalog"};
    }
    return {.state = Stage::pending,
            .detail = "software installation is ready for selection"};
  }

  [[nodiscard]] static GuidedStageEvidence observe_software_optimization(
      domain::software_optimization_batch::OptimizationBatchSnapshot const& batch,
      application::software_optimization_discovery::SoftwareOptimizationDiscoverySnapshot const& discovery) {
    using BatchState = domain::software_optimization_batch::OptimizationBatchState;
    auto from_batch_state = [](BatchState state) -> GuidedStageEvidence {
      switch (state) {
        case BatchState::completed:
          return {.state = GuidedStageState::completed,
                  .detail = "software optimization batch completed"};
        case BatchState::stopped:
          return {.state = GuidedStageState::partial,
                  .detail = "software optimization batch was stopped"};
        case BatchState::waiting_restart:
          return {.state = GuidedStageState::waiting_for_restart,
                  .detail = "software optimization is waiting for restart"};
        case BatchState::recovery_required:
        case BatchState::failed_closed:
          return {.state = GuidedStageState::failed,
                  .detail = "software optimization needs explicit recovery"};
        case BatchState::ready:
        case BatchState::running:
        case BatchState::stopping:
        case BatchState::awaiting_user:
        case BatchState::closing:
          return {.state = GuidedStageState::active,
                  .detail = "software optimization batch is active"};
      }
      return {};
    };
    if (batch.active.has_value()) {
      return from_batch_state(batch.active->state);
    }
    if (!batch.history.empty()) {
      return from_batch_state(batch.history.back().final_state);
    }
    if (!discovery.has_current_catalog || discovery.discovery.targets.empty()) {
      return {.state = GuidedStageState::no_applicable_items,
              .detail = "no software optimization is currently available"};
    }
    bool has_error = false;
    bool has_action = false;
    bool has_optimization = false;
    for (auto const& target : discovery.discovery.targets) {
      has_error = has_error || target.first_release_implementation_error;
      for (auto const& scheme : target.schemes) {
        has_optimization = true;
        has_action = has_action ||
                     scheme.state ==
                         domain::software_optimization_discovery::SchemeState::
                             can_optimize ||
                     scheme.state ==
                         domain::software_optimization_discovery::SchemeState::
                             needs_attention;
      }
    }
    if (has_error) {
      return {.state = GuidedStageState::failed,
              .detail = discovery.error.empty()
                            ? "software optimization catalog is incomplete"
                            : discovery.error};
    }
    if (!has_optimization || !has_action) {
      return {.state = GuidedStageState::no_applicable_items,
              .detail = "no executable software optimization is available"};
    }
    return {.state = GuidedStageState::pending,
            .detail = "software optimization recommendations are ready"};
  }

  [[nodiscard]] static application::guided_initialization::RestartGateState
  observe_restart_gate(application::restart_resume::RestartResumeSnapshot const& source) {
    using State = application::restart_resume::RestartResumeState;
    switch (source.state) {
      case State::idle:
        return application::guided_initialization::RestartGateState::none;
      case State::waiting_for_windows_restart:
        return application::guided_initialization::RestartGateState::
            waiting_for_windows_restart;
      case State::awaiting_read_only_verification:
        return application::guided_initialization::RestartGateState::
            awaiting_read_only_verification;
      case State::awaiting_user_decision:
        return application::guided_initialization::RestartGateState::
            awaiting_user_continue;
      case State::read_only:
        return application::guided_initialization::RestartGateState::read_only;
    }
    return application::guided_initialization::RestartGateState::none;
  }

  application::driver_acquisition::DriverAcquisitionService& drivers_;
  application::SystemSettingsApplyService& system_settings_;
  application::installation_batch::InstallationBatchService& installations_;
  application::software_optimization_discovery::
      SoftwareOptimizationDiscoveryService& discovery_;
  application::software_optimization_batch::SoftwareOptimizationBatchService&
      optimization_batches_;
  application::software_selection::SoftwareSelectionLifecycle& selection_;
  application::software_catalog::SoftwareCatalogLifecycle& software_catalog_;
  application::SoftwareOptimizationCatalogLifecycle& optimization_catalog_;
  application::restart_resume::RestartResumeService& restart_resume_;
};

class WindowsWorkbenchServices final
    : public application::WorkbenchServices,
      public std::enable_shared_from_this<WindowsWorkbenchServices> {
 public:
  explicit WindowsWorkbenchServices(
      BundledCatalogResources bundled_catalog_resources,
      adapters::windows::DeviceDataEnvironment environment,
      std::shared_ptr<application::ArchitecturePreferences>
          architecture_preferences,
      std::shared_ptr<application::CacheRetentionPreferences>
          cache_retention_preferences,
      std::shared_ptr<application::DebugModePreferenceStore>
          debug_mode_preferences)
      : state_subject_{environment.subject_id},
        clock_{},
        hardware_observer_{},
        hardware_overview_(hardware_observer_, clock_),
        state_files_(environment),
        states_(state_files_, clock_),
        restart_resume_registration_(),
        restart_resume_(states_, restart_resume_registration_),
        log_storage_(environment.root_utf8, environment.subject_id),
        log_(log_storage_, clock_),
        emergency_preflight_correlation_(log_.begin_correlation()),
        debug_mode_preferences_(std::move(debug_mode_preferences)),
        driver_handoff_platform_{},
        driver_network_{},
        driver_acquisition_(states_, hardware_overview_, driver_handoff_platform_,
                            driver_network_, log_, restart_resume_),
        emergency_notice_source_(),
        emergency_withdrawals_(
            states_, clock_, emergency_notice_source_,
            {.log = &log_, .correlation = emergency_preflight_correlation_}),
        occupancy_storage_(states_),
        occupancy_(occupancy_storage_, lease_tokens_),
        sogou_optimization_adapter_{},
        sogou_optimizations_(sogou_optimization_adapter_,
                             sogou_optimization_adapter_),
        platform_info_{},
        settings_catalog_file_(states_),
        settings_catalog_(settings_catalog_file_, settings_catalog_file_, log_,
                          occupancy_, settings_import_authorization_,
                          {.apply = {"windows.classic-context-menu.apply",
                                     "windows.windows10-explorer.apply"},
                           .detect = {"windows.classic-context-menu.detect",
                                      "windows.windows10-explorer.detect"},
                           .recover = {"windows.classic-context-menu.restore",
                                       "windows.windows10-explorer.restore"}}),
        system_settings_recovery_(states_),
        system_settings_apply_(settings_catalog_, system_settings_adapter_,
                               system_settings_recovery_, occupancy_, log_),
        operation_activity_(occupancy_),
        application_update_health_storage_(states_),
        application_update_platform_(platform_info_,
                                     application_update_health_storage_),
        application_updates_(application_update_platform_, operation_activity_,
                             log_, clock_),
        architecture_preferences_(std::move(architecture_preferences)),
        cache_retention_preferences_(std::move(cache_retention_preferences)),
        architecture_selection_(
            platform_info_, log_,
            architecture_preferences_
                ? architecture_preferences_->preference()
                : application::architecture_selection::selection_domain::
                      ArchitecturePreference::prefer_arm64_prompt_fallback),
        debug_mode_catalog_editor_(
            std::make_shared<application::DebugModeCatalogEditor>(
                log_, debug_mode_preferences_)),
        cache_root_{.kind = domain::offline_package_cache::
                            CacheLocationKind::system_directory,
                    .id = "program-data"},
        cache_storage_({adapters::infrastructure::
            ControlledPackageCacheRootConfiguration{
                .root = cache_root_,
                .directory = path_from_utf8(environment.root_utf8) /
                             "package-cache-v1",
                .create_if_missing = true}}),
        cache_downloader_{},
        live_offline_package_cache_(
            cache_storage_, cache_downloader_, network_, clock_, cache_root_,
            cache_retention_preferences_
                ? cache_retention_preferences_->retention()
                : domain::offline_package_cache::CacheRetentionPolicy::
                      retain_seven_days),
        batch_offline_package_cache_(
            cache_storage_, cache_downloader_, network_, clock_, cache_root_,
            cache_retention_preferences_
                ? cache_retention_preferences_->retention()
                : domain::offline_package_cache::CacheRetentionPolicy::
                      retain_seven_days),
        software_selection_(states_, clock_, log_, architecture_selection_,
                            source_resolver_, network_, presence_detector_,
                            external_launcher_, state_subject_),
        software_optimization_catalog_(
            states_, log_, occupancy_, optimization_catalog_file_,
            optimization_catalog_debug_authorization_,
            application::sogou_optimization::built_in_rule_definitions(),
            optimization_installer_baselines_),
        software_optimization_observer_(sogou_optimizations_),
        software_optimization_discovery_(software_optimization_catalog_,
                                         software_selection_,
                                         emergency_withdrawals_,
                                         software_optimization_observer_),
        software_optimization_batch_plans_(software_optimization_catalog_,
                                           software_optimization_discovery_),
        software_optimization_batch_executor_(sogou_optimizations_),
        software_optimization_batch_withdrawals_(emergency_withdrawals_),
        software_optimization_batches_(
            states_, occupancy_, log_, software_optimization_batch_plans_,
            software_optimization_batch_executor_,
            software_optimization_batch_withdrawals_, &restart_resume_),
        debug_log_policy_provider_(debug_mode_catalog_editor_),
        debug_log_policy_(debug_log_policy_provider_),
        software_optimization_catalog_update_source_(
            bundled_catalog_resources.software_optimization_catalog.bytes()),
        application_settings_(
            architecture_selection_, *architecture_preferences_,
            live_offline_package_cache_, batch_offline_package_cache_,
            *cache_retention_preferences_,
            history_and_logs_, system_settings_apply_, software_catalog_,
            settings_catalog_, software_optimization_catalog_,
            software_selection_, software_optimization_discovery_,
            &settings_catalog_update_source_,
            &software_optimization_catalog_update_source_,
            debug_mode_catalog_editor_.get()) {
    debug_mode_catalog_editor_->bind_catalog_lifecycle(software_catalog_);
    static_cast<void>(settings_catalog_.initialize_builtin(
        application::settings_catalog::initial_settings_catalog()));
    static_cast<void>(system_settings_apply_.refresh());
    static_cast<void>(architecture_selection_.start());
    static_cast<void>(software_catalog_.restore());
    static_cast<void>(software_selection_.restore());
    synchronize_catalog_selection_projection();
    synchronize_live_offline_package_cache();
    static_cast<void>(restart_resume_.restore());
    static_cast<void>(driver_acquisition_.restore());
    static_cast<void>(installation_batches_.restore());
    static_cast<void>(software_optimization_batches_.restore());
    static_cast<void>(guided_initialization_.restore());
    if (adapters::windows::is_restart_resume_login_launch()) {
      auto resumed = restart_resume_.resume_after_login();
      if (resumed.succeeded() && resumed.snapshot.checkpoint.has_value()) {
        bool read_only_verified = true;
        for (auto const& participant : resumed.snapshot.checkpoint->participants) {
          switch (participant.operation) {
            case application::restart_resume::RestartResumeOperation::installation_batch:
              read_only_verified =
                  installation_batches_.recover_read_only().succeeded() && read_only_verified;
              break;
            case application::restart_resume::RestartResumeOperation::software_optimization_batch:
              read_only_verified = software_optimization_batches_.recover_read_only().succeeded() &&
                                   read_only_verified;
              break;
            case application::restart_resume::RestartResumeOperation::driver_acquisition:
              read_only_verified = driver_acquisition_.recover_after_restart().succeeded() &&
                                   read_only_verified;
              break;
            case application::restart_resume::RestartResumeOperation::system_settings:
              // There is no system-settings read-only participant recovery yet.
              read_only_verified = false;
              break;
          }
        }
        if (read_only_verified) {
          static_cast<void>(restart_resume_.complete_read_only_verification());
        }
      }
    }
  }

  [[nodiscard]] startup::EmergencyPreflightStartResult
  start_emergency_preflight() {
    std::call_once(emergency_preflight_started_, [this] {
      auto self = shared_from_this();
      emergency_preflight_start_ = startup::start_emergency_preflight(
          [self = std::move(self)] {
            auto worker = std::thread{[self] {
              try {
                static_cast<void>(self->emergency_withdrawals_.preflight_check());
              } catch (...) {
                // Keep a future service regression from terminating this
                // detached thread while preserving the same typed failure.
                static_cast<void>(self->emergency_withdrawals_
                                      .report_preflight_execution_exception());
              }
            }};
            try {
              worker.detach();
            } catch (...) {
              // A failed detach leaves a joinable std::thread whose
              // destructor would terminate the process. Join it before
              // propagating the typed thread-start failure to the caller.
              if (worker.joinable()) {
                worker.join();
              }
              throw;
            }
          });
    });
    return emergency_preflight_start_;
  }

  [[nodiscard]] startup::StartupDiagnosticAvailability
  startup_diagnostic_availability() {
    return log_.snapshot().available
               ? startup::StartupDiagnosticAvailability::available
               : startup::StartupDiagnosticAvailability::unavailable;
  }

  [[nodiscard]] domain::StateSubject const& state_subject()
      const noexcept override {
    return state_subject_;
  }

  [[nodiscard]] application::DeviceStateStore& device_states()
      noexcept override {
    return states_;
  }

  [[nodiscard]] application::EmergencyWithdrawalService&
  emergency_withdrawals() noexcept override {
    return emergency_withdrawals_;
  }

  [[nodiscard]] application::ExecutionLog& execution_log()
      noexcept override {
    return log_;
  }

  [[nodiscard]] application::HistoryAndLogsService& history_and_logs()
      noexcept override {
    return history_and_logs_;
  }

  [[nodiscard]] application::ApplicationSettingsService& application_settings()
      noexcept override {
    return application_settings_;
  }

  [[nodiscard]] application::DebugModeCatalogEditor&
  debug_mode_catalog_editor() noexcept override {
    return *debug_mode_catalog_editor_;
  }

  [[nodiscard]] application::SharedOperationOccupancy& operation_occupancy()
      noexcept override {
    return occupancy_;
  }

  [[nodiscard]] application::sogou_optimization::SogouOptimizationService&
  sogou_optimizations() noexcept override {
    return sogou_optimizations_;
  }

  [[nodiscard]] application::software_optimization_discovery::
      SoftwareOptimizationDiscoveryService&
  software_optimization_discovery() noexcept override {
    return software_optimization_discovery_;
  }

  [[nodiscard]] application::SystemSettingsApplyService& system_settings_apply()
      noexcept override {
    return system_settings_apply_;
  }

  [[nodiscard]] std::shared_ptr<application::SystemSettingsApplyService>
  system_settings_apply_shared() {
    return std::shared_ptr<application::SystemSettingsApplyService>(
        shared_from_this(), &system_settings_apply_);
  }

  [[nodiscard]] application::ApplicationUpdateLifecycle& application_updates()
      noexcept override {
    return application_updates_;
  }

  [[nodiscard]] application::architecture_selection::
      ArchitectureSelectionLifecycle& architecture_selection()
      noexcept override {
    return architecture_selection_;
  }

  [[nodiscard]] application::software_selection::SoftwareSelectionLifecycle&
  software_selection() noexcept override {
    return software_selection_;
  }

  void shutdown() noexcept {
    try {
      std::call_once(shutdown_started_, [this] {
        // Persist the runner's close boundary before discarding its batch-owned
        // cache session. A later launch can then recover read-only rather than
        // treating a closing batch as safe to continue.
        try {
          static_cast<void>(software_optimization_batches_.request_close());
        } catch (...) {
          // The persisted batch state remains fail-closed for issue 12 recovery.
        }
        try {
          static_cast<void>(installation_batches_.request_close());
        } catch (...) {
          // Restore never auto-continues an active batch. If the best-effort
          // close receipt cannot be produced, the persisted state remains
          // fail-closed for the explicit recovery path on the next launch.
        }
        live_offline_package_cache_.shutdown();
        batch_offline_package_cache_.shutdown();
      });
    } catch (...) {
      // Keep the shutdown boundary noexcept; a later close or destruction can
      // retry if call_once cannot enter its callable.
    }
  }

  [[nodiscard]] application::installation_batch::InstallationBatchService&
  installation_batches() noexcept override {
    return installation_batches_;
  }

  [[nodiscard]] application::software_optimization_batch::
      SoftwareOptimizationBatchService&
  software_optimization_batches() noexcept override {
    return software_optimization_batches_;
  }

  [[nodiscard]] application::restart_resume::RestartResumeService&
  restart_resume() noexcept override {
    return restart_resume_;
  }

  [[nodiscard]] application::guided_initialization::GuidedInitializationService&
  guided_initialization() noexcept override {
    return guided_initialization_;
  }

  [[nodiscard]] application::HardwareOverviewService& hardware_overview()
      noexcept override {
    return hardware_overview_;
  }

  [[nodiscard]] application::driver_acquisition::DriverAcquisitionService&
  driver_acquisition() noexcept override {
    return driver_acquisition_;
  }

  [[nodiscard]] application::offline_package_cache::OfflinePackageCacheService&
  offline_package_cache() noexcept override {
    synchronize_live_offline_package_cache();
    return live_offline_package_cache_;
  }

  [[nodiscard]] adapters::windows::WindowsPlatformInfo const& platform_info()
      const noexcept {
    return platform_info_;
  }

 private:
  void synchronize_catalog_selection_projection() {
    auto const catalog = software_catalog_.snapshot();
    if (catalog.mode != application::software_catalog::CatalogLifecycleMode::ready ||
        !catalog.current.has_value() || !catalog.current_catalog.has_value()) {
      return;
    }
    static_cast<void>(software_selection_.on_catalog_replaced({
        .runtime = *catalog.current_catalog,
        .active = *catalog.current,
        .impact = {},
    }));
  }

  void synchronize_live_offline_package_cache() {
    auto const selection = software_selection_.snapshot();
    std::vector<application::offline_package_cache::CacheAsset> assets;
    for (auto const& source : selection.sources) {
      for (auto const& package : source.packages) {
        if (auto asset = application::offline_package_cache::make_cache_asset(
                source, package)) {
          assets.push_back(std::move(*asset));
        }
      }
    }
    live_offline_package_cache_.synchronize_assets(std::move(assets));
  }

  domain::StateSubject state_subject_;
  adapters::infrastructure::SystemClock clock_;
  adapters::windows::WindowsHardwareObserver hardware_observer_;
  application::HardwareOverviewService hardware_overview_;
  adapters::windows::WindowsStateFileSystem state_files_;
  application::DeviceStateStore states_;
  adapters::windows::WindowsLoginResumeRegistration
      restart_resume_registration_;
  application::restart_resume::RestartResumeService restart_resume_;
  adapters::infrastructure::LocalFileLogStorage log_storage_;
  adapters::infrastructure::StructuredExecutionLog log_;
  application::CorrelationId emergency_preflight_correlation_;
  std::shared_ptr<application::DebugModePreferenceStore>
      debug_mode_preferences_;
  adapters::windows::WindowsDriverHandoffPlatform driver_handoff_platform_;
  adapters::windows::WindowsDriverNetworkObserver driver_network_;
  application::driver_acquisition::DriverAcquisitionService driver_acquisition_;
  std::once_flag emergency_preflight_started_;
  std::once_flag shutdown_started_;
  startup::EmergencyPreflightStartResult emergency_preflight_start_;
  adapters::windows::WindowsEmergencyWithdrawalNoticeSource
      emergency_notice_source_;
  application::EmergencyWithdrawalService emergency_withdrawals_;
  adapters::infrastructure::StateOperationOccupancyStorage occupancy_storage_;
  adapters::windows::WindowsLeaseTokenSource lease_tokens_;
  application::SharedOperationOccupancy occupancy_;
  adapters::windows::WindowsSogouOptimizationAdapter
      sogou_optimization_adapter_;
  application::sogou_optimization::SogouOptimizationService
      sogou_optimizations_;
  adapters::infrastructure::SettingsCatalogFileAdapter settings_catalog_file_;
  ProductionSettingsCatalogImportAuthorization settings_import_authorization_;
  application::settings_catalog::SettingsCatalogLifecycle settings_catalog_;
  adapters::windows::WindowsSystemSettingsAdapter system_settings_adapter_;
  adapters::infrastructure::SystemSettingsRecoveryStore
      system_settings_recovery_;
  application::SystemSettingsApplyService system_settings_apply_;
  adapters::windows::WindowsPlatformInfo platform_info_;
  class OperationActivity final
      : public application::InitializationOperationActivity {
   public:
    explicit OperationActivity(application::SharedOperationOccupancy& occupancy)
        : occupancy_(occupancy) {}

    [[nodiscard]] application::InitializationOperationActivitySnapshot observe()
        override {
      auto occupied = occupancy_.inspect();
      if (occupied.code == application::OccupancyResultCode::observed) {
        if (!occupied.current.has_value()) {
          return {};
        }
        return {.phase = application::InitializationOperationPhase::active,
                .operation_id = occupied.current->identity.operation_id,
                .detail = "another workbench initialization operation is active"};
      }
      return {.phase = application::InitializationOperationPhase::
                         observation_unavailable,
              .detail = occupied.detail.empty()
                            ? "initialization operation occupancy is unavailable"
                            : std::move(occupied.detail)};
    }

   private:
    application::SharedOperationOccupancy& occupancy_;
  } operation_activity_;
  adapters::infrastructure::StateApplicationUpdateHealthStorage
      application_update_health_storage_;
  adapters::windows::WindowsApplicationUpdatePlatform
      application_update_platform_;
  application::ApplicationUpdateLifecycle application_updates_;
  std::shared_ptr<application::ArchitecturePreferences>
      architecture_preferences_;
  std::shared_ptr<application::CacheRetentionPreferences>
      cache_retention_preferences_;
  application::architecture_selection::ArchitectureSelectionLifecycle
      architecture_selection_;
  std::shared_ptr<application::DebugModeCatalogEditor>
      debug_mode_catalog_editor_;
  adapters::infrastructure::LocalSoftwareCatalogFileReader software_catalog_file_{
      adapters::infrastructure::LocalSoftwareCatalogFileReader::
          from_verified_built_in(
              bundled_catalog_resources.software_catalog.bytes())};
  adapters::infrastructure::TomlSoftwareCatalogCodec software_catalog_codec_;
  application::software_catalog::SoftwareCatalogLifecycle software_catalog_{
      states_, log_, occupancy_, software_catalog_file_, software_catalog_codec_,
      domain::software_catalog::initial_software_catalog_policy(),
      *debug_mode_catalog_editor_, state_subject_};
  UnavailableControlledSourceResolver source_resolver_;
  OfflineNetworkObserver network_;
  application::offline_package_cache::ControlledCacheRoot cache_root_;
  adapters::infrastructure::LocalPackageCacheStorage cache_storage_;
  adapters::infrastructure::UnavailableControlledPackageDownloader
      cache_downloader_;
  application::offline_package_cache::OfflinePackageCacheService
      live_offline_package_cache_;
  application::offline_package_cache::OfflinePackageCacheService
      batch_offline_package_cache_;
  UnavailableSoftwarePresenceDetector presence_detector_;
  adapters::windows::WindowsExternalAddressLauncher external_launcher_;
  application::software_selection::SoftwareSelectionLifecycle
      software_selection_;
  adapters::infrastructure::LocalSoftwareOptimizationCatalogFile
      optimization_catalog_file_;
  ProductionSoftwareOptimizationCatalogDebugAuthorization
      optimization_catalog_debug_authorization_;
  std::vector<domain::software_optimization_catalog::SoftwareCatalogInstallerBaseline>
      optimization_installer_baselines_{{
          .software_item_id = {"sogou-input"},
          .installer_baseline_id = {"sogou-input-windows-16.7"},
          .installed_versions = {"16.7", "16.7"},
      }};
  application::SoftwareOptimizationCatalogLifecycle
      software_optimization_catalog_;
  application::software_optimization_discovery::SogouOptimizationDiscoveryObserver
      software_optimization_observer_;
  application::software_optimization_discovery::SoftwareOptimizationDiscoveryService
      software_optimization_discovery_;
  application::software_optimization_batch::DiscoveryOptimizationBatchPlanSource
      software_optimization_batch_plans_;
  application::software_optimization_batch::SogouOptimizationBatchExecutor
      software_optimization_batch_executor_;
  application::software_optimization_batch::
      EmergencyWithdrawalOptimizationAuthorization
          software_optimization_batch_withdrawals_;
  application::software_optimization_batch::SoftwareOptimizationBatchService
      software_optimization_batches_;
  adapters::windows::WindowsInstallationDownloadAdapter batch_download_{
      batch_offline_package_cache_};
  adapters::windows::WindowsControlledProfileReadinessAdapter batch_readiness_;
  adapters::windows::WindowsOpaqueCacheInstallerLauncher batch_executor_{
      batch_offline_package_cache_};
  adapters::windows::WindowsInstallationResultVerifier batch_verifier_;
  adapters::windows::WindowsInstallationFactSink batch_facts_{log_};
  application::installation_batch::InstallationBatchService installation_batches_{
      states_, occupancy_, log_, batch_download_, batch_executor_, batch_readiness_,
      batch_verifier_, batch_facts_, software_catalog_, software_selection_,
      &restart_resume_};
  std::shared_ptr<application::DebugLogPolicyProvider const>
      debug_log_policy_provider_;
  application::DebugLogPolicyReader debug_log_policy_;
  application::HistoryAndLogsService history_and_logs_{
      clock_, application_updates_, platform_info_, hardware_overview_,
      software_catalog_, log_, installation_batches_,
      software_optimization_batches_, system_settings_apply_,
      software_selection_, &debug_log_policy_, &restart_resume_};
  EmbeddedSettingsCatalogUpdateSource settings_catalog_update_source_;
  EmbeddedSoftwareOptimizationCatalogUpdateSource
      software_optimization_catalog_update_source_;
  application::ApplicationSettingsService application_settings_;
  WindowsGuidedInitializationEvidenceSource guided_evidence_source_{
      driver_acquisition_, system_settings_apply_, installation_batches_,
      software_optimization_discovery_, software_optimization_batches_,
      software_selection_, software_catalog_, software_optimization_catalog_,
      restart_resume_};
  application::guided_initialization::GuidedInitializationService
      guided_initialization_{states_, clock_, guided_evidence_source_};
};

class StartupServicesShutdownGuard final {
 public:
  explicit StartupServicesShutdownGuard(
      std::shared_ptr<WindowsWorkbenchServices> services) noexcept
      : services_(std::move(services)) {}

  ~StartupServicesShutdownGuard() {
    shutdown_now();
  }

  void shutdown_now() noexcept {
    if (auto services = std::move(services_)) {
      services->shutdown();
    }
  }

  void release() noexcept {
    services_.reset();
  }

 private:
  std::shared_ptr<WindowsWorkbenchServices> services_;
};

[[nodiscard]] winrt::Microsoft::UI::Xaml::Window
create_static_startup_failure_window(
    startup::StartupAssemblyFailure const& failure) {
  using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
  using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
  using winrt::Microsoft::UI::Xaml::TextWrapping;

  auto window = winrt::Microsoft::UI::Xaml::Window{};
  window.Title(L"无法进入工作台");
  auto content = StackPanel{};
  content.Spacing(12);
  auto title = TextBlock{};
  title.Text(L"无法进入工作台");
  title.FontSize(22);
  auto message = TextBlock{};
  message.Text(winrt::hstring{failure.public_statement});
  message.TextWrapping(TextWrapping::WrapWholeWords);
  content.Children().Append(title);
  content.Children().Append(message);
  window.Content(content);
  window.Activate();
  return window;
}

[[nodiscard]] StartupAssemblyResult startup_failure(
    startup::StartupAssemblyStatus status,
    std::optional<adapters::windows::DeviceDataEnvironmentResult>
        device_data_environment_failure = std::nullopt) {
  winrt::Microsoft::UI::Xaml::Window failure_window{nullptr};
  try {
    failure_window = create_static_startup_failure_window(*status.failure);
  } catch (...) {
    // Preserve the typed status for the UI entry point. If the XAML failure
    // presenter itself is unavailable, App.xaml.cpp supplies a platform-level
    // last-resort error surface.
  }
  return {.window = std::move(failure_window),
          .status = std::move(status),
          .device_data_environment_failure =
              std::move(device_data_environment_failure)};
}

}  // namespace

StartupAssemblyResult assemble_startup() {
  auto diagnostic_availability = startup::StartupDiagnosticAvailability::unavailable;
  std::optional<StartupServicesShutdownGuard> services_shutdown;
  try {
    auto bundled_catalog_resources = bundled_catalog_paths();
    if (!bundled_catalog_resources.has_value()) {
      return startup_failure(startup::startup_assembly_failed(
          startup::StartupAssemblyStage::bundled_catalog_resources));
    }
    auto environment =
        adapters::windows::WindowsDeviceDataEnvironment::prepare();
    if (!environment) {
      auto failure = std::move(environment);
      auto const raw_error_code = failure.raw_error;
      return startup_failure(
          startup::startup_assembly_failed(
              startup::StartupAssemblyStage::device_data_environment,
              startup::StartupDiagnosticAvailability::unavailable,
              raw_error_code),
          std::move(failure));
    }
    {
      StartupOptimizationCatalogGate catalog_gate{*environment.environment};
      if (!catalog_gate.ensure_bundled_catalog(*bundled_catalog_resources)) {
        return startup_failure(startup::startup_assembly_failed(
            startup::StartupAssemblyStage::bundled_catalog_resources));
      }
    }
    auto view_preferences =
        std::make_shared<adapters::windows::WindowsViewPreferences>();
    auto advanced_view_preferences =
        std::make_shared<application::AdvancedViewPreferences>(view_preferences);
    auto architecture_preferences =
        std::make_shared<application::ArchitecturePreferences>(view_preferences);
    auto cache_retention_preferences =
        std::make_shared<application::CacheRetentionPreferences>(view_preferences);
    auto debug_mode_preferences =
        std::shared_ptr<application::DebugModePreferenceStore>(
            view_preferences,
            static_cast<application::DebugModePreferenceStore*>(
                view_preferences.get()));
    auto services = std::make_shared<WindowsWorkbenchServices>(
        std::move(*bundled_catalog_resources),
        std::move(*environment.environment), std::move(architecture_preferences),
        std::move(cache_retention_preferences), std::move(debug_mode_preferences));
    services_shutdown.emplace(services);
    auto workbench = std::make_shared<application::Workbench>(
        services->platform_info(), services);
    auto system_settings = services->system_settings_apply_shared();
    diagnostic_availability = services->startup_diagnostic_availability();
    if (diagnostic_availability !=
        startup::StartupDiagnosticAvailability::available) {
      services_shutdown->shutdown_now();
      return startup_failure(startup::startup_assembly_failed(
          startup::StartupAssemblyStage::core_records_unreadable,
          startup::StartupDiagnosticAvailability::unavailable));
    }
    startup::EmergencyPreflightStartResult emergency_preflight;

    std::shared_ptr<ui::winui::MotionPreferences> motion_preferences;
    try {
      motion_preferences = ui::winui::MotionPreferences::create();
    } catch (winrt::hresult_error const&) {
      motion_preferences = ui::winui::MotionPreferences::create_static();
    }

    winrt::com_ptr<winrt::Azzs::Ui::implementation::MainWindow> window;
    winrt::Microsoft::UI::Xaml::Window result{nullptr};
    try {
      window = winrt::make_self<winrt::Azzs::Ui::implementation::MainWindow>();
      result = window.as<winrt::Microsoft::UI::Xaml::Window>();
    } catch (winrt::hresult_error const&) {
      services_shutdown->shutdown_now();
      return startup_failure(startup::startup_assembly_failed(
          startup::StartupAssemblyStage::main_window_initialization,
          diagnostic_availability));
    }

    try {
      window->bind(std::move(workbench), std::move(motion_preferences),
                   std::move(system_settings),
                   std::move(advanced_view_preferences));
      result.Closed([services](auto&&, auto&&) {
        services->shutdown();
      });
    } catch (winrt::hresult_error const&) {
      services_shutdown->shutdown_now();
      return startup_failure(startup::startup_assembly_failed(
          startup::StartupAssemblyStage::main_window_binding,
          diagnostic_availability));
    }

    try {
      window->show_initial_page();
    } catch (winrt::hresult_error const&) {
      services_shutdown->shutdown_now();
      return startup_failure(startup::startup_assembly_failed(
          startup::StartupAssemblyStage::main_window_navigation,
          diagnostic_availability));
    }

    try {
      result.Activate();
    } catch (winrt::hresult_error const&) {
      services_shutdown->shutdown_now();
      return startup_failure(startup::startup_assembly_failed(
          startup::StartupAssemblyStage::main_window_activation,
          diagnostic_availability));
    }

    window->confirm_started_healthy();
    emergency_preflight = services->start_emergency_preflight();
    services_shutdown->release();
    return {.window = std::move(result),
            .status = startup::startup_assembly_ready(emergency_preflight)};
  } catch (...) {
    if (services_shutdown.has_value()) {
      services_shutdown->shutdown_now();
    }
    return startup_failure(startup::startup_assembly_failed(
        startup::StartupAssemblyStage::unexpected_assembly_exception,
        diagnostic_availability));
  }
}

}  // namespace azzs::composition::windows
