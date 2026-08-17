#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace azzs::adapters::windows {

struct BundledCatalogResourceExpectation final {
  std::uintmax_t byte_count{};
  std::array<std::uint8_t, 32> sha256{};
};

// The bytes are owned by this value and are exposed only as a const view after
// the resource path, length, and digest have been checked by the reader.
class VerifiedBundledCatalogResource final {
 public:
  explicit VerifiedBundledCatalogResource(std::string bytes) noexcept;

  [[nodiscard]] std::string const& bytes() const noexcept;

 private:
  std::string bytes_;
};

struct BundledCatalogResourceRead final {
  std::optional<VerifiedBundledCatalogResource> resource;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return resource.has_value();
  }
};

// Windows-only package reader. The root is supplied by the composition root in
// production and by an isolated fixture in the headless adapter contract.
class WindowsBundledCatalogResourceReader final {
 public:
  explicit WindowsBundledCatalogResourceReader(
      std::filesystem::path resource_root);

  [[nodiscard]] BundledCatalogResourceRead read(
      std::filesystem::path const& relative_path,
      BundledCatalogResourceExpectation const& expectation) const;

 private:
  std::filesystem::path resource_root_;
};

}  // namespace azzs::adapters::windows
