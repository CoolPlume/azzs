#include "azzs/adapters/windows/windows_view_preferences.hpp"

#include <winrt/Windows.Storage.h>

namespace azzs::adapters::windows {
namespace {

constexpr wchar_t kAdvancedViewPreference[] = L"AzzsAdvancedView";

}  // namespace

application::AdvancedViewPreferenceRead
WindowsViewPreferences::read_advanced_view() {
  try {
    auto const value = winrt::Windows::Storage::ApplicationData::Current()
                           .LocalSettings()
                           .Values()
                           .TryLookup(kAdvancedViewPreference);
    return {.status = application::AdvancedViewPreferenceReadStatus::loaded,
            .enabled = winrt::unbox_value_or<bool>(value, false)};
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

}  // namespace azzs::adapters::windows
