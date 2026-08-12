#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azzs::domain::offline_package_cache {

enum class CacheArchitecture {
  x64,
  arm64,
  architecture_independent,
  unknown,
};

enum class CacheAssetKind {
  full_package,
  online_installer,
  online_only,
  managed_source,
  unsupported,
};

// The source identity is an opaque stable fingerprint produced from one
// accepted source snapshot. It is intentionally not a URL or a file path.
struct CacheAssetIdentity final {
  std::string software_id;
  std::string version;
  CacheArchitecture architecture{CacheArchitecture::unknown};
  std::string source_identity;

  [[nodiscard]] bool valid() const noexcept;
  // This fixed-width file-system-safe key is only a storage slot. Readers
  // still validate the complete identity in the completion marker.
  [[nodiscard]] std::string stable_key() const;

  auto operator<=>(CacheAssetIdentity const&) const = default;
};

struct CacheAsset final {
  CacheAssetIdentity identity;
  CacheAssetKind kind{CacheAssetKind::unsupported};
  bool resume_supported{false};
  std::optional<std::uint64_t> expected_bytes;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool cacheable() const noexcept;
  [[nodiscard]] bool requires_network() const noexcept;

  auto operator<=>(CacheAsset const&) const = default;
};

struct BuiltInPackageResource final {
  CacheAsset asset;

  [[nodiscard]] bool valid() const noexcept;

  auto operator<=>(BuiltInPackageResource const&) const = default;
};

enum class CacheLocationKind {
  system_directory,
  local_volume,
  network_share,
  removable_media,
};

// A root is selected by an adapter-owned configuration registry. The core and
// UI only retain this typed opaque identifier, never a user-supplied path.
struct ControlledCacheRoot final {
  CacheLocationKind kind{CacheLocationKind::system_directory};
  std::string id;

  [[nodiscard]] bool valid() const noexcept;

  auto operator<=>(ControlledCacheRoot const&) const = default;
};

enum class CacheRetentionPolicy {
  delete_immediately,
  retain_seven_days,
  retain_thirty_days,
  retain_indefinitely,
};

[[nodiscard]] char const* to_string(CacheArchitecture value) noexcept;
[[nodiscard]] char const* to_string(CacheAssetKind value) noexcept;
[[nodiscard]] char const* to_string(CacheLocationKind value) noexcept;
[[nodiscard]] char const* to_string(CacheRetentionPolicy value) noexcept;

}  // namespace azzs::domain::offline_package_cache
