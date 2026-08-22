#include "azzs/adapters/windows/windows_view_preferences.hpp"

#include <cstdint>
#include <cmath>
#include <optional>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

namespace azzs::adapters::windows {
namespace {

constexpr wchar_t kAdvancedViewPreference[] = L"AzzsAdvancedView";
constexpr wchar_t kArchitecturePreference[] = L"AzzsArchitecturePreference";
constexpr wchar_t kCacheRetentionPreference[] = L"AzzsCacheRetentionPreference";
constexpr wchar_t kDebugModePreference[] = L"AzzsDebugMode";
constexpr wchar_t kSidebarWidthPreference[] = L"AzzsSidebarWidthDip";

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

[[nodiscard]] std::optional<double> read_sidebar_width_value(
    wchar_t const* key) {
  auto const value = winrt::Windows::Storage::ApplicationData::Current()
                         .LocalSettings()
                         .Values()
                         .TryLookup(key);
  auto const property_value =
      value.try_as<winrt::Windows::Foundation::IPropertyValue>();
  if (!property_value) {
    return std::nullopt;
  }
  if (property_value.Type() ==
      winrt::Windows::Foundation::PropertyType::Double) {
    return winrt::unbox_value<double>(value);
  }
  // Accept the first implementation's integral representation when upgrading
  // an existing profile, but always write the canonical Double form below.
  if (property_value.Type() ==
      winrt::Windows::Foundation::PropertyType::Int32) {
    return static_cast<double>(winrt::unbox_value<std::int32_t>(value));
  }
  return std::nullopt;
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

application::DebugModePreferenceRead WindowsViewPreferences::read_debug_mode() {
  try {
    auto const value = winrt::Windows::Storage::ApplicationData::Current()
                           .LocalSettings()
                           .Values()
                           .TryLookup(kDebugModePreference);
    auto const property_value =
        value.try_as<winrt::Windows::Foundation::IPropertyValue>();
    if (!property_value || property_value.Type() !=
                               winrt::Windows::Foundation::PropertyType::Boolean) {
      return {.status = application::DebugModePreferenceReadStatus::loaded};
    }
    return {.status = application::DebugModePreferenceReadStatus::loaded,
            .enabled = winrt::unbox_value<bool>(value)};
  } catch (...) {
    return {};
  }
}

application::DebugModePreferenceWriteStatus
WindowsViewPreferences::write_debug_mode(bool enabled) {
  try {
    winrt::Windows::Storage::ApplicationData::Current()
        .LocalSettings()
        .Values()
        .Insert(kDebugModePreference, winrt::box_value(enabled));
    return application::DebugModePreferenceWriteStatus::saved;
  } catch (...) {
    return application::DebugModePreferenceWriteStatus::unavailable;
  }
}

application::SidebarWidthPreferenceRead
WindowsViewPreferences::read_sidebar_width() {
  try {
    auto const value = read_sidebar_width_value(kSidebarWidthPreference);
    if (!value.has_value()) {
      return {.status = application::SidebarWidthPreferenceReadStatus::loaded};
    }
    auto const width = *value;
    if (!std::isfinite(width) ||
        width < application::kSidebarWidthMinimumDip ||
        width > application::kSidebarWidthMaximumDip) {
      return {.status = application::SidebarWidthPreferenceReadStatus::loaded};
    }
    return {.status = application::SidebarWidthPreferenceReadStatus::loaded,
            .width_dip = width};
  } catch (...) {
    return {};
  }
}

application::SidebarWidthPreferenceWriteStatus
WindowsViewPreferences::write_sidebar_width(double width_dip) {
  try {
    if (!std::isfinite(width_dip) ||
        width_dip < application::kSidebarWidthMinimumDip ||
        width_dip > application::kSidebarWidthMaximumDip) {
      return application::SidebarWidthPreferenceWriteStatus::unavailable;
    }
    winrt::Windows::Storage::ApplicationData::Current()
        .LocalSettings()
        .Values()
        .Insert(kSidebarWidthPreference, winrt::box_value(width_dip));
    return application::SidebarWidthPreferenceWriteStatus::saved;
  } catch (...) {
    return application::SidebarWidthPreferenceWriteStatus::unavailable;
  }
}

}  // namespace azzs::adapters::windows
