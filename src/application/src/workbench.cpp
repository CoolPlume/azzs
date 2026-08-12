#include "azzs/application/workbench.hpp"

#include <stop_token>
#include <utility>

#include "azzs/application/workbench_services.hpp"

namespace azzs::application {

Workbench::Workbench(PlatformInfo const& platform_info)
    : Workbench(platform_info, {}) {}

Workbench::Workbench(PlatformInfo const& platform_info,
                     std::shared_ptr<WorkbenchServices> services)
    : services_(std::move(services)) {
  domain::MinimumVersionPolicy const policy{kWindows10Version22H2};
  auto const observed_version = platform_info.windows_version();

  snapshot_ = WorkbenchSnapshot{
      .current_page = PageId::overview,
      .minimum_version_risk = policy.assess(observed_version),
      .observed_windows_version = observed_version,
      .target_windows_version = policy.target(),
      .system_settings =
          services_ ? services_->system_settings_apply().snapshot()
                    : SystemSettingsApplySnapshot{},
  };
}

void Workbench::navigate(PageId page) noexcept {
  snapshot_.current_page = page;
}

HardwareOverviewSnapshot Workbench::observe_hardware(
    HardwareOverviewTrigger trigger, std::stop_token cancellation) {
  if (!services_) {
    snapshot_.hardware_overview = HardwareOverviewSnapshot{
        .state = HardwareOverviewState::unrecognized,
        .error = "hardware overview service is unavailable",
        .stale = false,
    };
    return snapshot_.hardware_overview;
  }
  snapshot_.hardware_overview =
      services_->hardware_overview().observe(trigger, cancellation);
  return snapshot_.hardware_overview;
}

HardwareOverviewSnapshot Workbench::refresh_hardware(
    std::stop_token cancellation) {
  return observe_hardware(HardwareOverviewTrigger::user_refresh,
                          cancellation);
}

WorkbenchSnapshot Workbench::snapshot() const {
  return snapshot_;
}

std::shared_ptr<WorkbenchServices> Workbench::services() const noexcept {
  return services_;
}

}  // namespace azzs::application
