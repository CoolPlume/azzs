#pragma once

#include <optional>

#include "azzs/application/platform_info.hpp"
#include "azzs/domain/system_version.hpp"

namespace azzs::adapters::windows {

class WindowsPlatformInfo final : public application::PlatformInfo {
 public:
  [[nodiscard]] std::optional<domain::SystemVersion> windows_version()
      const override;
};

}  // namespace azzs::adapters::windows
