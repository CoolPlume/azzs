#include "azzs/application/workbench.hpp"

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
  };
  if (services_) {
    snapshot_.update = services_->application_updates().snapshot();
  } else {
    snapshot_.update.current = ApplicationBuildIdentity{
        .version = "0.1.0",
        .channel = ApplicationReleaseChannel::stable,
        .architecture = platform_info.windows_architecture(),
        .edition = ApplicationReleaseEdition::standard,
        .form = ApplicationReleaseForm::portable,
    };
    snapshot_.update.detail =
        "application update service is not available in this host";
  }
}

void Workbench::navigate(PageId page) noexcept {
  snapshot_.current_page = page;
}

UpdateCommandResult Workbench::handle_update(UpdateUserIntent intent) {
  if (!services_) {
    snapshot_.update.detail =
        "application update service is not available in this host";
    return {.code = UpdateCommandCode::rejected,
            .snapshot = snapshot_.update,
            .detail = snapshot_.update.detail};
  }
  auto result = services_->application_updates().handle(intent);
  snapshot_.update = result.snapshot;
  return result;
}

WorkbenchSnapshot Workbench::snapshot() const noexcept {
  return snapshot_;
}

}  // namespace azzs::application
