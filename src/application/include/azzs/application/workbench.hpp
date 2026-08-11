#pragma once

#include <optional>

#include "azzs/application/page_id.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/domain/minimum_version_policy.hpp"
#include "azzs/domain/system_version.hpp"

namespace azzs::application {

struct WorkbenchSnapshot final {
  PageId current_page{PageId::overview};
  domain::MinimumVersionRisk minimum_version_risk{
      domain::MinimumVersionRisk::version_unavailable};
  std::optional<domain::SystemVersion> observed_windows_version;
  domain::SystemVersion target_windows_version;
};

class Workbench final {
 public:
  static constexpr domain::SystemVersion kWindows10Version22H2{10, 0, 19045};

  explicit Workbench(PlatformInfo const& platform_info);

  void navigate(PageId page) noexcept;
  [[nodiscard]] WorkbenchSnapshot snapshot() const noexcept;

 private:
  WorkbenchSnapshot snapshot_;
};

}  // namespace azzs::application
