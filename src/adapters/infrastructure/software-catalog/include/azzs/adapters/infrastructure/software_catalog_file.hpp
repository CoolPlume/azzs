#pragma once

#include <string>
#include <string_view>

#include "azzs/application/software_catalog_lifecycle.hpp"

namespace azzs::adapters::infrastructure {

class LocalSoftwareCatalogFileReader final
    : public application::software_catalog::SoftwareCatalogFileReader {
 public:
  [[nodiscard]] application::software_catalog::CatalogFileRead read(
      std::string const& path) const override;
};

class TomlSoftwareCatalogCodec final
    : public application::software_catalog::SoftwareCatalogCodec {
 public:
  [[nodiscard]] application::software_catalog::CatalogDecodeResult decode(
      std::string_view bytes) const override;
  [[nodiscard]] std::string encode(
      domain::software_catalog::SoftwareCatalogDocument const& document) const
      override;
};

}  // namespace azzs::adapters::infrastructure
