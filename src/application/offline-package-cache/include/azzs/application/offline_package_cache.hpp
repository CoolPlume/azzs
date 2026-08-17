#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "azzs/application/clock.hpp"
#include "azzs/domain/offline_package_cache.hpp"
#include "azzs/domain/software_selection.hpp"

namespace azzs::application::offline_package_cache {

namespace cache_domain = domain::offline_package_cache;
namespace selection_domain = domain::software_selection;

using cache_domain::BuiltInPackageResource;
using cache_domain::CacheAsset;
using cache_domain::CacheAssetIdentity;
using cache_domain::CacheRetentionPolicy;
using cache_domain::ControlledCacheRoot;

struct CacheRootObservation final {
  bool available{false};
  std::optional<std::uint64_t> free_bytes;
  std::string detail;
};

enum class CompletedCacheReadCode {
  found,
  absent,
  root_unavailable,
  failed,
};

struct CompletedCacheEntry final {
  CacheAssetIdentity identity;
  std::uint64_t byte_count{};
  WallClockTime completed_at{};
};

struct CompletedCacheRead final {
  CompletedCacheReadCode code{CompletedCacheReadCode::failed};
  std::optional<CompletedCacheEntry> entry;
  std::string detail;
};

struct CompletedCacheList final {
  CompletedCacheReadCode code{CompletedCacheReadCode::failed};
  std::vector<CompletedCacheEntry> entries;
  std::string detail;
};

enum class CacheWriteCode {
  succeeded,
  failed,
};

struct CacheWriteResult final {
  CacheWriteCode code{CacheWriteCode::failed};
  std::string detail;
};

// A write session owns one temporary file and one same-asset lock. Keeping it
// alive is the only way a paused transfer remains resumable in this process.
class PackageCacheWriteSession {
 public:
  virtual ~PackageCacheWriteSession() = default;

  [[nodiscard]] virtual std::uint64_t received_bytes() const noexcept = 0;
  [[nodiscard]] virtual CacheWriteResult append(
      std::span<std::byte const> bytes) = 0;
  [[nodiscard]] virtual CacheWriteResult complete(
      WallClockTime completed_at) = 0;
  [[nodiscard]] virtual CacheWriteResult abandon() = 0;
};

enum class CacheWriteBeginCode {
  acquired,
  busy,
  root_unavailable,
  insufficient_space,
  failed,
};

struct CacheWriteBegin final {
  CacheWriteBeginCode code{CacheWriteBeginCode::failed};
  std::unique_ptr<PackageCacheWriteSession> session;
  std::string detail;
};

enum class CacheStorageCleanupCode {
  succeeded,
  root_unavailable,
  failed,
};

struct CacheStorageCleanupResult final {
  CacheStorageCleanupCode code{CacheStorageCleanupCode::failed};
  std::size_t removed_partial_count{};
  std::string detail;
};

enum class CacheStorageRemovalCode {
  removed,
  absent,
  root_unavailable,
  failed,
};

struct CacheStorageRemovalResult final {
  CacheStorageRemovalCode code{CacheStorageRemovalCode::failed};
  std::string detail;
};

// The storage seam resolves only adapter-registered roots and derived opaque
// asset keys. It never accepts arbitrary paths, names, or user input.
class PackageCacheStorage {
 public:
  virtual ~PackageCacheStorage() = default;

  [[nodiscard]] virtual CacheRootObservation observe_root(
      ControlledCacheRoot const& root) = 0;
  [[nodiscard]] virtual CompletedCacheRead read_completed(
      ControlledCacheRoot const& root,
      CacheAssetIdentity const& identity) = 0;
  [[nodiscard]] virtual CompletedCacheList list_completed(
      ControlledCacheRoot const& root) = 0;
  [[nodiscard]] virtual CacheWriteBegin begin_write(
      ControlledCacheRoot const& root, CacheAsset const& asset) = 0;
  [[nodiscard]] virtual CacheStorageCleanupResult clean_orphaned_partials(
      ControlledCacheRoot const& root) = 0;
  [[nodiscard]] virtual CacheStorageRemovalResult remove_completed(
      ControlledCacheRoot const& root,
      CacheAssetIdentity const& identity) = 0;
};

struct ControlledDownloadRequest final {
  CacheAsset asset;
  ControlledCacheRoot cache_root;
  std::uint64_t resume_from_bytes{};
};

enum class ControlledDownloadCode {
  completed,
  paused,
  interrupted,
  failed,
  network_unavailable,
};

struct ControlledDownloadResult final {
  ControlledDownloadCode code{ControlledDownloadCode::failed};
  std::vector<std::byte> bytes;
  std::string detail;
};

// Production downloaders receive a resolved, typed asset and a selected
// controlled root. URL resolution and any path handling remain outside this
// seam and cannot be supplied by a caller.
class ControlledPackageDownloader {
 public:
  virtual ~ControlledPackageDownloader() = default;
  [[nodiscard]] virtual ControlledDownloadResult transfer(
      ControlledDownloadRequest const& request) = 0;
};

class PackageCacheNetworkObserver {
 public:
  virtual ~PackageCacheNetworkObserver() = default;
  [[nodiscard]] virtual bool available() const noexcept = 0;
};

enum class OfflinePackageAvailability {
  built_in_available,
  cached_available,
  ready_to_download,
  waiting_for_network,
  online_installer_requires_network,
  managed_source_requires_network,
  unsupported,
};

enum class PackageCacheSessionState {
  idle,
  downloading,
  paused,
  completed,
  failed,
  waiting_for_network,
  location_unavailable,
  insufficient_space,
  busy,
  unsupported,
};

struct OfflinePackageItemSnapshot final {
  CacheAsset asset;
  OfflinePackageAvailability availability{
      OfflinePackageAvailability::unsupported};
  PackageCacheSessionState session{PackageCacheSessionState::idle};
  bool cache_present{false};
  bool continuation_eligible{false};
  bool manual_retry_available{false};
  std::size_t automatic_attempts{};
  std::optional<std::uint64_t> cached_bytes;
  std::string detail;
};

enum class CacheLocationState {
  available,
  unavailable,
  invalid,
};

struct OfflinePackageCacheSnapshot final {
  ControlledCacheRoot selected_root;
  CacheLocationState location_state{CacheLocationState::invalid};
  CacheRetentionPolicy retention{CacheRetentionPolicy::retain_seven_days};
  bool network_available{false};
  std::vector<OfflinePackageItemSnapshot> items;
  std::string location_detail;
};

enum class CacheActionCode {
  completed,
  already_available,
  paused,
  restarted_from_zero,
  waiting_for_network,
  location_unavailable,
  insufficient_space,
  space_unknown,
  busy,
  unsupported,
  asset_not_current,
  retry_not_available,
  interrupted,
  failed,
};

struct CacheActionResult final {
  CacheActionCode code{CacheActionCode::failed};
  OfflinePackageItemSnapshot item;
  std::string detail;
};

enum class CacheLocationConfigureCode {
  selected,
  selected_unavailable,
  rejected,
};

struct CacheLocationConfigureResult final {
  CacheLocationConfigureCode code{CacheLocationConfigureCode::rejected};
  OfflinePackageCacheSnapshot snapshot;
  std::string detail;
};

enum class CacheCleanupCode {
  completed,
  rejected_after_shutdown,
  failed,
};

struct CacheCleanupResult final {
  CacheCleanupCode code{CacheCleanupCode::failed};
  std::size_t removed_completed_count{};
  std::size_t removed_partial_count{};
  std::string detail;
};

// Owns cache availability, current-process download sessions, retry policy,
// selected-root state, and retention. It intentionally does not persist paused
// sessions: shutdown always removes unfinished bytes before the next run.
class OfflinePackageCacheService final {
 public:
  static constexpr std::size_t kAutomaticFailureRetries = 2;

  OfflinePackageCacheService(PackageCacheStorage& storage,
                             ControlledPackageDownloader& downloader,
                             PackageCacheNetworkObserver const& network,
                             Clock const& clock,
                             ControlledCacheRoot initial_root,
                             CacheRetentionPolicy retention =
                                 CacheRetentionPolicy::retain_seven_days);
  ~OfflinePackageCacheService();

  OfflinePackageCacheService(OfflinePackageCacheService const&) = delete;
  OfflinePackageCacheService& operator=(OfflinePackageCacheService const&) =
      delete;

  // Replacing the accepted source snapshot removes non-current assets from
  // this service and abandons their partial bytes. Completed cache files stay
  // physically retained but cannot make a removed catalog item installable.
  void synchronize_assets(std::vector<CacheAsset> assets,
                          std::vector<BuiltInPackageResource> built_in = {});
  [[nodiscard]] CacheLocationConfigureResult configure_location(
      ControlledCacheRoot root);
  void set_retention(CacheRetentionPolicy retention) noexcept;

  [[nodiscard]] OfflinePackageCacheSnapshot snapshot();
  [[nodiscard]] CacheActionResult download(CacheAssetIdentity const& identity);
  [[nodiscard]] CacheActionResult resume(CacheAssetIdentity const& identity);
  // Discards only this in-process unfinished transfer and its temporary
  // bytes. Completed cache entries are deliberately untouched.
  [[nodiscard]] CacheActionResult abandon(CacheAssetIdentity const& identity);
  [[nodiscard]] CacheActionResult retry(CacheAssetIdentity const& identity);
  [[nodiscard]] CacheCleanupResult clean();
  // Removes all completed workbench cache entries at the selected root. It
  // leaves bundled resources and in-progress sessions untouched.
  [[nodiscard]] CacheCleanupResult clear_completed();

  // Called by the composition root during orderly shutdown. It never removes
  // completed cache entries or bundled resources.
  void shutdown() noexcept;

 private:
  struct Session;

  [[nodiscard]] CacheActionResult action_for_missing_asset(
      CacheAssetIdentity const& identity) const;
  [[nodiscard]] OfflinePackageItemSnapshot item_snapshot(
      CacheAsset const& asset);
  [[nodiscard]] OfflinePackageAvailability availability_for(
      CacheAsset const& asset, bool cache_present) const noexcept;
  [[nodiscard]] CacheActionResult run_download(CacheAsset const& asset,
                                                bool manual_retry,
                                                bool resume);
  [[nodiscard]] CacheActionResult begin_transfer(CacheAsset const& asset,
                                                  bool manual_retry,
                                                  bool resume,
                                                  bool restarted_from_zero,
                                                  std::size_t automatic_attempts = 0);
  [[nodiscard]] std::optional<std::size_t> asset_index(
      CacheAssetIdentity const& identity) const noexcept;
  [[nodiscard]] std::optional<std::size_t> session_index(
      CacheAssetIdentity const& identity) const noexcept;
  [[nodiscard]] bool built_in_available(
      CacheAssetIdentity const& identity) const noexcept;
  [[nodiscard]] CacheLocationState current_location_state(
      CacheRootObservation const& observation) const noexcept;
  void abandon_session(std::size_t index) noexcept;

  PackageCacheStorage& storage_;
  ControlledPackageDownloader& downloader_;
  PackageCacheNetworkObserver const& network_;
  Clock const& clock_;
  ControlledCacheRoot selected_root_;
  CacheRetentionPolicy retention_;
  std::vector<CacheAsset> assets_;
  std::vector<BuiltInPackageResource> built_in_;
  std::vector<Session> sessions_;
  bool shutdown_{false};
};

// Converts only an already accepted issue-06 source snapshot into the opaque
// cache identity used by this module. The resolved address is hashed here and
// is never passed to a cache store or downloader.
[[nodiscard]] std::optional<CacheAsset> make_cache_asset(
    selection_domain::ResolvedSourceSnapshot const& source,
    selection_domain::ResolvedPackage const& package);

[[nodiscard]] char const* to_string(OfflinePackageAvailability value) noexcept;
[[nodiscard]] char const* to_string(PackageCacheSessionState value) noexcept;
[[nodiscard]] char const* to_string(CacheActionCode value) noexcept;

}  // namespace azzs::application::offline_package_cache
