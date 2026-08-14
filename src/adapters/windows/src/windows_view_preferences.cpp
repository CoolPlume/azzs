#include "azzs/adapters/windows/windows_view_preferences.hpp"

#include <cstdint>
#include <optional>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

namespace azzs::adapters::windows {
namespace {

constexpr wchar_t kAdvancedViewPreference[] = L"AzzsAdvancedView";
constexpr wchar_t kArchitecturePreference[] = L"AzzsArchitecturePreference";
constexpr wchar_t kCacheRetentionPreference[] = L"AzzsCacheRetentionPreference";

[[nodiscard]] std::optional<std::int32_t> read_int32(
    wchar_t const* key) {
  auto const value = winrt::Windows::Storage::ApplicationData::Current()
                         .LocalSettings()
                         .Values()
                         .TryLookup(key);
  auto const property_value =
      value.try_as<winrt::Windows::Foundation::IPropertyValue>();
  if (!property_value || property_value.Type() !=
                             winrt::Windows::Foundation::PropertyType::Int32) {
    return std::nullopt;
  }
  return winrt::unbox_value<std::int32_t>(value);
}

}  // namespace

application::AdvancedViewPreferenceRead
WindowsViewPreferences::read_advanced_view() {
  try {
    auto const value = winrt::Windows::Storage::ApplicationData::Current()
                           .LocalSettings()
                           .Values()
                           .TryLookup(kAdvancedViewPreference);
    auto const property_value =
        value.try_as<winrt::Windows::Foundation::IPropertyValue>();
    if (!property_value || property_value.Type() !=
                               winrt::Windows::Foundation::PropertyType::Boolean) {
      return {.status = application::AdvancedViewPreferenceReadStatus::loaded};
    }
    return {.status = application::AdvancedViewPreferenceReadStatus::loaded,
            .enabled = winrt::unbox_value<bool>(value)};
  } catch (...) {
    return {};
  }
}

application::AdvancedViewPreferenceWriteStatus
WindowsViewPreferences::write_advanced_view(bool enabled) {
  try {
    winrt::Windows::Storage::ApplicationData::Current()
        .LocalSettings()
        .Values()
        .Insert(kAdvancedViewPreference, winrt::box_value(enabled));
    return application::AdvancedViewPreferenceWriteStatus::saved;
  } catch (...) {
    return application::AdvancedViewPreferenceWriteStatus::unavailable;
  }
}

application::ArchitecturePreferenceRead
WindowsViewPreferences::read_architecture_preference() {
  using Preference = domain::architecture_selection::ArchitecturePreference;
  try {
    auto const value = read_int32(kArchitecturePreference);
    if (!value.has_value() ||
        *value < static_cast<std::int32_t>(
                     Preference::prefer_arm64_prompt_fallback) ||
        *value > static_cast<std::int32_t>(Preference::prefer_x64)) {
      return {.status = application::ArchitecturePreferenceReadStatus::loaded};
    }
    return {.status = application::ArchitecturePreferenceReadStatus::loaded,
            .preference = static_cast<Preference>(*value)};
  } catch (...) {
    return {};
  }
}

application::ArchitecturePreferenceWriteStatus
WindowsViewPreferences::write_architecture_preference(
    domain::architecture_selection::ArchitecturePreference preference) {
  try {
    winrt::Windows::Storage::ApplicationData::Current()
        .LocalSettings()
        .Values()
        .Insert(kArchitecturePreference,
                winrt::box_value(static_cast<std::int32_t>(preference)));
    return application::ArchitecturePreferenceWriteStatus::saved;
  } catch (...) {
    return application::ArchitecturePreferenceWriteStatus::unavailable;
  }
}

application::CacheRetentionPreferenceRead
WindowsViewPreferences::read_cache_retention() {
  using Retention = domain::offline_package_cache::CacheRetentionPolicy;
  try {
    auto const value = read_int32(kCacheRetentionPreference);
    if (!value.has_value() ||
        *value < static_cast<std::int32_t>(Retention::delete_immediately) ||
        *value > static_cast<std::int32_t>(Retention::retain_indefinitely)) {
      return {.status = application::CacheRetentionPreferenceReadStatus::loaded};
    }
    return {.status = application::CacheRetentionPreferenceReadStatus::loaded,
            .retention = static_cast<Retention>(*value)};
  } catch (...) {
    return {};
  }
}

application::CacheRetentionPreferenceWriteStatus
WindowsViewPreferences::write_cache_retention(
    domain::offline_package_cache::CacheRetentionPolicy retention) {
  try {
    winrt::Windows::Storage::ApplicationData::Current()
        .LocalSettings()
        .Values()
        .Insert(kCacheRetentionPreference,
                winrt::box_value(static_cast<std::int32_t>(retention)));
    return application::CacheRetentionPreferenceWriteStatus::saved;
  } catch (...) {
    return application::CacheRetentionPreferenceWriteStatus::unavailable;
  }
}

}  // namespace azzs::adapters::windows
