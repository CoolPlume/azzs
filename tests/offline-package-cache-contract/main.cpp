#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/offline_package_cache.hpp"

namespace {

namespace cache = azzs::application::offline_package_cache;
namespace domain_cache = azzs::domain::offline_package_cache;

using azzs::application::Clock;
using azzs::application::WallClockTime;
using cache::CacheActionCode;
using cache::CacheLocationState;
using cache::CacheRootObservation;
using cache::CacheStorageCleanupCode;
using cache::CacheStorageCleanupResult;
using cache::CacheStorageRemovalCode;
using cache::CacheStorageRemovalResult;
using cache::CacheWriteBegin;
using cache::CacheWriteBeginCode;
using cache::CacheWriteCode;
using cache::CacheWriteResult;
using cache::CompletedCacheEntry;
using cache::CompletedCacheList;
using cache::CompletedCacheRead;
using cache::CompletedCacheReadCode;
using cache::ControlledDownloadCode;
using cache::ControlledDownloadRequest;
using cache::ControlledDownloadResult;
using cache::ControlledPackageDownloader;
using cache::OfflinePackageAvailability;
using cache::OfflinePackageCacheService;
using cache::PackageCacheNetworkObserver;
using cache::PackageCacheStorage;
using cache::PackageCacheWriteSession;
using domain_cache::BuiltInPackageResource;
using domain_cache::CacheArchitecture;
using domain_cache::CacheAsset;
using domain_cache::CacheAssetIdentity;
using domain_cache::CacheAssetKind;
using domain_cache::CacheLocationKind;
using domain_cache::CacheRetentionPolicy;
using domain_cache::ControlledCacheRoot;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "offline package cache contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (auto const character : text) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

class FixedClock final : public Clock {
 public:
  explicit FixedClock(WallClockTime now) : now_(now) {}

  [[nodiscard]] WallClockTime now() const noexcept override { return now_; }
  void advance(std::chrono::milliseconds amount) noexcept { now_ += amount; }

 private:
  WallClockTime now_;
};

class FixtureNetwork final : public PackageCacheNetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override { return online; }
  bool online{true};
};

class InMemoryCacheStorage final : public PackageCacheStorage {
 public:
  struct Root final {
    bool available{true};
    std::optional<std::uint64_t> free_bytes{1024U * 1024U};
    std::string detail;
    struct Partial final {
      CacheAsset asset;
      std::vector<std::byte> data;
    };
    struct Stored final {
      CompletedCacheEntry entry;
      std::vector<std::byte> data;
    };
    std::map<std::string, Partial> partials;
    std::map<std::string, Stored> completed;
  };

  class Session final : public PackageCacheWriteSession {
   public:
    Session(InMemoryCacheStorage& owner, std::string root_id,
            CacheAsset asset) noexcept
        : owner_(owner), root_id_(std::move(root_id)), asset_(std::move(asset)) {}

    ~Session() override { static_cast<void>(abandon()); }

    [[nodiscard]] std::uint64_t received_bytes() const noexcept override {
      auto const* partial = owner_.partial(root_id_, asset_.identity);
      return partial == nullptr ? 0 : partial->data.size();
    }

    [[nodiscard]] CacheWriteResult append(
        std::span<std::byte const> input) override {
      if (closed_) {
        return {.detail = "write session is closed"};
      }
      auto* partial = owner_.partial(root_id_, asset_.identity);
      if (partial == nullptr) {
        return {.detail = "temporary cache file is absent"};
      }
      partial->data.insert(partial->data.end(), input.begin(), input.end());
      return {.code = CacheWriteCode::succeeded};
    }

    [[nodiscard]] CacheWriteResult complete(WallClockTime completed_at) override {
      if (closed_) {
        return {.detail = "write session is closed"};
      }
      auto const key = asset_.identity.stable_key();
      auto root = owner_.roots.find(root_id_);
      if (root == owner_.roots.end() || !root->second.available) {
        return {.detail = "cache root disappeared"};
      }
      auto partial = root->second.partials.find(key);
      if (partial == root->second.partials.end()) {
        return {.detail = "temporary cache file is absent"};
      }
      auto stored = Root::Stored{
          .entry = {.identity = asset_.identity,
                    .byte_count = partial->second.data.size(),
                    .completed_at = completed_at},
          .data = std::move(partial->second.data),
      };
      root->second.partials.erase(partial);
      root->second.completed.insert_or_assign(key, std::move(stored));
      owner_.locks.erase(owner_.lock_key(root_id_, asset_.identity));
      closed_ = true;
      return {.code = CacheWriteCode::succeeded};
    }

    [[nodiscard]] CacheWriteResult abandon() override {
      if (closed_) {
        return {.code = CacheWriteCode::succeeded};
      }
      auto root = owner_.roots.find(root_id_);
      if (root != owner_.roots.end()) {
        root->second.partials.erase(asset_.identity.stable_key());
      }
      owner_.locks.erase(owner_.lock_key(root_id_, asset_.identity));
      closed_ = true;
      return {.code = CacheWriteCode::succeeded};
    }

   private:
    InMemoryCacheStorage& owner_;
    std::string root_id_;
    CacheAsset asset_;
    bool closed_{false};
  };

  [[nodiscard]] CacheRootObservation observe_root(
      ControlledCacheRoot const& root) override {
    auto const found = roots.find(root.id);
    if (found == roots.end()) {
      return {.detail = "unknown controlled root"};
    }
    return {.available = found->second.available,
            .free_bytes = found->second.free_bytes,
            .detail = found->second.detail};
  }

  [[nodiscard]] CompletedCacheRead read_completed(
      ControlledCacheRoot const& root,
      CacheAssetIdentity const& identity) override {
    auto const found = roots.find(root.id);
    if (found == roots.end() || !found->second.available) {
      return {.code = CompletedCacheReadCode::root_unavailable,
              .detail = "cache root is unavailable"};
    }
    auto const entry = found->second.completed.find(identity.stable_key());
    if (entry == found->second.completed.end() ||
        entry->second.entry.identity != identity) {
      return {.code = CompletedCacheReadCode::absent};
    }
    return {.code = CompletedCacheReadCode::found, .entry = entry->second.entry};
  }

  [[nodiscard]] CompletedCacheList list_completed(
      ControlledCacheRoot const& root) override {
    auto const found = roots.find(root.id);
    if (found == roots.end() || !found->second.available) {
      return {.code = CompletedCacheReadCode::root_unavailable,
              .detail = "cache root is unavailable"};
    }
    CompletedCacheList result{.code = CompletedCacheReadCode::found};
    for (auto const& [_, entry] : found->second.completed) {
      result.entries.push_back(entry.entry);
    }
    return result;
  }

  [[nodiscard]] CacheWriteBegin begin_write(
      ControlledCacheRoot const& root, CacheAsset const& asset) override {
    auto found = roots.find(root.id);
    if (found == roots.end() || !found->second.available) {
      return {.code = CacheWriteBeginCode::root_unavailable,
              .detail = "cache root is unavailable"};
    }
    if (asset.expected_bytes.has_value() && !found->second.free_bytes.has_value()) {
      return {.code = CacheWriteBeginCode::failed,
              .detail = "cache root free space is unknown"};
    }
    if (asset.expected_bytes.has_value() &&
        *found->second.free_bytes < *asset.expected_bytes) {
      return {.code = CacheWriteBeginCode::insufficient_space,
              .detail = "cache root has insufficient space"};
    }
    auto const lock = lock_key(root.id, asset.identity);
    if (!locks.insert(lock).second) {
      return {.code = CacheWriteBeginCode::busy,
              .detail = "same asset temporary file is locked"};
    }
    found->second.partials.insert_or_assign(
        asset.identity.stable_key(), Root::Partial{.asset = asset});
    return {.code = CacheWriteBeginCode::acquired,
            .session = std::make_unique<Session>(*this, root.id, asset)};
  }

  [[nodiscard]] CacheStorageCleanupResult clean_orphaned_partials(
      ControlledCacheRoot const& root) override {
    auto found = roots.find(root.id);
    if (found == roots.end() || !found->second.available) {
      return {.code = CacheStorageCleanupCode::root_unavailable,
              .detail = "cache root is unavailable"};
    }
    std::size_t removed{};
    for (auto iterator = found->second.partials.begin();
         iterator != found->second.partials.end();) {
      auto const lock = root.id + ":" + iterator->first;
      if (locks.contains(lock)) {
        ++iterator;
        continue;
      }
      iterator = found->second.partials.erase(iterator);
      ++removed;
    }
    return {.code = CacheStorageCleanupCode::succeeded,
            .removed_partial_count = removed};
  }

  [[nodiscard]] CacheStorageRemovalResult remove_completed(
      ControlledCacheRoot const& root,
      CacheAssetIdentity const& identity) override {
    auto found = roots.find(root.id);
    if (found == roots.end() || !found->second.available) {
      return {.code = CacheStorageRemovalCode::root_unavailable,
              .detail = "cache root is unavailable"};
    }
    auto const removed = found->second.completed.erase(identity.stable_key());
    return {.code = removed == 0 ? CacheStorageRemovalCode::absent
                                 : CacheStorageRemovalCode::removed};
  }

  void define_root(ControlledCacheRoot const& root) {
    roots.insert_or_assign(root.id, Root{});
  }

  void define_root(ControlledCacheRoot const& root, Root state) {
    roots.insert_or_assign(root.id, std::move(state));
  }

  void seed_completed(ControlledCacheRoot const& root,
                      CacheAssetIdentity const& identity,
                      WallClockTime completed_at,
                      std::string_view data = "seed") {
    auto& target = roots[root.id];
    target.completed.insert_or_assign(
        identity.stable_key(),
        Root::Stored{.entry = {.identity = identity,
                                .byte_count = data.size(),
                                .completed_at = completed_at},
                     .data = bytes(data)});
  }

  void seed_orphaned_partial(ControlledCacheRoot const& root,
                             CacheAsset const& asset) {
    roots[root.id].partials.insert_or_assign(
        asset.identity.stable_key(), Root::Partial{.asset = asset});
  }

  [[nodiscard]] bool has_partial(ControlledCacheRoot const& root,
                                 CacheAssetIdentity const& identity) const {
    auto const found = roots.find(root.id);
    return found != roots.end() &&
           found->second.partials.contains(identity.stable_key());
  }

  [[nodiscard]] std::size_t partial_bytes(ControlledCacheRoot const& root,
                                          CacheAssetIdentity const& identity) const {
    auto const* value = partial(root.id, identity);
    return value == nullptr ? 0U : value->data.size();
  }

  [[nodiscard]] bool has_completed(ControlledCacheRoot const& root,
                                   CacheAssetIdentity const& identity) const {
    auto const found = roots.find(root.id);
    return found != roots.end() &&
           found->second.completed.contains(identity.stable_key());
  }

  [[nodiscard]] std::vector<std::byte> completed_bytes(
      ControlledCacheRoot const& root, CacheAssetIdentity const& identity) const {
    auto const found = roots.find(root.id);
    if (found == roots.end()) {
      return {};
    }
    auto const entry = found->second.completed.find(identity.stable_key());
    return entry == found->second.completed.end() ? std::vector<std::byte>{}
                                                   : entry->second.data;
  }

  std::map<std::string, Root> roots;

 private:
  std::set<std::string> locks;

  [[nodiscard]] std::string lock_key(std::string_view root,
                                     CacheAssetIdentity const& identity) const {
    return std::string{root} + ":" + identity.stable_key();
  }

  [[nodiscard]] Root::Partial* partial(
      std::string const& root, CacheAssetIdentity const& identity) {
    auto found = roots.find(root);
    if (found == roots.end()) {
      return nullptr;
    }
    auto entry = found->second.partials.find(identity.stable_key());
    return entry == found->second.partials.end() ? nullptr
                                                  : std::addressof(entry->second);
  }

  [[nodiscard]] Root::Partial const* partial(
      std::string const& root, CacheAssetIdentity const& identity) const {
    return const_cast<InMemoryCacheStorage*>(this)->partial(root, identity);
  }
};

struct DownloadScript final {
  ControlledDownloadCode code{ControlledDownloadCode::failed};
  std::string payload;
  std::string detail;
};

class ScriptedDownloader final : public ControlledPackageDownloader {
 public:
  [[nodiscard]] ControlledDownloadResult transfer(
      ControlledDownloadRequest const& request) override {
    requests.push_back(request);
    if (scripts.empty()) {
      return {.detail = "unexpected download request"};
    }
    auto script = std::move(scripts.front());
    scripts.erase(scripts.begin());
    return {.code = script.code,
            .bytes = bytes(script.payload),
            .detail = std::move(script.detail)};
  }

  std::vector<DownloadScript> scripts;
  std::vector<ControlledDownloadRequest> requests;
};

[[nodiscard]] ControlledCacheRoot root(std::string id = "system-cache") {
  return {.kind = CacheLocationKind::system_directory, .id = std::move(id)};
}

[[nodiscard]] CacheAsset asset(std::string software_id, std::string version,
                               CacheArchitecture architecture,
                               std::string source_identity,
                               CacheAssetKind kind = CacheAssetKind::full_package,
                               bool resume_supported = true,
                               std::optional<std::uint64_t> expected =
                                   std::nullopt) {
  return {.identity = {.software_id = std::move(software_id),
                       .version = std::move(version),
                       .architecture = architecture,
                       .source_identity = std::move(source_identity)},
          .kind = kind,
          .resume_supported = resume_supported,
          .expected_bytes = expected};
}

[[nodiscard]] OfflinePackageCacheService service(
    InMemoryCacheStorage& storage, ScriptedDownloader& downloader,
    FixtureNetwork const& network, FixedClock const& clock,
    ControlledCacheRoot selected_root,
    CacheRetentionPolicy retention = CacheRetentionPolicy::retain_seven_days) {
  return OfflinePackageCacheService{storage, downloader, network, clock,
                                    std::move(selected_root), retention};
}

[[nodiscard]] cache::OfflinePackageItemSnapshot const* item_for(
    cache::OfflinePackageCacheSnapshot const& snapshot,
    CacheAssetIdentity const& identity) {
  auto const found = std::ranges::find_if(
      snapshot.items, [&](cache::OfflinePackageItemSnapshot const& item) {
        return item.asset.identity == identity;
      });
  return found == snapshot.items.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] bool offline_availability_and_identity_are_fail_closed() {
  InMemoryCacheStorage storage;
  auto const selected_root = root();
  storage.define_root(selected_root);
  ScriptedDownloader downloader;
  FixtureNetwork network;
  network.online = false;
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'000}}};
  auto cache_service = service(storage, downloader, network, clock, selected_root);

  auto const full = asset("editor", "1.0.0", CacheArchitecture::x64,
                          "source-a");
  auto const online = asset("bootstrap", "2.0.0", CacheArchitecture::x64,
                            "source-a", CacheAssetKind::online_installer);
  auto const managed = asset("store-app", "3.0.0", CacheArchitecture::x64,
                             "store-source", CacheAssetKind::managed_source);
  auto const unsupported = asset("future-app", "1.0.0", CacheArchitecture::x64,
                                 "source-a", CacheAssetKind::unsupported);
  auto const built_in = asset("built-in", "4.0.0", CacheArchitecture::arm64,
                              "edition-resource");
  auto const different_source = asset("editor", "1.0.0", CacheArchitecture::x64,
                                      "source-b");
  auto const different_architecture = asset("editor", "1.0.0",
                                            CacheArchitecture::arm64, "source-a");
  cache_service.synchronize_assets(
      {full, online, managed, unsupported, built_in, different_source,
       different_architecture},
      {{.asset = built_in}});
  storage.seed_completed(selected_root, full.identity, clock.now(), "full");
  storage.seed_completed(selected_root, online.identity, clock.now(), "online");

  auto const snapshot = cache_service.snapshot();
  auto const* full_item = item_for(snapshot, full.identity);
  auto const* online_item = item_for(snapshot, online.identity);
  auto const* managed_item = item_for(snapshot, managed.identity);
  auto const* unsupported_item = item_for(snapshot, unsupported.identity);
  auto const* built_in_item = item_for(snapshot, built_in.identity);
  auto const* different_source_item = item_for(snapshot, different_source.identity);
  auto const* different_architecture_item =
      item_for(snapshot, different_architecture.identity);

  bool passed = true;
  passed &= expect(full_item != nullptr && full_item->cache_present &&
                       full_item->availability ==
                           OfflinePackageAvailability::cached_available,
                   "an exact completed full package must be offline available");
  passed &= expect(online_item != nullptr && online_item->cache_present &&
                       online_item->availability ==
                           OfflinePackageAvailability::online_installer_requires_network,
                   "a cached online installer must still require network");
  passed &= expect(managed_item != nullptr &&
                       managed_item->availability ==
                           OfflinePackageAvailability::managed_source_requires_network,
                   "managed sources must not manufacture cache files");
  passed &= expect(unsupported_item != nullptr &&
                       unsupported_item->availability ==
                           OfflinePackageAvailability::unsupported,
                   "unsupported sources must stay explicit");
  passed &= expect(built_in_item != nullptr &&
                       built_in_item->availability ==
                           OfflinePackageAvailability::built_in_available,
                   "a bundled full package must be distinct from downloaded cache");
  passed &= expect(different_source_item != nullptr &&
                       !different_source_item->cache_present &&
                       different_source_item->availability ==
                           OfflinePackageAvailability::waiting_for_network,
                   "a different source identity must not reuse another source cache");
  passed &= expect(different_architecture_item != nullptr &&
                       !different_architecture_item->cache_present,
                   "a different package architecture must not reuse another cache");
  return passed;
}

[[nodiscard]] bool concurrent_writes_use_temporary_state_and_cleanup() {
  InMemoryCacheStorage storage;
  auto const selected_root = root();
  storage.define_root(selected_root);
  FixtureNetwork network;
  FixedClock clock{WallClockTime{std::chrono::milliseconds{2'000}}};
  auto const package = asset("editor", "1.0.0", CacheArchitecture::x64,
                             "source-a", CacheAssetKind::full_package, true, 4);

  ScriptedDownloader first_downloader;
  first_downloader.scripts = {{.code = ControlledDownloadCode::paused,
                               .payload = "ab",
                               .detail = "paused by user"}};
  auto first = service(storage, first_downloader, network, clock, selected_root);
  first.synchronize_assets({package});
  auto const paused = first.download(package.identity);

  ScriptedDownloader second_downloader;
  second_downloader.scripts = {{.code = ControlledDownloadCode::completed,
                                .payload = "abcd"}};
  auto second = service(storage, second_downloader, network, clock, selected_root);
  second.synchronize_assets({package});
  auto const busy = second.download(package.identity);

  bool passed = true;
  passed &= expect(paused.code == CacheActionCode::paused &&
                       storage.has_partial(selected_root, package.identity) &&
                       storage.partial_bytes(selected_root, package.identity) == 2 &&
                       !storage.has_completed(selected_root, package.identity),
                   "a pause must retain only a locked temporary file without a completion marker");
  passed &= expect(busy.code == CacheActionCode::busy &&
                       second_downloader.requests.empty(),
                   "a concurrent same-identity writer must stop before transfer");

  first.shutdown();
  passed &= expect(!storage.has_partial(selected_root, package.identity),
                   "orderly shutdown must clear paused temporary bytes");
  auto const completed = second.download(package.identity);
  passed &= expect(completed.code == CacheActionCode::completed &&
                       storage.has_completed(selected_root, package.identity),
                   "a released same-asset lock must allow a new complete write");
  return passed;
}

[[nodiscard]] bool pause_resume_and_restart_rules_are_explicit() {
  InMemoryCacheStorage storage;
  auto const selected_root = root();
  storage.define_root(selected_root);
  FixtureNetwork network;
  FixedClock clock{WallClockTime{std::chrono::milliseconds{3'000}}};

  auto const resumable = asset("resumable", "1.0.0", CacheArchitecture::x64,
                               "source-a", CacheAssetKind::full_package, true, 4);
  ScriptedDownloader resumable_downloader;
  resumable_downloader.scripts = {
      {.code = ControlledDownloadCode::paused, .payload = "ab"},
      {.code = ControlledDownloadCode::completed, .payload = "cd"},
  };
  auto resumable_service =
      service(storage, resumable_downloader, network, clock, selected_root);
  resumable_service.synchronize_assets({resumable});
  auto const paused = resumable_service.download(resumable.identity);
  auto const resumed = resumable_service.resume(resumable.identity);

  auto const non_resumable = asset("restart", "1.0.0", CacheArchitecture::x64,
                                   "source-a", CacheAssetKind::full_package,
                                   false, 4);
  ScriptedDownloader restart_downloader;
  restart_downloader.scripts = {
      {.code = ControlledDownloadCode::paused, .payload = "ab"},
      {.code = ControlledDownloadCode::completed, .payload = "abcd"},
  };
  auto restart_service =
      service(storage, restart_downloader, network, clock, selected_root);
  restart_service.synchronize_assets({non_resumable});
  auto const pause_restart = restart_service.download(non_resumable.identity);
  auto const restarted = restart_service.resume(non_resumable.identity);

  bool passed = true;
  passed &= expect(paused.code == CacheActionCode::paused &&
                       resumed.code == CacheActionCode::completed &&
                       resumable_downloader.requests.size() == 2 &&
                       resumable_downloader.requests[1].resume_from_bytes == 2 &&
                       storage.completed_bytes(selected_root, resumable.identity) ==
                           bytes("abcd"),
                   "a supported paused transfer must resume its current-run temporary bytes");
  passed &= expect(pause_restart.code == CacheActionCode::paused &&
                       restarted.code == CacheActionCode::restarted_from_zero &&
                       restart_downloader.requests.size() == 2 &&
                       restart_downloader.requests[1].resume_from_bytes == 0 &&
                       storage.completed_bytes(selected_root, non_resumable.identity) ==
                           bytes("abcd"),
                   "an unsupported continuation must discard partial bytes and report a restart");
  return passed;
}

[[nodiscard]] bool two_failures_retry_then_manual_retry_is_available() {
  InMemoryCacheStorage storage;
  auto const selected_root = root();
  storage.define_root(selected_root);
  FixtureNetwork network;
  FixedClock clock{WallClockTime{std::chrono::milliseconds{4'000}}};
  auto const package = asset("retry", "1.0.0", CacheArchitecture::x64,
                             "source-a", CacheAssetKind::full_package, true, 2);
  ScriptedDownloader downloader;
  downloader.scripts = {
      {.code = ControlledDownloadCode::failed, .detail = "first failure"},
      {.code = ControlledDownloadCode::failed, .detail = "second failure"},
      {.code = ControlledDownloadCode::failed, .detail = "third failure"},
      {.code = ControlledDownloadCode::completed, .payload = "ok"},
  };
  auto cache_service = service(storage, downloader, network, clock, selected_root);
  cache_service.synchronize_assets({package});
  auto const failed = cache_service.download(package.identity);
  auto const after_failed = cache_service.snapshot();
  auto const* failed_item = item_for(after_failed, package.identity);
  auto const requests_after_automatic_retries = downloader.requests.size();
  auto const manual = cache_service.retry(package.identity);

  return expect(failed.code == CacheActionCode::failed &&
                    requests_after_automatic_retries == 3 &&
                    failed_item != nullptr &&
                    failed_item->automatic_attempts ==
                        OfflinePackageCacheService::kAutomaticFailureRetries &&
                    failed_item->manual_retry_available &&
                    manual.code == CacheActionCode::completed &&
                    downloader.requests.size() == 4,
                "a failed download must retry exactly twice then permit one-item retry");
}

[[nodiscard]] bool cleanup_and_location_failures_are_conservative() {
  InMemoryCacheStorage storage;
  auto const system_root = root("system-cache");
  auto const external_root = ControlledCacheRoot{
      .kind = CacheLocationKind::removable_media,
      .id = "usb-cache",
  };
  storage.define_root(system_root);
  storage.define_root(external_root,
                      {.available = false,
                       .free_bytes = std::nullopt,
                       .detail = "removable media is unavailable"});
  FixtureNetwork network;
  FixedClock clock{WallClockTime{std::chrono::milliseconds{20LL * 24LL * 60LL * 60LL * 1000LL}}};
  auto const old = asset("old", "1.0.0", CacheArchitecture::x64, "source-a");
  auto const fresh = asset("fresh", "1.0.0", CacheArchitecture::x64, "source-a");
  auto const sized = asset("sized", "1.0.0", CacheArchitecture::x64,
                           "source-a", CacheAssetKind::full_package, true, 100);
  storage.seed_completed(system_root, old.identity,
                         clock.now() - std::chrono::days{8}, "old");
  storage.seed_completed(system_root, fresh.identity,
                         clock.now() - std::chrono::days{2}, "fresh");
  storage.seed_orphaned_partial(system_root, sized);

  ScriptedDownloader downloader;
  auto cache_service = service(storage, downloader, network, clock, system_root);
  cache_service.synchronize_assets({old, fresh, sized});
  auto const cleaned = cache_service.clean();
  auto const switched = cache_service.configure_location(external_root);
  auto const unavailable = cache_service.download(sized.identity);

  storage.roots[external_root.id].available = true;
  storage.roots[external_root.id].free_bytes = std::nullopt;
  auto const unknown_space = cache_service.download(sized.identity);
  storage.roots[external_root.id].free_bytes = 99;
  auto const insufficient = cache_service.download(sized.identity);
  storage.roots[external_root.id].free_bytes = 1000;
  storage.roots[external_root.id].available = false;
  auto const removed_media = cache_service.download(sized.identity);
  auto const snapshot = cache_service.snapshot();

  bool passed = true;
  passed &= expect(cleaned.removed_completed_count == 1 &&
                       cleaned.removed_partial_count == 1 &&
                       !storage.has_completed(system_root, old.identity) &&
                       storage.has_completed(system_root, fresh.identity) &&
                       !storage.has_partial(system_root, sized.identity),
                   "retention cleanup must remove eligible completed cache and orphaned partials only");
  passed &= expect(switched.code == cache::CacheLocationConfigureCode::selected_unavailable &&
                       unavailable.code == CacheActionCode::location_unavailable,
                   "an unavailable selected removable location must not silently fall back");
  passed &= expect(unknown_space.code == CacheActionCode::space_unknown &&
                       insufficient.code == CacheActionCode::insufficient_space &&
                       removed_media.code == CacheActionCode::location_unavailable &&
                       snapshot.location_state == CacheLocationState::unavailable,
                   "unknown space, insufficient space, and removed media must fail conservatively");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= offline_availability_and_identity_are_fail_closed();
  passed &= concurrent_writes_use_temporary_state_and_cleanup();
  passed &= pause_resume_and_restart_rules_are_explicit();
  passed &= two_failures_retry_then_manual_retry_is_available();
  passed &= cleanup_and_location_failures_are_conservative();

  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "offline package cache contract passed\n";
  return EXIT_SUCCESS;
}
