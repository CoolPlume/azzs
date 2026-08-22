#include "azzs/adapters/infrastructure/local_software_optimization_catalog_file.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace azzs::adapters::infrastructure {
namespace {

constexpr std::uintmax_t kMaximumCatalogBytes = 4U * 1024U * 1024U;

}  // namespace

application::SoftwareOptimizationCatalogLocalImportRead
LocalSoftwareOptimizationCatalogFile::read(std::string_view path) {
  if (path.empty() || path.find('\0') != std::string_view::npos) {
    return {.error = "catalog path is empty or contains an embedded null"};
  }

  std::filesystem::path const file_path{std::string{path}};
  std::error_code error;
  auto const status = std::filesystem::status(file_path, error);
  if (error) {
    return {.error = "catalog file status failed: " + error.message()};
  }
  if (!std::filesystem::is_regular_file(status)) {
    return {.error = "catalog path is not a regular file"};
  }
  auto const size = std::filesystem::file_size(file_path, error);
  if (error) {
    return {.error = "catalog file size failed: " + error.message()};
  }
  if (size == 0 || size > kMaximumCatalogBytes ||
      size > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::streamsize>::max())) {
    return {.error = "catalog file size is outside the supported range"};
  }

  std::ifstream stream{file_path, std::ios::binary};
  if (!stream) {
    return {.error = "catalog file could not be opened"};
  }
  std::string source(static_cast<std::size_t>(size), '\0');
  stream.read(source.data(), static_cast<std::streamsize>(source.size()));
  if (!stream || stream.gcount() != static_cast<std::streamsize>(source.size())) {
    return {.error = "catalog file could not be read completely"};
  }
  if (std::ranges::find(source, '\0') != source.end()) {
    return {.error = "catalog file contains binary null bytes"};
  }
  return {.succeeded = true, .source = std::move(source)};
}

}  // namespace azzs::adapters::infrastructure
