#pragma once

#include "azzs/application/advanced_view_preferences.hpp"

namespace azzs::adapters::windows {

class WindowsViewPreferences final
    : public application::AdvancedViewPreferenceStore {
 public:
  [[nodiscard]] application::AdvancedViewPreferenceRead read_advanced_view()
      override;
  [[nodiscard]] application::AdvancedViewPreferenceWriteStatus
  write_advanced_view(bool enabled) override;
};

}  // namespace azzs::adapters::windows
