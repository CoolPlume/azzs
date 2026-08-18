#pragma once

#include <optional>

#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/application/startup_assembly_status.hpp"

namespace azzs::application::startup {

[[nodiscard]] inline bool catalog_gate_lifecycle_failure(
    SoftwareOptimizationCatalogLifecycleResult const& result) noexcept {
  const auto accepted_code =
      result.code == SoftwareOptimizationCatalogLifecycleCode::applied ||
      result.code == SoftwareOptimizationCatalogLifecycleCode::unchanged;
  return !accepted_code || !result.error.empty() ||
         !result.logging_error.empty() || !result.occupancy_error.empty();
}

[[nodiscard]] inline std::optional<StartupAssemblyStage>
startup_catalog_gate_failure_stage(
    SoftwareOptimizationCatalogLifecycleResult const& result) noexcept {
  if (!catalog_gate_lifecycle_failure(result)) {
    return std::nullopt;
  }
  return StartupAssemblyStage::software_optimization_catalog_lifecycle;
}

}  // namespace azzs::application::startup
