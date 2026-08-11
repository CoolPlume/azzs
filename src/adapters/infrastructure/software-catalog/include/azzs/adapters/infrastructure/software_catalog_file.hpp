#pragma once

#include <string>
#include <string_view>

#include "azzs/application/software_catalog_lifecycle.hpp"

namespace azzs::adapters::infrastructure {

class LocalSoftwareCatalogFileReader final
    : public application::software_catalog::SoftwareCatalogFileReader {
 public:
  LocalSoftwareCatalogFileReader(std::string built_in_path = {},
                                 std::string update_path = {});

  [[nodiscard]] application::software_catalog::CatalogFileRead read_built_in()
      const override;
  [[nodiscard]] application::software_catalog::CatalogFileRead read_update()
      const override;
  [[nodiscard]] application::software_catalog::CatalogFileRead
  read_manual_import(std::string const& path) const override;

  // This adapter-level operation is used by offline catalog verification and
  // does not assign an origin. Lifecycle use cases select an origin only via
  // the trusted methods above.
  [[nodiscard]] application::software_catalog::CatalogFileRead read(
      std::string const& path) const;

 private:
  std::string built_in_path_;
  std::string update_path_;
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
