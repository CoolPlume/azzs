#pragma once

#include <optional>

#include "azzs/application/sogou_optimization.hpp"

namespace azzs::adapters::windows {

// The 16.7 UI Automation identity profile has not yet been captured and
// verified on a real Windows device.  This adapter therefore fails closed;
// it contains no fallback to arbitrary text, paths, registry keys, commands,
// or selectors.
class WindowsSogouOptimizationAdapter final
    : public application::sogou_optimization::SogouOptimizationPlatformAdapter {
 public:
  [[nodiscard]] application::sogou_optimization::SogouTargetDetection
  detect_target() override;

  [[nodiscard]] application::sogou_optimization::SogouOptionDetection
  detect_option(
      application::sogou_optimization::SogouOptimizationAction action,
      std::optional<application::sogou_optimization::SogouCandidateCount>
          expected_value) override;

  [[nodiscard]] application::sogou_optimization::SogouOptimizationExecution
  execute(
      application::sogou_optimization::SogouOptimizationAction action,
      std::optional<application::sogou_optimization::SogouCandidateCount>
          value) override;
};

}  // namespace azzs::adapters::windows
