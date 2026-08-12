#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "azzs/application/offline_package_cache.hpp"

namespace azzs::adapters::infrastructure {

struct ControlledPackageCacheRootConfiguration final {
  application::offline_package_cache::ControlledCacheRoot root;
  // This path is supplied only by the composition root from controlled machine
  // configuration. It never crosses into the application cache contract.
  std::filesystem::path directory;
  bool create_if_missing{false};
};

// A filesystem adapter for a closed set of host-registered cache roots. Asset
// identity selects only a derived fixed-width filename beneath those roots.
class LocalPackageCacheStorage final
    : public application::offline_package_cache::PackageCacheStorage {
 public:
  explicit LocalPackageCacheStorage(
      std::vector<ControlledPackageCacheRootConfiguration> roots);
  ~LocalPackageCacheStorage() override;

  LocalPackageCacheStorage(LocalPackageCacheStorage const&) = delete;
  LocalPackageCacheStorage& operator=(LocalPackageCacheStorage const&) =
      delete;
  LocalPackageCacheStorage(LocalPackageCacheStorage&&) noexcept;
  LocalPackageCacheStorage& operator=(LocalPackageCacheStorage&&) noexcept;

  [[nodiscard]] application::offline_package_cache::CacheRootObservation
  observe_root(
      application::offline_package_cache::ControlledCacheRoot const& root)
      override;
  [[nodiscard]] application::offline_package_cache::CompletedCacheRead
  read_completed(
      application::offline_package_cache::ControlledCacheRoot const& root,
      application::offline_package_cache::CacheAssetIdentity const& identity)
      override;
  [[nodiscard]] application::offline_package_cache::CompletedCacheList
  list_completed(
      application::offline_package_cache::ControlledCacheRoot const& root)
      override;
  [[nodiscard]] application::offline_package_cache::CacheWriteBegin
  begin_write(
      application::offline_package_cache::ControlledCacheRoot const& root,
      application::offline_package_cache::CacheAsset const& asset) override;
  [[nodiscard]] application::offline_package_cache::CacheStorageCleanupResult
  clean_orphaned_partials(
      application::offline_package_cache::ControlledCacheRoot const& root)
      override;
  [[nodiscard]] application::offline_package_cache::CacheStorageRemovalResult
  remove_completed(
      application::offline_package_cache::ControlledCacheRoot const& root,
      application::offline_package_cache::CacheAssetIdentity const& identity)
      override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Issue 07 deliberately does not implement real network transfers. This
// adapter makes that boundary explicit while preserving the typed downloader
// seam for a later controlled resolver/network implementation.
class UnavailableControlledPackageDownloader final
    : public application::offline_package_cache::ControlledPackageDownloader {
 public:
  [[nodiscard]] application::offline_package_cache::ControlledDownloadResult
  transfer(application::offline_package_cache::ControlledDownloadRequest const&
               request) override;
};

}  // namespace azzs::adapters::infrastructure
