#pragma once

#include <memory>
#include <optional>
#include <stop_token>

#include "azzs/application/application_update.hpp"
#include "azzs/application/page_id.hpp"
#include "azzs/application/hardware_overview.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/domain/minimum_version_policy.hpp"
#include "azzs/domain/system_version.hpp"

namespace azzs::application {

class WorkbenchServices;

struct WorkbenchSnapshot final {
  PageId current_page{PageId::overview};
  domain::MinimumVersionRisk minimum_version_risk{
      domain::MinimumVersionRisk::version_unavailable};
  std::optional<domain::SystemVersion> observed_windows_version;
  domain::SystemVersion target_windows_version;
  UpdateSnapshot update;
  HardwareOverviewSnapshot hardware_overview;
};

class Workbench final {
 public:
  static constexpr domain::SystemVersion kWindows10Version22H2{10, 0, 19045};

  explicit Workbench(PlatformInfo const& platform_info);
  Workbench(PlatformInfo const& platform_info,
            std::shared_ptr<WorkbenchServices> services);

  void navigate(PageId page) noexcept;
  [[nodiscard]] UpdateCommandResult handle_update(UpdateUserIntent intent);
  [[nodiscard]] HardwareOverviewSnapshot observe_hardware(
      HardwareOverviewTrigger trigger,
      std::stop_token cancellation = {});
  [[nodiscard]] HardwareOverviewSnapshot refresh_hardware(
      std::stop_token cancellation = {});
  [[nodiscard]] WorkbenchSnapshot snapshot() const;
  [[nodiscard]] std::shared_ptr<WorkbenchServices> services() const noexcept;

 private:
  WorkbenchSnapshot snapshot_;
  std::shared_ptr<WorkbenchServices> services_;
};

}  // namespace azzs::application
