#pragma once

#include <optional>

#include "azzs/application/platform_info.hpp"
#include "azzs/domain/system_version.hpp"

namespace azzs::testing {

class FixedPlatformInfo final : public application::PlatformInfo {
 public:
  explicit FixedPlatformInfo(
      std::optional<domain::SystemVersion> version,
      domain::SystemArchitecture architecture =
          domain::SystemArchitecture::unknown) noexcept
      : version_(version), architecture_(architecture) {}

  [[nodiscard]] std::optional<domain::SystemVersion> windows_version()
      const override {
    return version_;
  }

  [[nodiscard]] domain::SystemArchitecture windows_architecture()
      const override {
    return architecture_;
  }

 private:
  std::optional<domain::SystemVersion> version_;
  domain::SystemArchitecture architecture_;
};

}  // namespace azzs::testing
