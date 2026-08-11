#pragma once

#include <optional>

#include "azzs/application/platform_info.hpp"
#include "azzs/domain/system_version.hpp"

namespace azzs::testing {

class FixedPlatformInfo final : public application::PlatformInfo {
 public:
  explicit FixedPlatformInfo(
      std::optional<domain::SystemVersion> version) noexcept
      : version_(version) {}

  [[nodiscard]] std::optional<domain::SystemVersion> windows_version()
      const override {
    return version_;
  }

 private:
  std::optional<domain::SystemVersion> version_;
};

}  // namespace azzs::testing
