#include "pch.h"

#include "composition_root.hpp"

#include <memory>
#include <mutex>
#include <filesystem>
#include <thread>
#include <stdexcept>
#include <string>
#include <string_view>
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
#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/advanced_view_preferences.hpp"
#include "azzs/application/application_update.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/driver_acquisition.hpp"
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

namespace azzs::composition::windows {
namespace {

class ProductionSettingsCatalogImportAuthorization final
    : public application::settings_catalog::SettingsCatalogImportAuthorization {
 public:
  [[nodiscard]] bool debug_import_allowed() const noexcept override {
    return false;
  }
};

class ProductionCatalogMaintenanceAccess final
    : public application::software_catalog::CatalogMaintenanceAccess {
 public:
  [[nodiscard]] application::software_catalog::CatalogEditorAccess
  editor_access() const noexcept override {
    return application::software_catalog::CatalogEditorAccess::unavailable;
  }
};

class ProductionSoftwareOptimizationCatalogDebugAuthorization final
    : public application::SoftwareOptimizationCatalogDebugAuthorization {
 public:
  [[nodiscard]] bool local_import_allowed() const noexcept override {
    return false;
  }
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

[[nodiscard]] std::string utf8_from_path(std::filesystem::path const& value) {
  auto const native = value.generic_u8string();
  return {reinterpret_cast<char const*>(native.data()), native.size()};
}

[[nodiscard]] std::filesystem::path repository_catalog_path() {
  // The repository catalog is an explicitly embedded, deployment-controlled
  // input for this desktop host. Its path does not cross any application seam.
  auto const source = std::filesystem::path{__FILE__};
  return source.parent_path().parent_path().parent_path().parent_path() /
         "catalog" / "software-catalog.toml";
}

[[nodiscard]] std::filesystem::path repository_optimization_catalog_path() {
  auto const source = std::filesystem::path{__FILE__};
  return source.parent_path().parent_path().parent_path().parent_path() /
         "catalog" / "software-optimization-catalog.toml";
}

class UnavailableSoftwarePresenceDetector final
    : public application::software_selection::SoftwarePresenceDetector {
 public:
  [[nodiscard]] application::software_selection::PresenceDetection detect(
      std::string_view) override {
    return {.detail = "software presence detection is not configured"};
  }
};

class WindowsWorkbenchServices final
    : public application::WorkbenchServices,
      public std::enable_shared_from_this<WindowsWorkbenchServices> {
 public:
  explicit WindowsWorkbenchServices(
      adapters::windows::DeviceDataEnvironment environment)
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
        driver_handoff_platform_{},
        driver_network_{},
        driver_acquisition_(states_, hardware_overview_, driver_handoff_platform_,
                            driver_network_, log_, restart_resume_),
        emergency_notice_source_(),
        emergency_withdrawals_(
            states_, clock_, emergency_notice_source_,
            {.log = &log_, .correlation = log_.begin_correlation()}),
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
        architecture_selection_(platform_info_, log_,
                                application::architecture_selection::
                                    selection_domain::ArchitecturePreference::
                                        prefer_arm64_prompt_fallback),
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
        live_offline_package_cache_(cache_storage_, cache_downloader_, network_,
                                    clock_, cache_root_),
        batch_offline_package_cache_(cache_storage_, cache_downloader_, network_,
                                     clock_, cache_root_),
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
            software_optimization_batch_withdrawals_, &restart_resume_) {
    static_cast<void>(settings_catalog_.initialize_builtin(
        application::settings_catalog::initial_settings_catalog()));
    static_cast<void>(system_settings_apply_.refresh());
    static_cast<void>(architecture_selection_.start());
    static_cast<void>(software_catalog_.restore());
    static_cast<void>(software_selection_.restore());
    synchronize_catalog_selection_projection();
    auto const optimization_catalog = optimization_catalog_file_.read(
        utf8_from_path(repository_optimization_catalog_path()));
    if (optimization_catalog.succeeded) {
      static_cast<void>(software_optimization_catalog_.ensure_builtin(
          optimization_catalog.source, "embedded-software-optimization-catalog"));
    }
    synchronize_live_offline_package_cache();
    static_cast<void>(restart_resume_.restore());
    static_cast<void>(driver_acquisition_.restore());
    static_cast<void>(installation_batches_.restore());
    static_cast<void>(software_optimization_batches_.restore());
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

  void start_emergency_preflight() {
    std::call_once(emergency_preflight_started_, [this] {
      auto self = shared_from_this();
      std::thread([self = std::move(self)] {
        try {
          (void)self->emergency_withdrawals_.preflight_check();
        } catch (...) {
          // The service owns the typed failure path; the adapter boundary must
          // never terminate the workbench because a startup check threw.
        }
      }).detach();
    });
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
  adapters::windows::WindowsDriverHandoffPlatform driver_handoff_platform_;
  adapters::windows::WindowsDriverNetworkObserver driver_network_;
  application::driver_acquisition::DriverAcquisitionService driver_acquisition_;
  std::once_flag emergency_preflight_started_;
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
  application::architecture_selection::ArchitectureSelectionLifecycle
      architecture_selection_;
  adapters::infrastructure::LocalSoftwareCatalogFileReader software_catalog_file_{
      utf8_from_path(repository_catalog_path())};
  adapters::infrastructure::TomlSoftwareCatalogCodec software_catalog_codec_;
  ProductionCatalogMaintenanceAccess software_catalog_maintenance_access_;
  application::software_catalog::SoftwareCatalogLifecycle software_catalog_{
      states_, log_, occupancy_, software_catalog_file_, software_catalog_codec_,
      domain::software_catalog::initial_software_catalog_policy(),
      software_catalog_maintenance_access_, state_subject_};
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
  application::HistoryAndLogsService history_and_logs_{
      clock_, application_updates_, platform_info_, hardware_overview_,
      software_catalog_, log_, installation_batches_,
      software_optimization_batches_, system_settings_apply_,
      software_selection_};
};

}  // namespace

winrt::Microsoft::UI::Xaml::Window create_main_window() {
  auto environment =
      adapters::windows::WindowsDeviceDataEnvironment::prepare();
  if (!environment) {
    throw std::runtime_error(
        "device data environment preparation failed (raw=" +
        std::to_string(environment.raw_error) + "): " + environment.detail);
  }
  auto services = std::make_shared<WindowsWorkbenchServices>(
      std::move(*environment.environment));
  auto workbench = std::make_shared<application::Workbench>(
      services->platform_info(), services);
  auto system_settings = services->system_settings_apply_shared();
  services->start_emergency_preflight();
  auto motion_preferences = ui::winui::MotionPreferences::create();
  auto window = winrt::make_self<winrt::Azzs::Ui::implementation::MainWindow>();
  auto view_preferences =
      std::make_shared<adapters::windows::WindowsViewPreferences>();
  auto advanced_view_preferences =
      std::make_shared<application::AdvancedViewPreferences>(
          std::move(view_preferences));
  window->bind(std::move(workbench), std::move(motion_preferences),
               std::move(system_settings),
               std::move(advanced_view_preferences));
  auto result = window.as<winrt::Microsoft::UI::Xaml::Window>();
  result.Closed([services](auto&&, auto&&) {
    services->shutdown();
  });
  return result;
}

}  // namespace azzs::composition::windows
