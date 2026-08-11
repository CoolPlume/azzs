#include "azzs/domain/minimum_version_policy.hpp"

namespace azzs::domain {

MinimumVersionRisk MinimumVersionPolicy::assess(
    std::optional<SystemVersion> observed) const noexcept {
  if (!observed.has_value()) {
    return MinimumVersionRisk::version_unavailable;
  }

  if (*observed < target_) {
    return MinimumVersionRisk::earlier_than_target;
  }

  return MinimumVersionRisk::none;
}

}  // namespace azzs::domain
