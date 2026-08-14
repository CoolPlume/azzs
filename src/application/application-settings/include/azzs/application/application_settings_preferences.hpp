#pragma once

#include <memory>

#include "azzs/domain/architecture_selection.hpp"
#include "azzs/domain/offline_package_cache.hpp"

namespace azzs::application {

namespace architecture_selection {
namespace selection_domain = domain::architecture_selection;
}

namespace offline_package_cache {
namespace cache_domain = domain::offline_package_cache;
}

enum class ArchitecturePreferenceReadStatus {
  loaded,
  unavailable,
};

struct ArchitecturePreferenceRead final {
  ArchitecturePreferenceReadStatus status{
      ArchitecturePreferenceReadStatus::unavailable};
  domain::architecture_selection::ArchitecturePreference preference{
      domain::architecture_selection::ArchitecturePreference::
          prefer_arm64_prompt_fallback};
};

enum class ArchitecturePreferenceWriteStatus {
  saved,
  unavailable,
};

class ArchitecturePreferenceStore {
 public:
  virtual ~ArchitecturePreferenceStore() = default;

  [[nodiscard]] virtual ArchitecturePreferenceRead read_architecture_preference() =
      0;
  [[nodiscard]] virtual ArchitecturePreferenceWriteStatus
  write_architecture_preference(
      domain::architecture_selection::ArchitecturePreference preference) = 0;
};

class ArchitecturePreferences final {
 public:
  explicit ArchitecturePreferences(
      std::shared_ptr<ArchitecturePreferenceStore> store);

  [[nodiscard]] domain::architecture_selection::ArchitecturePreference
  preference() const noexcept;
  [[nodiscard]] domain::architecture_selection::ArchitecturePreference
  set_preference(
      domain::architecture_selection::ArchitecturePreference preference);

 private:
  std::shared_ptr<ArchitecturePreferenceStore> store_;
  domain::architecture_selection::ArchitecturePreference preference_{
      domain::architecture_selection::ArchitecturePreference::
          prefer_arm64_prompt_fallback};
};

enum class CacheRetentionPreferenceReadStatus {
  loaded,
  unavailable,
};

struct CacheRetentionPreferenceRead final {
  CacheRetentionPreferenceReadStatus status{
      CacheRetentionPreferenceReadStatus::unavailable};
  domain::offline_package_cache::CacheRetentionPolicy retention{
      domain::offline_package_cache::CacheRetentionPolicy::retain_seven_days};
};

enum class CacheRetentionPreferenceWriteStatus {
  saved,
  unavailable,
};

class CacheRetentionPreferenceStore {
 public:
  virtual ~CacheRetentionPreferenceStore() = default;

  [[nodiscard]] virtual CacheRetentionPreferenceRead read_cache_retention() =
      0;
  [[nodiscard]] virtual CacheRetentionPreferenceWriteStatus
  write_cache_retention(
      domain::offline_package_cache::CacheRetentionPolicy retention) = 0;
};

class CacheRetentionPreferences final {
 public:
  explicit CacheRetentionPreferences(
      std::shared_ptr<CacheRetentionPreferenceStore> store);

  [[nodiscard]] domain::offline_package_cache::CacheRetentionPolicy
  retention() const noexcept;
  [[nodiscard]] domain::offline_package_cache::CacheRetentionPolicy
  set_retention(domain::offline_package_cache::CacheRetentionPolicy retention);

 private:
  std::shared_ptr<CacheRetentionPreferenceStore> store_;
  domain::offline_package_cache::CacheRetentionPolicy retention_{
      domain::offline_package_cache::CacheRetentionPolicy::retain_seven_days};
};

}  // namespace azzs::application
