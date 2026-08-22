#pragma once

#include <optional>

#include "azzs/domain/system_version.hpp"

namespace azzs::domain {

enum class MinimumVersionRisk {
  none,
  earlier_than_target,
  version_unavailable,
};

class MinimumVersionPolicy final {
 public:
  explicit constexpr MinimumVersionPolicy(SystemVersion target) noexcept
      : target_(target) {}

  [[nodiscard]] MinimumVersionRisk assess(
      std::optional<SystemVersion> observed) const noexcept;
  [[nodiscard]] constexpr SystemVersion target() const noexcept {
    return target_;
  }

 private:
  SystemVersion target_;
};

}  // namespace azzs::domain
