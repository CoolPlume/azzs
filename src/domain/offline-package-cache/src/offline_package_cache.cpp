#include "azzs/domain/offline_package_cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace azzs::domain::offline_package_cache {
namespace {

constexpr std::size_t kMaxIdentityText = 256;
constexpr std::size_t kMaxRootId = 96;

[[nodiscard]] bool printable_ascii(std::string_view value,
                                   std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  for (unsigned char character : value) {
    if (character < 0x21 || character > 0x7e) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool root_id_character(unsigned char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '-' ||
         character == '_' || character == '.';
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view value,
                                  std::uint64_t seed =
                                      14695981039346656037ULL) noexcept {
  auto hash = seed;
  for (unsigned char character : value) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::string hex(std::uint64_t value) {
  constexpr std::array<char, 16> digits{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string result(16, '0');
  for (std::size_t index = 0; index < result.size(); ++index) {
    auto const shift = (result.size() - index - 1U) * 4U;
    result[index] = digits[(value >> shift) & 0x0fU];
  }
  return result;
}

[[nodiscard]] std::uint64_t hash_field(std::uint64_t hash,
                                        std::string_view field) noexcept {
  hash = fnv1a(field, hash);
  constexpr char separator = '\0';
  return fnv1a(std::string_view{&separator, 1}, hash);
}

}  // namespace

bool CacheAssetIdentity::valid() const noexcept {
  return printable_ascii(software_id, kMaxIdentityText) &&
         printable_ascii(version, kMaxIdentityText) &&
         printable_ascii(source_identity, kMaxIdentityText) &&
         architecture != CacheArchitecture::unknown;
}

std::string CacheAssetIdentity::stable_key() const {
  auto hash = hash_field(14695981039346656037ULL, software_id);
  hash = hash_field(hash, version);
  hash = hash_field(hash, to_string(architecture));
  hash = hash_field(hash, source_identity);
  return hex(hash);
}

bool CacheAsset::valid() const noexcept {
  return identity.valid() &&
         (!expected_bytes.has_value() || *expected_bytes > 0);
}

bool CacheAsset::cacheable() const noexcept {
  return kind == CacheAssetKind::full_package ||
         kind == CacheAssetKind::online_installer;
}

bool CacheAsset::requires_network() const noexcept {
  return kind == CacheAssetKind::online_installer ||
         kind == CacheAssetKind::online_only ||
         kind == CacheAssetKind::managed_source;
}

bool BuiltInPackageResource::valid() const noexcept {
  return asset.valid() && asset.kind == CacheAssetKind::full_package;
}

bool ControlledCacheRoot::valid() const noexcept {
  if (id.empty() || id.size() > kMaxRootId) {
    return false;
  }
  for (unsigned char character : id) {
    if (!root_id_character(character)) {
      return false;
    }
  }
  return true;
}

char const* to_string(CacheArchitecture value) noexcept {
  switch (value) {
    case CacheArchitecture::x64:
      return "x64";
    case CacheArchitecture::arm64:
      return "arm64";
    case CacheArchitecture::architecture_independent:
      return "independent";
    case CacheArchitecture::unknown:
      return "unknown";
  }
  return "unknown";
}

char const* to_string(CacheAssetKind value) noexcept {
  switch (value) {
    case CacheAssetKind::full_package:
      return "full-package";
    case CacheAssetKind::online_installer:
      return "online-installer";
    case CacheAssetKind::online_only:
      return "online-only";
    case CacheAssetKind::managed_source:
      return "managed-source";
    case CacheAssetKind::unsupported:
      return "unsupported";
  }
  return "unsupported";
}

char const* to_string(CacheLocationKind value) noexcept {
  switch (value) {
    case CacheLocationKind::system_directory:
      return "system-directory";
    case CacheLocationKind::local_volume:
      return "local-volume";
    case CacheLocationKind::network_share:
      return "network-share";
    case CacheLocationKind::removable_media:
      return "removable-media";
  }
  return "system-directory";
}

char const* to_string(CacheRetentionPolicy value) noexcept {
  switch (value) {
    case CacheRetentionPolicy::delete_immediately:
      return "delete-immediately";
    case CacheRetentionPolicy::retain_seven_days:
      return "retain-seven-days";
    case CacheRetentionPolicy::retain_thirty_days:
      return "retain-thirty-days";
    case CacheRetentionPolicy::retain_indefinitely:
      return "retain-indefinitely";
  }
  return "retain-seven-days";
}

}  // namespace azzs::domain::offline_package_cache
