#include "azzs/adapters/infrastructure/software_catalog_file.hpp"

#include <array>
#include <fstream>
#include <string>
#include <utility>

namespace azzs::adapters::infrastructure {

LocalSoftwareCatalogFileReader::LocalSoftwareCatalogFileReader(
    std::string built_in_path, std::string update_path)
    : built_in_path_(std::move(built_in_path)),
      update_path_(std::move(update_path)) {}

LocalSoftwareCatalogFileReader
LocalSoftwareCatalogFileReader::from_verified_built_in(std::string bytes) {
  LocalSoftwareCatalogFileReader reader;
  reader.verified_built_in_bytes_ = std::move(bytes);
  return reader;
}

application::software_catalog::CatalogFileRead
LocalSoftwareCatalogFileReader::read_built_in() const {
  if (verified_built_in_bytes_.has_value()) {
    if (verified_built_in_bytes_->empty()) {
      return {.error = "trusted built-in catalog bytes are unavailable"};
    }
    return {.succeeded = true,
            .path = "embedded-software-catalog",
            .bytes = *verified_built_in_bytes_};
  }
  if (built_in_path_.empty()) {
    return {.error = "trusted built-in catalog source is unavailable"};
  }
  return read(built_in_path_);
}

application::software_catalog::CatalogFileRead
LocalSoftwareCatalogFileReader::read_update() const {
  if (update_path_.empty()) {
    return {.error = "trusted catalog update source is unavailable"};
  }
  return read(update_path_);
}

application::software_catalog::CatalogFileRead
LocalSoftwareCatalogFileReader::read_manual_import(
    std::string const& path) const {
  return read(path);
}

application::software_catalog::CatalogFileRead
LocalSoftwareCatalogFileReader::read(std::string const& path) const {
  constexpr std::uintmax_t k_max_catalog_size = 16U * 1024U * 1024U;
  application::software_catalog::CatalogFileRead result{.path = path};
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    result.error = "catalog file could not be opened";
    return result;
  }
  std::array<char, 64U * 1024U> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    auto const read = stream.gcount();
    if (read > 0) {
      auto const count = static_cast<std::size_t>(read);
      if (result.bytes.size() > k_max_catalog_size - count) {
        result.bytes.clear();
        result.error = "catalog file exceeds the supported size";
        return result;
      }
      result.bytes.append(buffer.data(), count);
    }
  }
  if (!stream.eof()) {
    result.bytes.clear();
    result.error = "catalog file could not be read completely";
    return result;
  }
  result.succeeded = true;
  return result;
}

}  // namespace azzs::adapters::infrastructure
