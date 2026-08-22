#pragma once

#include <string_view>

#include "azzs/application/software_optimization_catalog_lifecycle.hpp"

namespace azzs::adapters::infrastructure {

class LocalSoftwareOptimizationCatalogFile final
    : public application::SoftwareOptimizationCatalogLocalImportFile {
 public:
  [[nodiscard]] application::SoftwareOptimizationCatalogLocalImportRead read(
      std::string_view path) override;
};

}  // namespace azzs::adapters::infrastructure
