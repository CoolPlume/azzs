#include "azzs/application/workbench.hpp"

#include <utility>

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
  };
}

void Workbench::navigate(PageId page) noexcept {
  snapshot_.current_page = page;
}

WorkbenchSnapshot Workbench::snapshot() const noexcept {
  return snapshot_;
}

std::shared_ptr<WorkbenchServices> Workbench::services() const noexcept {
  return services_;
}

}  // namespace azzs::application
