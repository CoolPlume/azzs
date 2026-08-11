#pragma once

#include <string_view>

#include "azzs/application/software_optimization_catalog_lifecycle.hpp"

namespace azzs::adapters::infrastructure {

class LocalSoftwareOptimizationCatalogFile final
    : public application::SoftwareOptimizationCatalogFile {
 public:
  [[nodiscard]] application::SoftwareOptimizationCatalogFileRead read(
      std::string_view path) override;
};

}  // namespace azzs::adapters::infrastructure
