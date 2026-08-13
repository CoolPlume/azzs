#include "azzs/application/offline_package_cache.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::application::offline_package_cache {
namespace {

using cache_domain::CacheArchitecture;
using cache_domain::CacheAssetKind;

[[nodiscard]] CacheArchitecture cache_architecture(
    domain::architecture_selection::PackageArchitecture architecture) noexcept {
  using SourceArchitecture =
      domain::architecture_selection::PackageArchitecture;
  switch (architecture) {
    case SourceArchitecture::x64:
      return CacheArchitecture::x64;
    case SourceArchitecture::arm64:
      return CacheArchitecture::arm64;
    case SourceArchitecture::architecture_independent:
      return CacheArchitecture::architecture_independent;
    case SourceArchitecture::unknown:
      return CacheArchitecture::unknown;
  }
  return CacheArchitecture::unknown;
}

[[nodiscard]] CacheAssetKind cache_asset_kind(
    selection_domain::PackageType package_type) noexcept {
  switch (package_type) {
    case selection_domain::PackageType::full_package:
      return CacheAssetKind::full_package;
    case selection_domain::PackageType::online_installer:
      return CacheAssetKind::online_installer;
    case selection_domain::PackageType::external_handoff:
      return CacheAssetKind::managed_source;
  }
  return CacheAssetKind::unsupported;
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

[[nodiscard]] std::string source_identity(
    selection_domain::ResolvedSourceSnapshot const& source) {
  auto hash = fnv1a(source.actual_address);
  hash = fnv1a(std::string_view{"\0", 1}, hash);
  hash = fnv1a(source.hosting_mechanism, hash);
  hash = fnv1a(std::string_view{"\0", 1}, hash);
  hash = fnv1a(source.capability_version, hash);
  return "source-" + hex(hash);
}

[[nodiscard]] bool checked_sum(std::uint64_t left, std::uint64_t right,
                               std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

}  // namespace

struct OfflinePackageCacheService::Session final {
  CacheAssetIdentity identity;
  std::unique_ptr<PackageCacheWriteSession> writer;
  PackageCacheSessionState state{PackageCacheSessionState::idle};
  bool continuation_eligible{false};
  bool manual_retry_available{false};
  std::size_t automatic_attempts{};
  std::string detail;
};

OfflinePackageCacheService::OfflinePackageCacheService(
    PackageCacheStorage& storage, ControlledPackageDownloader& downloader,
    PackageCacheNetworkObserver const& network, Clock const& clock,
    ControlledCacheRoot initial_root, CacheRetentionPolicy retention)
    : storage_(storage),
      downloader_(downloader),
      network_(network),
      clock_(clock),
      selected_root_(std::move(initial_root)),
      retention_(retention) {}

OfflinePackageCacheService::~OfflinePackageCacheService() { shutdown(); }

void OfflinePackageCacheService::synchronize_assets(
    std::vector<CacheAsset> assets,
    std::vector<BuiltInPackageResource> built_in) {
  std::erase_if(assets, [](CacheAsset const& asset) { return !asset.valid(); });
  std::ranges::sort(assets, {}, [](CacheAsset const& asset) {
    return asset.identity;
  });
  assets.erase(std::unique(assets.begin(), assets.end(),
                           [](CacheAsset const& left,
                              CacheAsset const& right) {
                             return left.identity == right.identity;
                           }),
               assets.end());

  std::erase_if(built_in, [](BuiltInPackageResource const& resource) {
    return !resource.valid();
  });
  std::ranges::sort(built_in, {}, [](BuiltInPackageResource const& resource) {
    return resource.asset.identity;
  });
  built_in.erase(std::unique(built_in.begin(), built_in.end(),
                             [](BuiltInPackageResource const& left,
                                BuiltInPackageResource const& right) {
                               return left.asset.identity ==
                                      right.asset.identity;
                             }),
                 built_in.end());

  for (auto index = sessions_.size(); index > 0; --index) {
    auto const& session = sessions_[index - 1U];
    auto const found = std::ranges::find(assets, session.identity,
                                         &CacheAsset::identity);
    if (found == assets.end()) {
      abandon_session(index - 1U);
      sessions_.erase(sessions_.begin() +
                      static_cast<std::ptrdiff_t>(index - 1U));
    }
  }
  assets_ = std::move(assets);
  built_in_ = std::move(built_in);
}

CacheLocationConfigureResult OfflinePackageCacheService::configure_location(
    ControlledCacheRoot root) {
  if (!root.valid()) {
    return {.code = CacheLocationConfigureCode::rejected,
            .snapshot = snapshot(),
            .detail = "cache location identifier is invalid"};
  }

  for (auto index = sessions_.size(); index > 0; --index) {
    abandon_session(index - 1U);
  }
  sessions_.clear();
  selected_root_ = std::move(root);
  auto result = CacheLocationConfigureResult{
      .snapshot = snapshot(),
  };
  result.code = result.snapshot.location_state == CacheLocationState::available
                    ? CacheLocationConfigureCode::selected
                    : CacheLocationConfigureCode::selected_unavailable;
  result.detail = result.snapshot.location_detail;
  return result;
}

void OfflinePackageCacheService::set_retention(
    CacheRetentionPolicy retention) noexcept {
  retention_ = retention;
}

OfflinePackageCacheSnapshot OfflinePackageCacheService::snapshot() {
  auto const root = selected_root_.valid()
                        ? storage_.observe_root(selected_root_)
                        : CacheRootObservation{.detail = "cache location identifier is invalid"};
  OfflinePackageCacheSnapshot result{
      .selected_root = selected_root_,
      .location_state = current_location_state(root),
      .retention = retention_,
      .network_available = network_.available(),
      .location_detail = root.detail,
  };
  result.items.reserve(assets_.size());
  for (auto const& asset : assets_) {
    result.items.push_back(item_snapshot(asset));
  }
  return result;
}

CacheActionResult OfflinePackageCacheService::download(
    CacheAssetIdentity const& identity) {
  auto const index = asset_index(identity);
  if (!index.has_value()) {
    return action_for_missing_asset(identity);
  }
  return run_download(assets_[*index], false, false);
}

CacheActionResult OfflinePackageCacheService::resume(
    CacheAssetIdentity const& identity) {
  auto const index = asset_index(identity);
  if (!index.has_value()) {
    return action_for_missing_asset(identity);
  }
  return run_download(assets_[*index], false, true);
}

CacheActionResult OfflinePackageCacheService::retry(
    CacheAssetIdentity const& identity) {
  auto const asset = asset_index(identity);
  auto const session = session_index(identity);
  if (!asset.has_value() || !session.has_value() ||
      !sessions_[*session].manual_retry_available) {
    return {.code = asset.has_value() ? CacheActionCode::retry_not_available
                                      : CacheActionCode::asset_not_current,
            .detail = "manual retry is not available for this asset"};
  }
  return run_download(assets_[*asset], true, false);
}

CacheCleanupResult OfflinePackageCacheService::clean() {
  auto const root = selected_root_.valid()
                        ? storage_.observe_root(selected_root_)
                        : CacheRootObservation{};
  if (!root.available) {
    return {.detail = root.detail.empty() ? "cache location is unavailable"
                                          : root.detail};
  }

  auto partial = storage_.clean_orphaned_partials(selected_root_);
  CacheCleanupResult result{
      .removed_partial_count = partial.removed_partial_count,
      .detail = partial.detail,
  };
  if (partial.code != CacheStorageCleanupCode::succeeded) {
    return result;
  }
  if (retention_ == CacheRetentionPolicy::retain_indefinitely) {
    return result;
  }

  auto entries = storage_.list_completed(selected_root_);
  if (entries.code != CompletedCacheReadCode::found) {
    result.detail = entries.detail;
    return result;
  }
  auto const now = clock_.now();
  for (auto const& entry : entries.entries) {
    auto remove = retention_ == CacheRetentionPolicy::delete_immediately;
    if (!remove) {
      auto const retention_days =
          retention_ == CacheRetentionPolicy::retain_seven_days ? 7 : 30;
      remove = now - entry.completed_at >=
               std::chrono::days{retention_days};
    }
    if (!remove) {
      continue;
    }
    auto const erased = storage_.remove_completed(selected_root_, entry.identity);
    if (erased.code == CacheStorageRemovalCode::removed) {
      ++result.removed_completed_count;
    } else if (erased.code == CacheStorageRemovalCode::root_unavailable ||
               erased.code == CacheStorageRemovalCode::failed) {
      result.detail = erased.detail;
      return result;
    }
  }
  return result;
}

void OfflinePackageCacheService::shutdown() noexcept {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  for (auto index = sessions_.size(); index > 0; --index) {
    abandon_session(index - 1U);
  }
  sessions_.clear();
}

CacheActionResult OfflinePackageCacheService::action_for_missing_asset(
    CacheAssetIdentity const&) const {
  return {.code = CacheActionCode::asset_not_current,
          .detail = "asset is absent from the current controlled source snapshot"};
}

OfflinePackageItemSnapshot OfflinePackageCacheService::item_snapshot(
    CacheAsset const& asset) {
  auto completed = selected_root_.valid()
                       ? storage_.read_completed(selected_root_, asset.identity)
                       : CompletedCacheRead{};
  auto const cache_present =
      completed.code == CompletedCacheReadCode::found &&
      completed.entry.has_value() &&
      (!asset.expected_bytes.has_value() ||
       completed.entry->byte_count == *asset.expected_bytes);
  OfflinePackageItemSnapshot result{
      .asset = asset,
      .availability = availability_for(asset, cache_present),
      .cache_present = cache_present,
      .cached_bytes = cache_present && completed.entry.has_value()
                          ? std::optional{completed.entry->byte_count}
                          : std::nullopt,
      .detail = completed.detail,
  };
  if (completed.code == CompletedCacheReadCode::found && !cache_present) {
    result.detail = "completed cache byte count does not match the controlled asset";
  }
  if (built_in_available(asset.identity)) {
    result.availability = OfflinePackageAvailability::built_in_available;
  }
  if (auto const session = session_index(asset.identity); session.has_value()) {
    auto const& active = sessions_[*session];
    result.session = active.state;
    result.continuation_eligible = active.continuation_eligible;
    result.manual_retry_available = active.manual_retry_available;
    result.automatic_attempts = active.automatic_attempts;
    if (!active.detail.empty()) {
      result.detail = active.detail;
    }
  }
  return result;
}

OfflinePackageAvailability OfflinePackageCacheService::availability_for(
    CacheAsset const& asset, bool cache_present) const noexcept {
  if (asset.kind == CacheAssetKind::unsupported) {
    return OfflinePackageAvailability::unsupported;
  }
  if (asset.kind == CacheAssetKind::managed_source) {
    return OfflinePackageAvailability::managed_source_requires_network;
  }
  if (asset.kind == CacheAssetKind::online_only) {
    return OfflinePackageAvailability::waiting_for_network;
  }
  if (asset.kind == CacheAssetKind::online_installer) {
    return OfflinePackageAvailability::online_installer_requires_network;
  }
  if (cache_present) {
    return OfflinePackageAvailability::cached_available;
  }
  return network_.available() ? OfflinePackageAvailability::ready_to_download
                              : OfflinePackageAvailability::waiting_for_network;
}

CacheActionResult OfflinePackageCacheService::run_download(
    CacheAsset const& asset, bool manual_retry, bool resume) {
  if (shutdown_) {
    return {.code = CacheActionCode::interrupted,
            .detail = "cache service is shutting down"};
  }
  if (!asset.cacheable()) {
    return {.code = CacheActionCode::unsupported,
            .item = item_snapshot(asset),
            .detail = "this controlled source does not produce a cache file"};
  }
  if (built_in_available(asset.identity)) {
    return {.code = CacheActionCode::already_available,
            .item = item_snapshot(asset),
            .detail = "the selected artifact is built into this edition"};
  }
  auto const completed = storage_.read_completed(selected_root_, asset.identity);
  if (completed.code == CompletedCacheReadCode::found &&
      completed.entry.has_value() &&
      (!asset.expected_bytes.has_value() ||
       completed.entry->byte_count == *asset.expected_bytes)) {
    return {.code = CacheActionCode::already_available,
            .item = item_snapshot(asset),
            .detail = "a completed cache entry already matches this identity"};
  }
  if (!network_.available()) {
    return {.code = CacheActionCode::waiting_for_network,
            .item = item_snapshot(asset),
            .detail = "network is unavailable"};
  }
  auto const root = selected_root_.valid()
                        ? storage_.observe_root(selected_root_)
                        : CacheRootObservation{};
  if (!root.available) {
    return {.code = CacheActionCode::location_unavailable,
            .item = item_snapshot(asset),
            .detail = root.detail.empty() ? "cache location is unavailable"
                                           : root.detail};
  }
  if (asset.expected_bytes.has_value() && !root.free_bytes.has_value()) {
    return {.code = CacheActionCode::space_unknown,
            .item = item_snapshot(asset),
            .detail = "cache location free space cannot be observed"};
  }
  if (asset.expected_bytes.has_value() &&
      *root.free_bytes < *asset.expected_bytes) {
    return {.code = CacheActionCode::insufficient_space,
            .item = item_snapshot(asset),
            .detail = "cache location does not have enough free space"};
  }

  auto const existing = session_index(asset.identity);
  if (resume && existing.has_value()) {
    auto& session = sessions_[*existing];
    if (!session.continuation_eligible || !session.writer) {
      abandon_session(*existing);
      sessions_.erase(sessions_.begin() +
                      static_cast<std::ptrdiff_t>(*existing));
      return begin_transfer(asset, manual_retry, false, true);
    }
    return begin_transfer(asset, manual_retry, true, false);
  }
  if (existing.has_value()) {
    auto const state = sessions_[*existing].state;
    if (state == PackageCacheSessionState::downloading) {
      return {.code = CacheActionCode::busy,
              .item = item_snapshot(asset),
              .detail = "this asset already has an active write session"};
    }
    abandon_session(*existing);
    sessions_.erase(sessions_.begin() +
                    static_cast<std::ptrdiff_t>(*existing));
  }
  return begin_transfer(asset, manual_retry, false, false);
}

CacheActionResult OfflinePackageCacheService::begin_transfer(
    CacheAsset const& asset, bool manual_retry, bool resume,
    bool restarted_from_zero, std::size_t automatic_attempts) {
  auto session_position = session_index(asset.identity);
  if (!session_position.has_value()) {
    auto started = storage_.begin_write(selected_root_, asset);
    if (started.code != CacheWriteBeginCode::acquired || !started.session) {
      CacheActionCode code = CacheActionCode::failed;
      switch (started.code) {
        case CacheWriteBeginCode::busy:
          code = CacheActionCode::busy;
          break;
        case CacheWriteBeginCode::root_unavailable:
          code = CacheActionCode::location_unavailable;
          break;
        case CacheWriteBeginCode::insufficient_space:
          code = CacheActionCode::insufficient_space;
          break;
        case CacheWriteBeginCode::failed:
        case CacheWriteBeginCode::acquired:
          break;
      }
      return {.code = code, .item = item_snapshot(asset), .detail = started.detail};
    }
    sessions_.push_back({.identity = asset.identity,
                         .writer = std::move(started.session),
                         .state = PackageCacheSessionState::idle,
                         .automatic_attempts = automatic_attempts});
    session_position = sessions_.size() - 1U;
  }

  auto& session = sessions_[*session_position];
  session.state = PackageCacheSessionState::downloading;
  session.continuation_eligible = false;
  session.manual_retry_available = false;
  if (manual_retry) {
    session.automatic_attempts = 0;
  }

  auto const resume_from = resume && session.writer ? session.writer->received_bytes() : 0;
  auto const transfer = downloader_.transfer({
      .asset = asset,
      .cache_root = selected_root_,
      .resume_from_bytes = resume_from,
  });

  std::uint64_t resulting_bytes{};
  if (!checked_sum(resume_from, transfer.bytes.size(), resulting_bytes) ||
      (asset.expected_bytes.has_value() && resulting_bytes > *asset.expected_bytes)) {
    session.detail = "download byte count is inconsistent with the controlled asset";
    session.state = PackageCacheSessionState::failed;
    session.manual_retry_available = true;
    abandon_session(*session_position);
    return {.code = CacheActionCode::failed,
            .item = item_snapshot(asset),
            .detail = session.detail};
  }
  if (!transfer.bytes.empty()) {
    auto written = session.writer->append(transfer.bytes);
    if (written.code != CacheWriteCode::succeeded) {
      session.detail = written.detail;
      session.state = PackageCacheSessionState::failed;
      session.manual_retry_available = true;
      abandon_session(*session_position);
      return {.code = CacheActionCode::failed,
              .item = item_snapshot(asset),
              .detail = session.detail};
    }
  }

  session.detail = transfer.detail;
  switch (transfer.code) {
    case ControlledDownloadCode::completed: {
      if (asset.expected_bytes.has_value() && resulting_bytes != *asset.expected_bytes) {
        session.detail = "completed byte count does not match the controlled asset";
        session.state = PackageCacheSessionState::failed;
        session.manual_retry_available = true;
        abandon_session(*session_position);
        return {.code = CacheActionCode::failed,
                .item = item_snapshot(asset),
                .detail = session.detail};
      }
      auto completed = session.writer->complete(clock_.now());
      if (completed.code != CacheWriteCode::succeeded) {
        session.detail = completed.detail;
        session.state = PackageCacheSessionState::failed;
        session.manual_retry_available = true;
        abandon_session(*session_position);
        return {.code = CacheActionCode::failed,
                .item = item_snapshot(asset),
                .detail = session.detail};
      }
      session.writer.reset();
      session.state = PackageCacheSessionState::completed;
      session.continuation_eligible = false;
      session.manual_retry_available = false;
      return {.code = restarted_from_zero ? CacheActionCode::restarted_from_zero
                                          : CacheActionCode::completed,
              .item = item_snapshot(asset),
              .detail = restarted_from_zero
                            ? "continuation was unavailable; download restarted from zero"
                            : transfer.detail};
    }
    case ControlledDownloadCode::paused:
      session.state = PackageCacheSessionState::paused;
      session.continuation_eligible = asset.resume_supported;
      return {.code = CacheActionCode::paused,
              .item = item_snapshot(asset),
              .detail = transfer.detail};
    case ControlledDownloadCode::network_unavailable:
      session.state = PackageCacheSessionState::waiting_for_network;
      session.continuation_eligible = asset.resume_supported;
      return {.code = CacheActionCode::waiting_for_network,
              .item = item_snapshot(asset),
              .detail = transfer.detail};
    case ControlledDownloadCode::interrupted:
      abandon_session(*session_position);
      session.state = PackageCacheSessionState::failed;
      session.manual_retry_available = true;
      return {.code = CacheActionCode::interrupted,
              .item = item_snapshot(asset),
              .detail = transfer.detail};
    case ControlledDownloadCode::failed:
      if (session.automatic_attempts < kAutomaticFailureRetries) {
        auto const next_automatic_attempt = session.automatic_attempts + 1U;
        abandon_session(*session_position);
        sessions_.erase(sessions_.begin() +
                        static_cast<std::ptrdiff_t>(*session_position));
        return begin_transfer(asset, false, false, restarted_from_zero,
                              next_automatic_attempt);
      }
      abandon_session(*session_position);
      session.state = PackageCacheSessionState::failed;
      session.manual_retry_available = true;
      return {.code = CacheActionCode::failed,
              .item = item_snapshot(asset),
              .detail = transfer.detail};
  }
  return {.code = CacheActionCode::failed,
          .item = item_snapshot(asset),
          .detail = transfer.detail};
}

std::optional<std::size_t> OfflinePackageCacheService::asset_index(
    CacheAssetIdentity const& identity) const noexcept {
  auto const found = std::ranges::find(assets_, identity, &CacheAsset::identity);
  if (found == assets_.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - assets_.begin());
}

std::optional<std::size_t> OfflinePackageCacheService::session_index(
    CacheAssetIdentity const& identity) const noexcept {
  auto const found = std::ranges::find(sessions_, identity, &Session::identity);
  if (found == sessions_.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - sessions_.begin());
}

bool OfflinePackageCacheService::built_in_available(
    CacheAssetIdentity const& identity) const noexcept {
  return std::ranges::any_of(built_in_, [&](BuiltInPackageResource const& resource) {
    return resource.asset.identity == identity;
  });
}

CacheLocationState OfflinePackageCacheService::current_location_state(
    CacheRootObservation const& observation) const noexcept {
  if (!selected_root_.valid()) {
    return CacheLocationState::invalid;
  }
  return observation.available ? CacheLocationState::available
                               : CacheLocationState::unavailable;
}

void OfflinePackageCacheService::abandon_session(std::size_t index) noexcept {
  if (index >= sessions_.size() || !sessions_[index].writer) {
    return;
  }
  static_cast<void>(sessions_[index].writer->abandon());
  sessions_[index].writer.reset();
  sessions_[index].continuation_eligible = false;
}

std::optional<CacheAsset> make_cache_asset(
    selection_domain::ResolvedSourceSnapshot const& source,
    selection_domain::ResolvedPackage const& package) {
  if (!source.valid() || package.candidate.software_id != source.software_id ||
      package.candidate.version != source.version) {
    return std::nullopt;
  }
  auto const kind = cache_asset_kind(package.package_type);
  auto asset = CacheAsset{
      .identity = {
          .software_id = package.candidate.software_id,
          .version = package.candidate.version,
          .architecture = cache_architecture(package.candidate.architecture),
          .source_identity = source_identity(source),
      },
      .kind = kind,
      .resume_supported = kind == CacheAssetKind::full_package ||
                          kind == CacheAssetKind::online_installer,
  };
  if (!asset.valid()) {
    return std::nullopt;
  }
  return asset;
}

char const* to_string(OfflinePackageAvailability value) noexcept {
  switch (value) {
    case OfflinePackageAvailability::built_in_available:
      return "built-in-available";
    case OfflinePackageAvailability::cached_available:
      return "cached-available";
    case OfflinePackageAvailability::ready_to_download:
      return "ready-to-download";
    case OfflinePackageAvailability::waiting_for_network:
      return "waiting-for-network";
    case OfflinePackageAvailability::online_installer_requires_network:
      return "online-installer-requires-network";
    case OfflinePackageAvailability::managed_source_requires_network:
      return "managed-source-requires-network";
    case OfflinePackageAvailability::unsupported:
      return "unsupported";
  }
  return "unsupported";
}

char const* to_string(PackageCacheSessionState value) noexcept {
  switch (value) {
    case PackageCacheSessionState::idle:
      return "idle";
    case PackageCacheSessionState::downloading:
      return "downloading";
    case PackageCacheSessionState::paused:
      return "paused";
    case PackageCacheSessionState::completed:
      return "completed";
    case PackageCacheSessionState::failed:
      return "failed";
    case PackageCacheSessionState::waiting_for_network:
      return "waiting-for-network";
    case PackageCacheSessionState::location_unavailable:
      return "location-unavailable";
    case PackageCacheSessionState::insufficient_space:
      return "insufficient-space";
    case PackageCacheSessionState::busy:
      return "busy";
    case PackageCacheSessionState::unsupported:
      return "unsupported";
  }
  return "failed";
}

char const* to_string(CacheActionCode value) noexcept {
  switch (value) {
    case CacheActionCode::completed:
      return "completed";
    case CacheActionCode::already_available:
      return "already-available";
    case CacheActionCode::paused:
      return "paused";
    case CacheActionCode::restarted_from_zero:
      return "restarted-from-zero";
    case CacheActionCode::waiting_for_network:
      return "waiting-for-network";
    case CacheActionCode::location_unavailable:
      return "location-unavailable";
    case CacheActionCode::insufficient_space:
      return "insufficient-space";
    case CacheActionCode::space_unknown:
      return "space-unknown";
    case CacheActionCode::busy:
      return "busy";
    case CacheActionCode::unsupported:
      return "unsupported";
    case CacheActionCode::asset_not_current:
      return "asset-not-current";
    case CacheActionCode::retry_not_available:
      return "retry-not-available";
    case CacheActionCode::interrupted:
      return "interrupted";
    case CacheActionCode::failed:
      return "failed";
  }
  return "failed";
}

}  // namespace azzs::application::offline_package_cache
