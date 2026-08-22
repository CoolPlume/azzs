#include "azzs/adapters/windows/windows_sogou_optimization_adapter.hpp"

#include <string>
#include <string_view>

namespace azzs::adapters::windows {
namespace {

constexpr std::string_view kPendingProfile{
    "Sogou 16.7 UI Automation identity profile is not verified on a real Windows device"};

}  // namespace

application::sogou_optimization::SogouTargetDetection
WindowsSogouOptimizationAdapter::detect_target() {
  return {.status = application::sogou_optimization::SogouOptimizationStatus::
              pending_confirmation,
          .detail = std::string{kPendingProfile}};
}

application::sogou_optimization::SogouOptionDetection
WindowsSogouOptimizationAdapter::detect_option(
    application::sogou_optimization::SogouOptimizationAction,
    std::optional<application::sogou_optimization::SogouCandidateCount>) {
  return {.status = application::sogou_optimization::SogouOptimizationStatus::
              pending_confirmation,
          .detail = std::string{kPendingProfile}};
}

application::sogou_optimization::SogouOptimizationExecution
WindowsSogouOptimizationAdapter::execute(
    application::sogou_optimization::SogouOptimizationAction,
    std::optional<application::sogou_optimization::SogouCandidateCount>) {
  return {.status = application::sogou_optimization::SogouOptimizationStatus::
              pending_confirmation,
          .detail = std::string{kPendingProfile}};
}

}  // namespace azzs::adapters::windows
