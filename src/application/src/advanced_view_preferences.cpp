#include "azzs/application/advanced_view_preferences.hpp"

#include <utility>

namespace azzs::application {

AdvancedViewPreferences::AdvancedViewPreferences(
    std::shared_ptr<AdvancedViewPreferenceStore> store)
    : store_(std::move(store)) {
  if (!store_) {
    return;
  }
  auto const read = store_->read_advanced_view();
  if (read.status == AdvancedViewPreferenceReadStatus::loaded) {
    enabled_ = read.enabled;
  }
}

bool AdvancedViewPreferences::enabled() const noexcept { return enabled_; }

bool AdvancedViewPreferences::set_enabled(bool enabled) {
  if (!store_ ||
      store_->write_advanced_view(enabled) !=
          AdvancedViewPreferenceWriteStatus::saved) {
    return enabled_;
  }
  enabled_ = enabled;
  return enabled_;
}

}  // namespace azzs::application
