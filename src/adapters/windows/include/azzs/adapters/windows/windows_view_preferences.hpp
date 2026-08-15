#pragma once

#include "azzs/application/advanced_view_preferences.hpp"
#include "azzs/application/application_settings_preferences.hpp"

namespace azzs::adapters::windows {

class WindowsViewPreferences final
    : public application::AdvancedViewPreferenceStore,
      public application::ArchitecturePreferenceStore,
      public application::CacheRetentionPreferenceStore,
      public application::DebugModePreferenceStore {
 public:
  [[nodiscard]] application::AdvancedViewPreferenceRead read_advanced_view()
      override;
  [[nodiscard]] application::AdvancedViewPreferenceWriteStatus
  write_advanced_view(bool enabled) override;
  [[nodiscard]] application::ArchitecturePreferenceRead
  read_architecture_preference() override;
  [[nodiscard]] application::ArchitecturePreferenceWriteStatus
  write_architecture_preference(
      domain::architecture_selection::ArchitecturePreference preference) override;
  [[nodiscard]] application::CacheRetentionPreferenceRead
  read_cache_retention() override;
  [[nodiscard]] application::CacheRetentionPreferenceWriteStatus
  write_cache_retention(
      domain::offline_package_cache::CacheRetentionPolicy retention) override;
  [[nodiscard]] application::DebugModePreferenceRead read_debug_mode() override;
  [[nodiscard]] application::DebugModePreferenceWriteStatus write_debug_mode(
      bool enabled) override;
};

}  // namespace azzs::adapters::windows
