#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_selection.hpp"

namespace azzs::adapters::windows {

// A resolver registration is an explicitly reviewed source fact.  The
// resolver never discovers a URL, follows a release page, or derives an
// installer from a display name; every field required for a stable snapshot
// must be supplied by the registration owner.
struct WindowsSourceResolutionRegistration final {
  std::string software_id;
  domain::software_catalog::SourcePurpose purpose{
      domain::software_catalog::SourcePurpose::primary};
  std::string declared_address;
  std::string version;
  std::string actual_address;
  std::string hosting_mechanism;
  std::string branch;
  std::vector<domain::software_selection::ResolvedPackage> packages;
  bool network_required{true};
  std::string capability_version;
};

// The download adapter binds a redirect to the complete registered asset
// identity.  Keeping this value type free of WinHTTP types also makes the
// redirect policy contract testable without opening a network connection.
struct WindowsControlledAddressIdentity final {
  std::string host;
  std::uint16_t port{0};
  std::string path;
  std::string query;
};

[[nodiscard]] std::optional<WindowsControlledAddressIdentity>
parse_windows_controlled_https_address(std::string_view address);

[[nodiscard]] bool windows_controlled_redirect_matches(
    WindowsControlledAddressIdentity const& registered,
    WindowsControlledAddressIdentity const& target) noexcept;

// GitHub release assets may redirect from github.com to its signed CDN.  The
// exception remains bound to the registered release path and asset filename.
[[nodiscard]] bool windows_controlled_github_release_redirect_matches(
    WindowsControlledAddressIdentity const& registered,
    WindowsControlledAddressIdentity const& target) noexcept;

// A non-zero resume request is valid only when the response identifies a
// byte range beginning exactly at the requested offset.
[[nodiscard]] bool windows_controlled_content_range_matches(
    std::string_view content_range, std::uint64_t resume_from) noexcept;

class WindowsRegisteredSourceResolver final
    : public application::software_selection::ControlledSourceResolver {
 public:
  explicit WindowsRegisteredSourceResolver(
      std::vector<WindowsSourceResolutionRegistration> registrations = {});

  [[nodiscard]] application::software_selection::SourceResolutionResult resolve(
      std::string_view software_id,
      domain::software_catalog::CatalogSource const& declared_source) override;

 private:
  std::vector<WindowsSourceResolutionRegistration> registrations_;
};

struct WindowsPresenceRegistration final {
  std::string software_id;
  std::vector<std::wstring> display_names;
};

enum class WindowsPresenceRegistryHive {
  current_user,
  local_machine,
};

enum class WindowsPresenceRegistryView {
  view_32,
  view_64,
};

enum class WindowsPresenceRegistryReadStatus {
  succeeded,
  root_absent,
  failed,
};

struct WindowsPresenceRegistryEntry final {
  std::wstring display_name;
  // Empty means the uninstall entry did not expose a readable DisplayVersion.
  std::wstring display_version;
};

struct WindowsPresenceRegistryRead final {
  WindowsPresenceRegistryReadStatus status{
      WindowsPresenceRegistryReadStatus::failed};
  std::vector<WindowsPresenceRegistryEntry> entries;
  std::string detail;
};

// This is the only registry seam used by the presence detector. Production
// code supplies the fixed Windows implementation; contract tests can return
// deterministic read-only observations without touching the host registry.
class WindowsPresenceRegistryQuery {
 public:
  virtual ~WindowsPresenceRegistryQuery() = default;
  [[nodiscard]] virtual WindowsPresenceRegistryRead read(
      WindowsPresenceRegistryHive hive,
      WindowsPresenceRegistryView view) const = 0;
};

// Reads only the fixed Windows uninstall registry locations.  An unknown
// registration, unreadable hive, or malformed value is reported as an
// incomplete observation and is never converted into "not installed".
class WindowsRegistrySoftwarePresenceDetector final
    : public application::software_selection::SoftwarePresenceDetector {
 public:
  explicit WindowsRegistrySoftwarePresenceDetector(
      std::vector<WindowsPresenceRegistration> registrations = {});
  WindowsRegistrySoftwarePresenceDetector(
      std::unique_ptr<WindowsPresenceRegistryQuery> query,
      std::vector<WindowsPresenceRegistration> registrations = {});

  [[nodiscard]] application::software_selection::PresenceDetection detect(
      std::string_view software_id) override;
  [[nodiscard]] application::software_selection::PresenceDetection detect_version(
      std::string_view software_id, std::string_view expected_version);

 private:
  [[nodiscard]] application::software_selection::PresenceDetection detect_impl(
      std::string_view software_id,
      std::optional<std::wstring> expected_version);
  std::unique_ptr<WindowsPresenceRegistryQuery> query_;
  std::vector<WindowsPresenceRegistration> registrations_;
};

// The downloader accepts only an asset identity that was explicitly bound to
// a registered HTTPS address.  The application cache contract still receives
// opaque identities; URL parsing and WinHTTP are confined to this adapter.
class WindowsControlledPackageDownloader final
    : public application::offline_package_cache::ControlledPackageDownloader {
 public:
  using CacheAsset = domain::offline_package_cache::CacheAsset;
  using CacheAssetIdentity = domain::offline_package_cache::CacheAssetIdentity;
  using ControlledCacheRoot = domain::offline_package_cache::ControlledCacheRoot;

  explicit WindowsControlledPackageDownloader(ControlledCacheRoot root);

  WindowsControlledPackageDownloader(
      WindowsControlledPackageDownloader const&) = delete;
  WindowsControlledPackageDownloader& operator=(
      WindowsControlledPackageDownloader const&) = delete;

  // Called by the composition root after a source snapshot has been accepted.
  // Duplicate identities are rejected unless the exact same URL is supplied.
  [[nodiscard]] bool register_source(CacheAssetIdentity const& identity,
                                      std::string_view actual_address,
                                      std::optional<std::uint64_t> expected_bytes =
                                          std::nullopt,
                                      std::optional<std::string_view>
                                          expected_sha256 = std::nullopt);
  void clear_registered_sources() noexcept;

  [[nodiscard]] application::offline_package_cache::ControlledDownloadResult
  transfer(application::offline_package_cache::ControlledDownloadRequest const& request)
      override;

 private:
  struct RegisteredSource final {
    CacheAssetIdentity identity;
    std::string address;
    std::vector<std::string> allowed_redirect_hosts;
    std::optional<std::uint64_t> expected_bytes;
    std::optional<std::string> expected_sha256;
  };

  ControlledCacheRoot root_;
  std::vector<RegisteredSource> sources_;
};

// The registration set contains only the small set of reviewed fixed assets;
// entries without a frozen .exe/.msi address are rejected by the resolver.
// Presence rules remain project-owned and never accept arbitrary registry
// paths or display names supplied by the catalog.
[[nodiscard]] std::vector<WindowsSourceResolutionRegistration>
initial_windows_source_registrations();

[[nodiscard]] std::vector<WindowsPresenceRegistration>
initial_windows_presence_registrations();

}  // namespace azzs::adapters::windows
