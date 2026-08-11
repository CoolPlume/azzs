#include "azzs/adapters/infrastructure/software_catalog_file.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace azzs::adapters::infrastructure {

application::software_catalog::CatalogFileRead
LocalSoftwareCatalogFileReader::read(std::string const& path) const {
  constexpr std::uintmax_t k_max_catalog_size = 16U * 1024U * 1024U;
  application::software_catalog::CatalogFileRead result{.path = path};
  std::error_code error;
  auto const size = std::filesystem::file_size(path, error);
  if (error) {
    result.error = "catalog file size could not be read: " + error.message();
    return result;
  }
  if (size > k_max_catalog_size) {
    result.error = "catalog file exceeds the supported size";
    return result;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    result.error = "catalog file could not be opened";
    return result;
  }
  result.bytes.assign(std::istreambuf_iterator<char>{stream},
                      std::istreambuf_iterator<char>{});
  if (stream.bad() || result.bytes.size() != size) {
    result.bytes.clear();
    result.error = "catalog file could not be read completely";
    return result;
  }
  result.succeeded = true;
  return result;
}

}  // namespace azzs::adapters::infrastructure
