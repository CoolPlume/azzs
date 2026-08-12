#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/adapters/infrastructure/local_package_cache_storage.hpp"

namespace {

namespace cache = azzs::application::offline_package_cache;
namespace domain_cache = azzs::domain::offline_package_cache;
namespace infrastructure = azzs::adapters::infrastructure;

using domain_cache::CacheArchitecture;
using domain_cache::CacheAsset;
using domain_cache::CacheAssetKind;
using domain_cache::CacheLocationKind;
using domain_cache::ControlledCacheRoot;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "local package cache adapter contract failed: " << message
              << '\n';
  }
  return condition;
}

[[nodiscard]] CacheAsset asset(std::string software_id = "editor") {
  return {.identity = {.software_id = std::move(software_id),
                        .version = "1.2.3",
                        .architecture = CacheArchitecture::x64,
                        .source_identity = "source-a"},
          .kind = CacheAssetKind::full_package,
          .resume_supported = true,
          .expected_bytes = 4};
}

[[nodiscard]] ControlledCacheRoot root(std::string id = "test-root") {
  return {.kind = CacheLocationKind::local_volume, .id = std::move(id)};
}

[[nodiscard]] std::filesystem::path package_path(
    std::filesystem::path const& root_path,
    domain_cache::CacheAssetIdentity const& identity, std::string_view suffix) {
  return root_path / "packages" /
         (identity.stable_key() + std::string{suffix});
}

[[nodiscard]] std::filesystem::path unique_root() {
  auto const ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("azzs-offline-cache-contract-" + std::to_string(ticks));
}

bool adapter_contract() {
  auto const root_id = root();
  auto const root_path = unique_root();
  std::error_code cleanup_error;
  std::filesystem::remove_all(root_path, cleanup_error);
  infrastructure::LocalPackageCacheStorage storage({
      {.root = root_id, .directory = root_path, .create_if_missing = true},
  });

  auto const package = asset();
  auto const identity = package.identity;
  bool passed = true;
  auto const observed = storage.observe_root(root_id);
  passed &= expect(observed.available && observed.free_bytes.has_value(),
                   "registered controlled root must be observable");

  auto first = storage.begin_write(root_id, package);
  if (first.code != cache::CacheWriteBeginCode::acquired) {
    std::cerr << "local package cache adapter diagnostic: first begin code="
              << static_cast<int>(first.code) << " detail=" << first.detail
              << '\n';
  }
  passed &= expect(first.code == cache::CacheWriteBeginCode::acquired &&
                       first.session != nullptr,
                   "first writer must acquire a typed controlled asset lock");
  auto const second = storage.begin_write(root_id, package);
  passed &= expect(second.code == cache::CacheWriteBeginCode::busy,
                   "same identity must be locked across write sessions");
  if (first.session) {
    passed &= expect(first.session->append(std::span<std::byte const>{
                       reinterpret_cast<std::byte const*>("data"), 4})
                           .code == cache::CacheWriteCode::succeeded,
                       "controlled writer must append to its partial file");
    passed &= expect(first.session->complete(
                         azzs::application::WallClockTime{
                             std::chrono::milliseconds{123}})
                           .code == cache::CacheWriteCode::succeeded,
                     "controlled writer must finalize payload and marker");
    first.session.reset();
  }

  auto const complete_marker = package_path(root_path, identity, ".complete");
  auto const payload = package_path(root_path, identity, ".payload");
  auto const partial = package_path(root_path, identity, ".partial");
  passed &= expect(std::filesystem::is_regular_file(complete_marker) &&
                       std::filesystem::is_regular_file(payload) &&
                       !std::filesystem::exists(partial),
                   "completion must publish marker and payload without partial");
  passed &= expect(storage.read_completed(root_id, identity).code ==
                       cache::CompletedCacheReadCode::found,
                   "matching marker and payload must be readable");

  {
    std::ofstream corrupt(complete_marker, std::ios::binary | std::ios::trunc);
    corrupt << "corrupt-marker";
  }
  passed &= expect(storage.read_completed(root_id, identity).code ==
                       cache::CompletedCacheReadCode::absent,
                   "a corrupt completion marker must fail closed");
  std::filesystem::remove(complete_marker, cleanup_error);
  passed &= expect(storage.read_completed(root_id, identity).code ==
                       cache::CompletedCacheReadCode::absent,
                   "payload without completion marker must remain unavailable");

  {
    std::ofstream orphan(partial, std::ios::binary);
    orphan << "orphan";
  }
  auto const cleaned = storage.clean_orphaned_partials(root_id);
  passed &= expect(cleaned.code == cache::CacheStorageCleanupCode::succeeded &&
                       cleaned.removed_partial_count == 1 &&
                       !std::filesystem::exists(partial),
                   "unlocked orphaned partials must be removed");
  passed &= expect(!std::filesystem::exists(payload),
                   "payload without a completion marker must be cleaned as orphaned state");

  auto const other_root = root("other-root");
  auto const other_path = unique_root();
  std::filesystem::create_directories(other_path);
  infrastructure::LocalPackageCacheStorage two_roots({
      {.root = root_id, .directory = root_path, .create_if_missing = false},
      {.root = other_root, .directory = other_path, .create_if_missing = false},
  });
  auto const switched = two_roots.observe_root(other_root);
  passed &= expect(switched.available, "a separately registered root is usable");
  std::filesystem::remove_all(other_path, cleanup_error);
  auto const unavailable = two_roots.observe_root(other_root);
  passed &= expect(!unavailable.available &&
                       two_roots.observe_root(root_id).available,
                   "an invalidated selected root must not silently fall back");

  std::filesystem::remove_all(root_path, cleanup_error);
  return passed;
}

}  // namespace

int main() {
  return adapter_contract() ? EXIT_SUCCESS : EXIT_FAILURE;
}
